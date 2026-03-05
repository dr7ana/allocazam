#pragma once

#include "types.hpp"

#include <limits>
#include <span>

namespace allocazam { namespace runner {
    template <bool CanGrow, bool CollectStats = false>
    class alignas(detail::cache_line_size) allocator {
        struct run_header {
            size_t size_and_flags;
        };

        struct free_links {
            run_header* next;
            run_header* prev;
        };

        struct chunk_node {
            std::byte* begin;
            size_t bytes;
            std::byte* raw;
            chunk_node* next;
        };

        static constexpr size_t run_alignment{alignof(std::max_align_t)};
        static constexpr size_t min_bin_shift{3};
        static constexpr size_t min_bin_bytes{size_t{1} << min_bin_shift};
        static constexpr size_t linear_bin_cutoff{4096};
        static constexpr size_t linear_bin_count{linear_bin_cutoff / min_bin_bytes};
        static constexpr size_t log_bin_count{24};
        static constexpr size_t bin_count{linear_bin_count + log_bin_count};
        static constexpr size_t bin_word_bits{std::numeric_limits<size_t>::digits};
        static constexpr size_t bin_word_count{(bin_count + bin_word_bits - 1) / bin_word_bits};
        static constexpr size_t min_free_run_size{sizeof(run_header) + sizeof(free_links) + sizeof(size_t)};
        static constexpr size_t split_remainder_threshold{64};
        static constexpr size_t epilogue_size{sizeof(run_header)};
        static constexpr size_t min_chunk_bytes{min_free_run_size + epilogue_size};

        static constexpr size_t flag_is_free{size_t{1}};
        static constexpr size_t flag_prev_is_free{size_t{2}};
        static constexpr size_t flag_mask{flag_is_free | flag_prev_is_free};
        static constexpr bool can_grow{CanGrow};
        static constexpr bool collect_stats{CollectStats};

      public:
        struct stats_t {
            size_t allocate_calls{};
            size_t allocate_success{};
            size_t allocate_fail{};
            size_t deallocate_calls{};
            size_t find_fit_calls{};
            size_t scanned_nodes{};
            size_t split_count{};
            size_t coalesce_next_count{};
            size_t coalesce_prev_count{};
            size_t grow_calls{};
            size_t peak_live_bytes{};
            size_t live_bytes{};
            size_t requested_bytes{};
            size_t granted_bytes{};
        };

        explicit allocator(size_t initial_bytes = 65536)
                : _page_size{detail::detect_page_size()}, _next_growth{std::ranges::max(initial_bytes, size_t{65536})} {
            _add_owned_chunk(initial_bytes);
        }

        explicit allocator(std::span<std::byte> backing) : _page_size{detail::detect_page_size()}, _next_growth{0} {
            _add_external_chunk(backing);
        }

        ~allocator() { _release_owned_chunks(); }

        allocator(const allocator&) = delete;
        allocator& operator=(const allocator&) = delete;
        allocator(allocator&&) = delete;
        allocator& operator=(allocator&&) = delete;

        [[nodiscard]] constexpr const stats_t& stats() const noexcept { return _stats; }
        constexpr void reset_stats() noexcept { _stats = {}; }

        void* allocate_bytes(size_t bytes, size_t alignment) {
            if (bytes == 0) {
                bytes = 1;
            }
            if (alignment > run_alignment) {
                return nullptr;
            }

            if constexpr (collect_stats) {
                ++_stats.allocate_calls;
                _stats.requested_bytes += bytes;
            }

            size_t payload = _align_up_pow2(bytes, run_alignment);
            size_t needed = _align_up_pow2(sizeof(run_header) + payload, run_alignment);
            needed = std::ranges::max(needed, min_free_run_size);
            size_t scan_count = 0;
            run_header* h = _find_fit(needed, scan_count);
            if (h == nullptr) {
                if constexpr (can_grow) {
                    _grow_for(needed);
                    h = _find_fit(needed, scan_count);
                }
                if (h == nullptr) {
                    if constexpr (collect_stats) {
                        ++_stats.allocate_fail;
                    }
                    return nullptr;
                }
            }
            void* out = _allocate_from_free_run(h, needed);
            if constexpr (collect_stats) {
                ++_stats.allocate_success;
                size_t granted = _run_size(_header_from_payload(out)) - sizeof(run_header);
                _stats.granted_bytes += granted;
            }
            return out;
        }

        void deallocate_bytes(void* p) noexcept {
            if (p == nullptr) {
                return;
            }

            if constexpr (collect_stats) {
                ++_stats.deallocate_calls;
            }

            run_header* current = _header_from_payload(p);
            size_t current_size = _run_size(current);
            if constexpr (collect_stats) {
                size_t payload = current_size - sizeof(run_header);
                _stats.live_bytes -= payload;
            }

            run_header* next = _next_run(current);
            if (next != nullptr && _is_free(next)) {
                _unlink_free(next);
                current_size += _run_size(next);
                if constexpr (collect_stats) {
                    ++_stats.coalesce_next_count;
                }
            }

            if (_prev_is_free(current)) {
                run_header* prev = _prev_run(current);
                if (prev != nullptr && _is_free(prev)) {
                    _unlink_free(prev);
                    current = prev;
                    current_size += _run_size(prev);
                    if constexpr (collect_stats) {
                        ++_stats.coalesce_prev_count;
                    }
                }
            }

            _set_header(current, current_size, true, _prev_is_free(current));
            _write_footer(current);

            run_header* next_after = _next_run(current);
            if (next_after != nullptr) {
                _set_prev_free(next_after, true);
            }

            _link_free(current);
        }

      private:
        static constexpr size_t _align_up_pow2(size_t value, size_t alignment) noexcept {
            static_assert(std::has_single_bit(run_alignment));
            size_t mask = alignment - 1;
            return (value + mask) & ~mask;
        }

        /*
         * Hybrid binning:
         * - linear 8-byte bins up to 2 KiB remove start-bin scans for small/medium requests
         * - logarithmic bins above 2 KiB keep metadata compact for large runs
         * - non-empty tracking uses a multiword bitmask, so lookup still jumps with countr_zero
         */
        static constexpr size_t _bin_word_index(size_t idx) noexcept { return idx / bin_word_bits; }

        static constexpr size_t _bin_word_bit(size_t idx) noexcept { return size_t{1} << (idx % bin_word_bits); }

        static constexpr std::byte* _align_up_ptr(std::byte* p, size_t alignment) noexcept {
            auto value = reinterpret_cast<uintptr_t>(p);
            auto aligned = _align_up_pow2(static_cast<size_t>(value), alignment);
            return reinterpret_cast<std::byte*>(aligned);
        }

        static constexpr size_t _bin_index_for(size_t bytes) noexcept {
            size_t aligned = _align_up_pow2(bytes | size_t{1}, min_bin_bytes);
            if (aligned <= linear_bin_cutoff) {
                return (aligned >> min_bin_shift) - 1;
            }

            size_t log_idx = std::bit_width(aligned - 1) - std::bit_width(linear_bin_cutoff);
            return std::min(linear_bin_count + log_idx, bin_count - 1);
        }

        static constexpr size_t _size_field(size_t size_and_flags) noexcept { return size_and_flags & ~flag_mask; }

        static constexpr bool _flag(size_t size_and_flags, size_t bit) noexcept { return (size_and_flags & bit) != 0; }

        static constexpr size_t _pack(size_t bytes, bool is_free, bool prev_is_free) noexcept {
            return bytes | (is_free ? flag_is_free : 0) | (prev_is_free ? flag_prev_is_free : 0);
        }

        static constexpr run_header* _header_from_payload(void* p) noexcept {
            auto* raw = static_cast<std::byte*>(p);
            return reinterpret_cast<run_header*>(raw - sizeof(run_header));
        }

        static constexpr std::byte* _payload_from_header(run_header* h) noexcept {
            return reinterpret_cast<std::byte*>(h) + sizeof(run_header);
        }

        static constexpr free_links* _links(run_header* h) noexcept {
            return reinterpret_cast<free_links*>(_payload_from_header(h));
        }

        static constexpr size_t* _footer_ptr(run_header* h) noexcept {
            size_t bytes = _size_field(h->size_and_flags);
            return reinterpret_cast<size_t*>(reinterpret_cast<std::byte*>(h) + bytes - sizeof(size_t));
        }

        static constexpr size_t _run_size(const run_header* h) noexcept { return _size_field(h->size_and_flags); }

        static constexpr bool _is_free(const run_header* h) noexcept { return _flag(h->size_and_flags, flag_is_free); }

        static constexpr bool _prev_is_free(const run_header* h) noexcept {
            return _flag(h->size_and_flags, flag_prev_is_free);
        }

        static constexpr void _set_header(run_header* h, size_t bytes, bool is_free, bool prev_is_free) noexcept {
            h->size_and_flags = _pack(bytes, is_free, prev_is_free);
        }

        static constexpr void _set_prev_free(run_header* h, bool prev_is_free) noexcept {
            if (_prev_is_free(h) == prev_is_free) {
                return;
            }

            if (prev_is_free) {
                h->size_and_flags |= flag_prev_is_free;
            } else {
                h->size_and_flags &= ~flag_prev_is_free;
            }
        }

        static constexpr run_header* _next_run(run_header* h) noexcept {
            auto* next = reinterpret_cast<run_header*>(reinterpret_cast<std::byte*>(h) + _run_size(h));
            if (_run_size(next) == 0) {
                return nullptr;
            }
            return next;
        }

        static constexpr run_header* _prev_run(run_header* h) noexcept {
            auto* footer = reinterpret_cast<size_t*>(reinterpret_cast<std::byte*>(h) - sizeof(size_t));
            size_t prev_size = *footer;
            if (prev_size == 0) {
                return nullptr;
            }
            return reinterpret_cast<run_header*>(reinterpret_cast<std::byte*>(h) - prev_size);
        }

        constexpr void _write_footer(run_header* h) noexcept { *_footer_ptr(h) = _run_size(h); }

        constexpr void _set_bin_nonempty(size_t idx) noexcept {
            _bin_nonempty_mask[_bin_word_index(idx)] |= _bin_word_bit(idx);
        }

        constexpr void _set_bin_empty(size_t idx) noexcept {
            _bin_nonempty_mask[_bin_word_index(idx)] &= ~_bin_word_bit(idx);
        }

        constexpr size_t _first_nonempty_bin_from(size_t start_idx) const noexcept {
            if (start_idx >= bin_count) {
                return bin_count;
            }

            size_t word_idx = _bin_word_index(start_idx);
            size_t bit_idx = start_idx % bin_word_bits;
            size_t word = _bin_nonempty_mask[word_idx] & (~size_t{} << bit_idx);

            while (true) {
                if (word != 0) {
                    size_t idx = word_idx * bin_word_bits + std::countr_zero(word);
                    return idx < bin_count ? idx : bin_count;
                }

                ++word_idx;
                if (word_idx >= bin_word_count) {
                    return bin_count;
                }
                word = _bin_nonempty_mask[word_idx];
            }
        }

        constexpr void _link_free(run_header* h) noexcept {
            assert(_is_free(h));
            size_t idx = _bin_index_for(_run_size(h));
            free_links* l = _links(h);
            l->prev = nullptr;
            l->next = _bins[idx];
            if (_bins[idx] != nullptr) {
                _links(_bins[idx])->prev = h;
            }
            _bins[idx] = h;
            _set_bin_nonempty(idx);
        }

        constexpr void _unlink_free(run_header* h) noexcept {
            size_t idx = _bin_index_for(_run_size(h));
            free_links* l = _links(h);
            if (l->prev != nullptr) {
                _links(l->prev)->next = l->next;
            } else {
                _bins[idx] = l->next;
            }

            if (l->next != nullptr) {
                _links(l->next)->prev = l->prev;
            }

            if (_bins[idx] == nullptr) {
                _set_bin_empty(idx);
            }
        }

        constexpr run_header* _find_fit(size_t needed, [[maybe_unused]] size_t& scan_count) noexcept {
            if constexpr (collect_stats) {
                ++_stats.find_fit_calls;
            }

            size_t start_bin = _bin_index_for(needed);
            for (size_t idx = _first_nonempty_bin_from(start_bin); idx < bin_count;
                 idx = _first_nonempty_bin_from(idx + 1)) {
                if (idx != start_bin || idx < linear_bin_count) {
                    if constexpr (collect_stats) {
                        ++scan_count;
                    }
                    if constexpr (collect_stats) {
                        _stats.scanned_nodes += scan_count;
                    }
                    return _bins[idx];
                }

                for (run_header* h = _bins[idx]; h != nullptr; h = _links(h)->next) {
                    if constexpr (collect_stats) {
                        ++scan_count;
                    }
                    if (_run_size(h) >= needed) {
                        if constexpr (collect_stats) {
                            _stats.scanned_nodes += scan_count;
                        }
                        return h;
                    }
                }
            }

            if constexpr (collect_stats) {
                _stats.scanned_nodes += scan_count;
            }
            return nullptr;
        }

        constexpr void* _allocate_from_free_run(run_header* h, size_t needed) noexcept {
            size_t old_size = _run_size(h);
            bool prev_free = _prev_is_free(h);
            size_t rem_size = old_size - needed;

            _unlink_free(h);

            if (rem_size >= (min_free_run_size + split_remainder_threshold)) {
                _set_header(h, needed, false, prev_free);
                if constexpr (collect_stats) {
                    ++_stats.split_count;
                }

                auto* rem = reinterpret_cast<run_header*>(reinterpret_cast<std::byte*>(h) + needed);
                _set_header(rem, rem_size, true, false);
                _write_footer(rem);
                _link_free(rem);

                run_header* next_after_rem = _next_run(rem);
                if (next_after_rem != nullptr) {
                    _set_prev_free(next_after_rem, true);
                }
            } else {
                _set_header(h, old_size, false, prev_free);
                run_header* next = _next_run(h);
                if (next != nullptr) {
                    _set_prev_free(next, false);
                }
            }

            if constexpr (collect_stats) {
                size_t payload = _run_size(h) - sizeof(run_header);
                _stats.live_bytes += payload;
                if (_stats.live_bytes > _stats.peak_live_bytes) {
                    _stats.peak_live_bytes = _stats.live_bytes;
                }
            }

            return static_cast<void*>(_payload_from_header(h));
        }

        void _grow_for(size_t needed)
            requires(can_grow)
        {
            if constexpr (collect_stats) {
                ++_stats.grow_calls;
            }
            size_t chunk_bytes = std::ranges::max(_next_growth, needed + min_chunk_bytes);
            _add_owned_chunk(chunk_bytes);
            _next_growth = std::ranges::max(_next_growth * 2, chunk_bytes);
        }

        void _initialize_free_run(chunk_node* c) {
            auto* h = reinterpret_cast<run_header*>(c->begin);
            size_t run_bytes = c->bytes - epilogue_size;
            _set_header(h, run_bytes, true, false);
            _write_footer(h);
            _link_free(h);

            auto* epilogue = reinterpret_cast<run_header*>(c->begin + run_bytes);
            _set_header(epilogue, 0, false, true);
        }

        void _add_owned_chunk(size_t bytes) {
            size_t chunk_bytes = detail::round_to_multiple_of(std::ranges::max(bytes, min_chunk_bytes), _page_size);

            size_t total_bytes = sizeof(chunk_node) + run_alignment - 1 + chunk_bytes;
            auto* raw = static_cast<std::byte*>(::operator new[](total_bytes, std::align_val_t{run_alignment}));
            auto* c = reinterpret_cast<chunk_node*>(raw);
            std::byte* begin = _align_up_ptr(raw + sizeof(chunk_node), run_alignment);

            c->begin = begin;
            c->bytes = chunk_bytes;
            c->raw = raw;
            c->next = _chunks_head;
            _chunks_head = c;

            _initialize_free_run(c);
        }

        void _add_external_chunk(std::span<std::byte> backing) {
            if (backing.empty()) {
                throw std::invalid_argument{"runner::allocator backing must not be empty"};
            }

            void* aligned = backing.data();
            size_t space = backing.size();
            if (std::align(run_alignment, min_chunk_bytes, aligned, space) == nullptr) {
                throw std::invalid_argument{"runner::allocator backing alignment is insufficient"};
            }

            size_t chunk_bytes = space - (space % run_alignment);
            if (chunk_bytes < min_chunk_bytes) {
                throw std::invalid_argument{"runner::allocator backing too small"};
            }

            _external_chunk.begin = static_cast<std::byte*>(aligned);
            _external_chunk.bytes = chunk_bytes;
            _external_chunk.raw = nullptr;
            _external_chunk.next = _chunks_head;
            _chunks_head = &_external_chunk;

            _initialize_free_run(&_external_chunk);
        }

        void _release_owned_chunks() noexcept {
            for (chunk_node* c = _chunks_head; c != nullptr;) {
                chunk_node* next = c->next;
                if (c->raw != nullptr) {
                    ::operator delete[](c->raw, std::align_val_t{run_alignment});
                }
                c = next;
            }
        }

        std::array<run_header*, bin_count> _bins{};
        std::array<size_t, bin_word_count> _bin_nonempty_mask{};
        chunk_node* _chunks_head{nullptr};
        chunk_node _external_chunk{};
        size_t _page_size{4096};
        size_t _next_growth{65536};
        stats_t _stats{};
    };
}}  // namespace allocazam::runner
