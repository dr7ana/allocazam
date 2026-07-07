#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace allocazam {
    //
    enum class memory_mode : uint8_t { fixed, dynamic, noheap };
    enum class huge_pages : uint8_t { disabled, enabled };

    // defers the first chunk to first allocation. default-created thread states use this
    // so a state that is never allocated from never maps memory; explicit states stay
    // eager — users sized them deliberately, and eager is the fixed-mode capacity contract
    struct lazy_init_t {};
    inline constexpr lazy_init_t lazy_init{};

    namespace detail {

        inline constexpr size_t round_to_multiple_of(size_t value, size_t multiple) noexcept {
            size_t rem = value % multiple;
            if (rem == 0) {
                return value;
            }

            return value + (multiple - rem);
        }

        inline constexpr bool checked_add(size_t lhs, size_t rhs, size_t& out) noexcept {
            if (lhs > (std::numeric_limits<size_t>::max() - rhs)) {
                return false;
            }
            out = lhs + rhs;
            return true;
        }

        inline constexpr bool checked_mul(size_t lhs, size_t rhs, size_t& out) noexcept {
            if (lhs == 0 || rhs == 0) {
                out = 0;
                return true;
            }
            if (lhs > (std::numeric_limits<size_t>::max() / rhs)) {
                return false;
            }
            out = lhs * rhs;
            return true;
        }

        inline constexpr bool checked_round_to_multiple_of(size_t value, size_t multiple, size_t& out) noexcept {
            if (multiple == 0) {
                return false;
            }
            size_t rem = value % multiple;
            if (rem == 0) {
                out = value;
                return true;
            }
            return checked_add(value, multiple - rem, out);
        }

#if defined(__cpp_lib_hardware_interference_size)
        inline constexpr size_t cache_line_size = [] {
            size_t s = std::hardware_destructive_interference_size;
            return std::has_single_bit(s) ? s : size_t{64};
        }();
#else
        inline constexpr size_t cache_line_size = 64;
#endif

        size_t detect_page_size() noexcept;
    }  // namespace detail

    template <typename T>
    struct node_t {
        static constexpr size_t node_size{std::ranges::max(sizeof(T), sizeof(void*))};
        static constexpr size_t node_alignment{std::ranges::max(alignof(T), alignof(void*))};

        alignas(node_alignment) std::array<std::byte, node_size> storage;
    };

    // sits at every granule base of an OwnerHeaders chunk; resolved from any node pointer
    // by masking with ~(granule - 1). 16 bytes in all build modes (layout stability); the
    // tag is consulted only by debug asserts — never by release-mode routing decisions
    // (user payloads are user-controlled bytes; collisions are legitimate)
    struct granule_header {
        void* owner;
        size_t debug_tag;
    };

    static_assert(sizeof(granule_header) == 16);

    inline constexpr size_t granule_debug_tag{0xa110ca2a'6a121e57};

    // OwnerHeaders selects the granule-header layout (owner pointer resolvable by masking
    // any node pointer); false keeps today's raw layout byte-identical
    template <typename T, bool owns_memory = true, bool OwnerHeaders = false>
    struct chunk_t {
        static_assert(!OwnerHeaders || owns_memory, "granule headers require owned (alignable) backing");

        using node = node_t<T>;

        // granule base holds the header, first node starts at the next node-aligned offset
        static constexpr size_t header_offset{std::ranges::max(sizeof(granule_header), node_t<T>::node_alignment)};

        [[nodiscard]] static constexpr size_t granule_for(size_t page_size) noexcept
            requires(OwnerHeaders)
        {
            return std::ranges::max(page_size, std::bit_ceil(sizeof(node) + header_offset));
        }

        [[nodiscard]] static constexpr size_t nodes_per_granule(size_t granule) noexcept
            requires(OwnerHeaders)
        {
            return (granule - header_offset) / sizeof(node);
        }

        constexpr chunk_t() = default;

        constexpr explicit chunk_t(size_t sz, size_t page_size, size_t alloc_alignment)
            requires(owns_memory)
        {
            if constexpr (OwnerHeaders) {
                (void)alloc_alignment;
                size_t granule = granule_for(page_size);
                size_t per_granule = nodes_per_granule(granule);
                assert(per_granule >= 1 && "granule must hold at least one node");

                size_t granule_count = 0;
                if (!detail::checked_add(sz, per_granule - 1, granule_count)) {
                    throw std::bad_array_new_length{};
                }
                granule_count /= per_granule;
                size_t alloc_bytes = 0;
                if (!detail::checked_mul(granule_count, granule, alloc_bytes)) {
                    throw std::bad_array_new_length{};
                }
                // capacity is a contract, not a rounding artifact: the tail slots of the
                // last granule stay unseeded (never enter the free list, rejected by
                // contains()) so fixed-mode budgets hold exactly
                count = sz;

                void* raw = ::operator new[](alloc_bytes, std::align_val_t{granule});
                nodes = static_cast<node*>(raw);
                alloc_align = granule;

                auto* base = reinterpret_cast<std::byte*>(raw);
                std::ranges::for_each(std::views::iota(size_t{0}, granule_count), [&](size_t g) {
                    auto* header = reinterpret_cast<granule_header*>(base + (g * granule));
                    header->owner = nullptr;
                    header->debug_tag = granule_debug_tag;
                    std::uninitialized_default_construct_n(
                            reinterpret_cast<node*>(base + (g * granule) + header_offset), per_granule);
                });
            } else {
                count = sz;
                alloc_align = alloc_alignment;

                size_t node_bytes = 0;
                if (!detail::checked_mul(count, sizeof(node), node_bytes)) {
                    throw std::bad_array_new_length{};
                }

                size_t alloc_bytes = 0;
                if (!detail::checked_round_to_multiple_of(node_bytes, page_size, alloc_bytes)) {
                    throw std::bad_array_new_length{};
                }

                void* raw = ::operator new[](alloc_bytes, std::align_val_t{alloc_align});
                nodes = static_cast<node*>(raw);
                std::uninitialized_default_construct_n(nodes, count);
            }
        }

        constexpr explicit chunk_t(node* external_nodes, size_t sz)
            requires(!owns_memory)
                : count{sz}, nodes{external_nodes} {}

        constexpr ~chunk_t() { release(); }

        chunk_t(const chunk_t&) = delete;
        chunk_t& operator=(const chunk_t&) = delete;

        constexpr chunk_t(chunk_t&& other) noexcept
                : count(std::exchange(other.count, 0)),
                  alloc_align(std::exchange(other.alloc_align, alignof(node))),
                  nodes(std::exchange(other.nodes, nullptr)) {}

        constexpr chunk_t& operator=(chunk_t&& other) noexcept {
            if (this == &other) {
                return *this;
            }

            release();
            count = std::exchange(other.count, 0);
            alloc_align = std::exchange(other.alloc_align, alignof(node));
            nodes = std::exchange(other.nodes, nullptr);
            return *this;
        }

        constexpr auto at(this auto&& self, size_t i) noexcept {
            using self_t = std::remove_reference_t<decltype(self)>;
            using ptr_t = std::conditional_t<std::is_const_v<self_t>, const node*, node*>;
            using byte_t = std::conditional_t<std::is_const_v<self_t>, const std::byte, std::byte>;
            if constexpr (OwnerHeaders) {
                size_t granule = self.alloc_align;
                size_t per_granule = nodes_per_granule(granule);
                auto* base = reinterpret_cast<byte_t*>(self.nodes);
                return reinterpret_cast<ptr_t>(
                        base + ((i / per_granule) * granule) + header_offset + ((i % per_granule) * sizeof(node)));
            } else {
                return static_cast<ptr_t>(self.nodes + i);
            }
        }

        [[nodiscard]] constexpr bool contains(const node* target) const noexcept {
            if (nodes == nullptr) {
                return false;
            }
            if constexpr (OwnerHeaders) {
                size_t granule = alloc_align;
                size_t per_granule = nodes_per_granule(granule);
                const auto* base = reinterpret_cast<const std::byte*>(nodes);
                const auto* b = reinterpret_cast<const std::byte*>(target);
                size_t granule_count = (count + per_granule - 1) / per_granule;
                size_t total_bytes = granule_count * granule;
                if (b < base || b >= (base + total_bytes)) {
                    return false;
                }
                size_t delta = static_cast<size_t>(b - base);
                size_t offset = delta % granule;
                if (offset < header_offset || ((offset - header_offset) % sizeof(node)) != 0) {
                    return false;
                }
                size_t idx = (offset - header_offset) / sizeof(node);
                if (idx >= per_granule) {
                    return false;
                }
                // slot bound rejects the unseeded tail of the last granule
                return ((delta / granule) * per_granule) + idx < count;
            } else {
                return target >= nodes && target < (nodes + count);
            }
        }

        constexpr void stamp_owner(void* owner) noexcept
            requires(OwnerHeaders)
        {
            if (nodes == nullptr) {
                return;
            }
            size_t granule = alloc_align;
            size_t per_granule = nodes_per_granule(granule);
            size_t granule_count = (count + per_granule - 1) / per_granule;
            auto* base = reinterpret_cast<std::byte*>(nodes);
            std::ranges::for_each(std::views::iota(size_t{0}, granule_count), [&](size_t g) {
                reinterpret_cast<granule_header*>(base + (g * granule))->owner = owner;
            });
        }

        constexpr void release() noexcept {
            if (nodes == nullptr) {
                return;
            }
            if constexpr (owns_memory) {
                if constexpr (!std::is_trivially_destructible_v<node>) {
                    static_assert(!OwnerHeaders, "granule-local node destruction not implemented");
                    std::destroy_n(nodes, count);
                }
                ::operator delete[](nodes, std::align_val_t{alloc_align});
            }
            nodes = nullptr;
            count = 0;
        }

        size_t count{};
        size_t alloc_align{alignof(node)};
        node* nodes{nullptr};
    };

}  // namespace allocazam
