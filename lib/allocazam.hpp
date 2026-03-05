#pragma once

#include "runner.hpp"
#include "types.hpp"

#include <span>

namespace allocazam {
    //
    template <memory_mode Mode>
    concept fixed_mode = Mode == memory_mode::fixed;

    template <memory_mode Mode>
    concept dynamic_mode = Mode == memory_mode::dynamic;

    template <memory_mode Mode>
    concept noheap_mode = Mode == memory_mode::noheap;

    template <memory_mode Mode>
    concept heap_backed_mode = fixed_mode<Mode> || dynamic_mode<Mode>;

    template <memory_mode Mode>
    concept fixed_like_mode = fixed_mode<Mode> || noheap_mode<Mode>;

    template <typename T, memory_mode Mode = memory_mode::fixed>
    class allocazam {
        using node = node_t<T>;
        using chunk = chunk_t<T, heap_backed_mode<Mode>>;

        static constexpr size_t node_alignment{alignof(node)};
        static constexpr size_t node_size{sizeof(node)};

        using chunk_buffer = std::conditional_t<dynamic_mode<Mode>, std::vector<chunk>, std::array<chunk, 1>>;

      public:
        constexpr explicit allocazam(size_t pool_size = 4096)
            requires(heap_backed_mode<Mode>)
                : _page_size{detail::detect_page_size()},
                  _nodes_per_page{_page_node_capacity(_page_size)},
                  _alloc_align{_page_alloc_align(_page_size)} {
            if (!std::has_single_bit(pool_size)) {
                throw std::invalid_argument{"allocator buffer size must be a power of 2"};
            }

            if constexpr (dynamic_mode<Mode>) {
                _chunks.reserve(4);
            }

            _next_growth = pool_size;
            _add_chunk(_next_growth);
        }

        constexpr explicit allocazam(std::span<std::byte> backing)
            requires(noheap_mode<Mode>)
        {
            if (backing.empty()) {
                throw std::invalid_argument{"allocator backing buffer must not be empty"};
            }

            void* aligned = backing.data();
            size_t space = backing.size();
            if (std::align(node_alignment, node_size, aligned, space) == nullptr) {
                throw std::invalid_argument{"allocator backing buffer cannot satisfy node alignment"};
            }

            size_t node_count = space / node_size;
            if (node_count == 0) {
                throw std::invalid_argument{"allocator backing buffer too small for one node"};
            }

            _add_external_chunk(static_cast<node*>(aligned), node_count);
        }

        constexpr ~allocazam() {
            // compiled out in release mode
            assert(_size == 0 && "outstanding objects at pool destruction");
        }

        allocazam(const allocazam&) = delete;
        allocazam& operator=(const allocazam&) = delete;
        allocazam(allocazam&&) noexcept = default;
        allocazam& operator=(allocazam&&) noexcept = default;

        constexpr T* allocate() {
            node* n = _pop_free_node();
            if (n == nullptr) {
                if constexpr (fixed_like_mode<Mode>) {
                    return nullptr;
                } else {
                    _grow();
                    n = _pop_free_node();
                    if (n == nullptr) {
                        throw std::bad_alloc{};
                    }
                }
            }

            ++_size;
            return std::launder(_value_ptr(n));
        }

        template <typename... args_t>
        constexpr T* construct(T* ptr, args_t&&... args) {
            assert(ptr != nullptr && "construct pointer must not be null");
            assert(_owns_pointer(ptr) && "pointer does not belong to this pool");
            assert(!_is_in_free_list(ptr) && "cannot construct into a free node");
            T* obj = std::construct_at(ptr, std::forward<args_t>(args)...);
            return std::launder(obj);
        }

        constexpr void destroy(T* ptr) noexcept {
            if (ptr == nullptr) {
                return;
            }

            assert(_owns_pointer(ptr) && "pointer does not belong to this pool");
            assert(!_is_in_free_list(ptr) && "double free detected");
            std::destroy_at(std::launder(ptr));
        }

        constexpr void deallocate(T* ptr) noexcept {
            if (ptr == nullptr) {
                return;
            }

            assert(_owns_pointer(ptr) && "pointer does not belong to this pool");
            assert(!_is_in_free_list(ptr) && "double free detected");

            _push_free_node(_node_from_value(ptr));
            --_size;
        }

        [[nodiscard]] static constexpr memory_mode mode() noexcept { return Mode; }
        [[nodiscard]] constexpr size_t size() const noexcept { return _size; }
        [[nodiscard]] constexpr size_t capacity() const noexcept { return _capacity; }
        [[nodiscard]] constexpr size_t free_count() const noexcept { return _capacity - _size; }

        friend constexpr bool operator==(const allocazam&, const allocazam&) { return true; }
        friend constexpr bool operator!=(const allocazam&, const allocazam&) { return false; }

      private:
        static constexpr T* _value_ptr(node* n) noexcept { return reinterpret_cast<T*>(n->storage.data()); }

        static constexpr node* _node_from_value(T* ptr) noexcept {
            auto* raw = reinterpret_cast<std::byte*>(ptr);
            return reinterpret_cast<node*>(raw - offsetof(node, storage));
        }

        static constexpr node** _next_ptr(node* n) noexcept { return reinterpret_cast<node**>(n->storage.data()); }

        static constexpr const node* _node_from_bytes(const std::byte* p) noexcept {
            return reinterpret_cast<const node*>(p - offsetof(node, storage));
        }

        static constexpr size_t _page_alloc_align(size_t page_size) noexcept {
            if (std::has_single_bit(page_size) && page_size > node_alignment) {
                return page_size;
            }
            return node_alignment;
        }

        static constexpr size_t _page_node_capacity(size_t page_size) noexcept {
            size_t n = page_size / node_size;
            return n == 0 ? 1 : n;
        }

        constexpr void _push_free_node(node* n) noexcept {
            std::construct_at(_next_ptr(n), _free_head);
            _free_head = n;
        }

        constexpr node* _pop_free_node() noexcept {
            if (_free_head == nullptr) {
                return nullptr;
            }

            node* n = _free_head;
            _free_head = *std::launder(_next_ptr(n));
            return n;
        }

        constexpr void _add_chunk(size_t slot_count)
            requires(heap_backed_mode<Mode>)
        {
            chunk* c = nullptr;
            if constexpr (fixed_mode<Mode>) {
                assert(_capacity == 0 && "fixed mode can only create one chunk");
                _chunks[0] = chunk(slot_count, _page_size, _alloc_align);
                c = &_chunks[0];
            } else {
                _chunks.emplace_back(slot_count, _page_size, _alloc_align);
                c = &_chunks.back();
            }

            for (size_t i : std::ranges::iota_view{size_t{0}, c->count}) {
                _push_free_node(c->at(i));
            }

            _capacity += c->count;
        }

        constexpr void _add_external_chunk(node* nodes, size_t slot_count)
            requires(noheap_mode<Mode>)
        {
            assert(_capacity == 0 && "noheap mode can only create one chunk");
            _chunks[0] = chunk(nodes, slot_count);
            chunk* c = &_chunks[0];

            for (size_t i : std::ranges::iota_view{size_t{0}, c->count}) {
                _push_free_node(c->at(i));
            }

            _capacity += c->count;
        }

        constexpr void _grow()
            requires(dynamic_mode<Mode>)
        {
            _next_growth = std::ranges::max(static_cast<size_t>(1), _next_growth * 2);
            _add_chunk(_next_growth);
        }

        constexpr bool _owns_pointer(const T* ptr) const noexcept {
            auto* target_node = _node_from_bytes(reinterpret_cast<const std::byte*>(ptr));

            for (const auto& c : _chunks) {
                if (c.nodes == nullptr) {
                    continue;
                }
                if (target_node >= c.nodes && target_node < (c.nodes + c.count)) {
                    return true;
                }
            }
            return false;
        }

        constexpr bool _is_in_free_list(const T* ptr) const noexcept {
            auto* target_node = _node_from_bytes(reinterpret_cast<const std::byte*>(ptr));

            for (node* n = _free_head; n != nullptr; n = *std::launder(_next_ptr(n))) {
                if (n == target_node) {
                    return true;
                }
            }
            return false;
        }

        size_t _size{};
        size_t _capacity{};
        size_t _next_growth{};

        size_t _page_size{4096};
        size_t _nodes_per_page{1};
        size_t _alloc_align{node_alignment};

        node* _free_head{nullptr};
        chunk_buffer _chunks{};
    };

    template <typename T>
    using fixed_allocazam = allocazam<T, memory_mode::fixed>;

    template <typename T>
    using dynamic_allocazam = allocazam<T, memory_mode::dynamic>;

    template <typename T>
    using noheap_allocazam = allocazam<T, memory_mode::noheap>;

    template <typename T, memory_mode Mode = memory_mode::dynamic>
    struct allocazam_std_state;

    template <typename T>
    struct alignas(detail::cache_line_size) allocazam_std_state<T, memory_mode::fixed> {
        allocazam<T, memory_mode::fixed> pool;
        runner::allocator<false> runs;

        explicit allocazam_std_state(size_t pool_size = 4096)
                : pool(pool_size), runs(std::ranges::max(pool_size * sizeof(T), size_t{4096})) {}
    };

    template <typename T>
    struct alignas(detail::cache_line_size) allocazam_std_state<T, memory_mode::dynamic> {
        allocazam<T, memory_mode::dynamic> pool;
        runner::allocator<true> runs;

        explicit allocazam_std_state(size_t pool_size = 4096, size_t runner_bytes = 65536)
                : pool(pool_size), runs(runner_bytes) {}
    };

    template <typename T>
    struct alignas(detail::cache_line_size) allocazam_std_state<T, memory_mode::noheap> {
        allocazam<T, memory_mode::noheap> pool;
        runner::allocator<false> runs;

        explicit allocazam_std_state(std::span<std::byte> node_backing, std::span<std::byte> run_backing)
                : pool(node_backing), runs(run_backing) {}

        explicit allocazam_std_state(std::span<std::byte> backing)
                : allocazam_std_state(
                          backing.first(backing.size() / 2),
                          backing.subspan(backing.size() / 2, backing.size() - (backing.size() / 2))) {}
    };

    template <typename T, memory_mode Mode = memory_mode::dynamic>
    class allocazam_std_allocator {
      public:
        using value_type = T;
        using state_type = allocazam_std_state<T, Mode>;
        using runs_type = std::conditional_t<dynamic_mode<Mode>, runner::allocator<true>, runner::allocator<false>>;
        static constexpr size_t linear_cache_cutoff = 4096;

        using propagate_on_container_copy_assignment = std::true_type;
        using propagate_on_container_move_assignment = std::true_type;
        using propagate_on_container_swap = std::true_type;
        using is_always_equal = std::false_type;

#if defined(__cpp_lib_allocate_at_least)
        using allocate_at_least_result = std::allocation_result<T*>;
#else
        struct allocate_at_least_result {
            T* ptr;
            size_t count;
        };
#endif

        template <typename U>
        struct rebind {
            using other = allocazam_std_allocator<U, Mode>;
        };

        constexpr allocazam_std_allocator()
            requires(heap_backed_mode<Mode>)
                : _state{&_default_state()}, _runs_override{nullptr}, _tls_enabled{true} {}

        constexpr explicit allocazam_std_allocator(state_type& state) noexcept
                : _state{&state}, _runs_override{nullptr}, _tls_enabled{false} {}

        template <typename U>
        constexpr allocazam_std_allocator(const allocazam_std_allocator<U, Mode>& other) noexcept
                : _state{nullptr}, _runs_override{other._runs_ptr()}, _tls_enabled{false} {
            assert(_runs_override != nullptr && "rebind source allocator must be initialized");
        }

        template <typename U>
        constexpr allocazam_std_allocator(const allocazam_std_allocator<U, Mode>&, state_type& state) noexcept
                : _state{&state}, _runs_override{nullptr}, _tls_enabled{false} {}

        [[nodiscard]] T* allocate(size_t n) {
            assert(_has_allocation_resource() && "allocator state must be initialized");

            if (n == 0) {
                return nullptr;
            }

            if (n > max_size()) {
                throw std::bad_array_new_length{};
            }

            if (n == 1) [[likely]] {
                if (_state != nullptr) {
                    T* p = _state->pool.allocate();
                    if (p == nullptr) {
                        throw std::bad_alloc{};
                    }
                    return p;
                }

                void* raw = _runs().allocate_bytes(sizeof(T), alignof(T));
                if (raw == nullptr) {
                    throw std::bad_alloc{};
                }
                return static_cast<T*>(raw);
            }

            size_t bytes = n * sizeof(T);

            if (_can_use_tls_run_cache(bytes)) [[likely]] {
                T* p = _allocate_from_tls_run_cache(bytes);
                if (p != nullptr) {
                    return p;
                }
            }

            void* raw = _runs().allocate_bytes(bytes, alignof(T));
            if (raw == nullptr) {
                throw std::bad_alloc{};
            }
            return static_cast<T*>(raw);
        }

        [[nodiscard]] allocate_at_least_result allocate_at_least(size_t n) {
            if (n == 0) {
                return {nullptr, 0};
            }

            size_t count = n;
            if constexpr (dynamic_mode<Mode>) {
                if (n > 1) {
                    size_t extra = n / 2;
                    if (n > (max_size() - extra)) {
                        count = max_size();
                    } else {
                        count = n + extra;
                    }
                }
            }

            T* p = allocate(count);
            return {p, count};
        }

        void deallocate(T* p, size_t n) noexcept {
            if (p == nullptr || n == 0) {
                return;
            }

            assert(_has_allocation_resource() && "allocator state must be initialized");

            if (n == 1) [[likely]] {
                if (_state != nullptr) {
                    _state->pool.deallocate(p);
                } else {
                    _runs().deallocate_bytes(static_cast<void*>(p));
                }
                return;
            }

            size_t bytes = n * sizeof(T);

            if (_can_use_tls_run_cache(bytes)) [[likely]] {
                _deallocate_to_tls_run_cache(p, bytes);
                return;
            }

            _runs().deallocate_bytes(static_cast<void*>(p));
        }

        template <typename U, typename... args_t>
        constexpr void construct(U* p, args_t&&... args) {
            std::construct_at(p, std::forward<args_t>(args)...);
        }

        template <typename U>
        constexpr void destroy(U* p) {
            std::destroy_at(p);
        }

        [[nodiscard]] constexpr size_t max_size() const noexcept { return static_cast<size_t>(-1) / sizeof(T); }

        [[nodiscard]] constexpr state_type* state() const noexcept { return _state; }

        friend constexpr bool operator==(
                const allocazam_std_allocator& lhs, const allocazam_std_allocator& rhs) noexcept {
            return lhs._state == rhs._state && lhs._runs_override == rhs._runs_override;
        }

        friend constexpr bool operator!=(
                const allocazam_std_allocator& lhs, const allocazam_std_allocator& rhs) noexcept {
            return !(lhs == rhs);
        }

      private:
        template <typename U, memory_mode OtherMode>
        friend class allocazam_std_allocator;

        struct tls_run_node {
            tls_run_node* next;
        };

        struct tls_run_class {
            tls_run_node* head{};
            size_t count{};
        };

        static constexpr size_t tls_run_quantum{alignof(std::max_align_t)};
        static constexpr size_t tls_run_cutoff{linear_cache_cutoff};
        static constexpr size_t tls_run_class_count{tls_run_cutoff / tls_run_quantum};
        static constexpr size_t tls_refill_batch{1};
        static constexpr size_t tls_high_watermark{32};
        static constexpr size_t tls_drain_low_watermark{16};
        static_assert(std::has_single_bit(tls_run_quantum));
        static_assert((tls_run_cutoff % tls_run_quantum) == 0);

        struct tls_run_cache {
            std::array<tls_run_class, tls_run_class_count> classes{};
        };

        inline static thread_local tls_run_cache _tls_run_cache{};

        [[nodiscard]] static constexpr size_t _tls_align_up(size_t bytes) noexcept {
            size_t mask = tls_run_quantum - 1;
            return (bytes + mask) & ~mask;
        }

        [[nodiscard]] static constexpr size_t _tls_class_index_for(size_t bytes) noexcept {
            return (_tls_align_up(bytes) / tls_run_quantum) - 1;
        }

        [[nodiscard]] static constexpr size_t _tls_class_bytes(size_t idx) noexcept {
            return (idx + 1) * tls_run_quantum;
        }

        [[nodiscard]] constexpr bool _can_use_tls_run_cache(size_t bytes) const noexcept {
            if constexpr (heap_backed_mode<Mode>) {
                if constexpr (sizeof(T) != 1) {
                    return false;
                }
                return _tls_enabled && (bytes <= tls_run_cutoff);
            } else {
                return false;
            }
        }

        [[nodiscard]] constexpr bool _has_allocation_resource() const noexcept {
            return _state != nullptr || _runs_override != nullptr;
        }

        [[nodiscard]] runs_type* _runs_ptr() const noexcept {
            if (_state != nullptr) {
                return const_cast<runs_type*>(std::addressof(_state->runs));
            }
            return _runs_override;
        }

        [[nodiscard]] runs_type& _runs() const noexcept {
            runs_type* runs = _runs_ptr();
            assert(runs != nullptr && "allocator runs must be initialized");
            return *runs;
        }

        static void _tls_push(size_t idx, void* p) noexcept {
            auto& cls = _tls_run_cache.classes[idx];
            auto* node = std::construct_at(static_cast<tls_run_node*>(p), cls.head);
            cls.head = node;
            ++cls.count;
        }

        [[nodiscard]] static tls_run_node* _tls_pop(size_t idx) noexcept {
            auto& cls = _tls_run_cache.classes[idx];
            if (cls.head == nullptr) {
                return nullptr;
            }

            tls_run_node* node = cls.head;
            cls.head = node->next;
            --cls.count;
            return node;
        }

        void _tls_drain_class(size_t idx, size_t keep_count) noexcept {
            while (_tls_run_cache.classes[idx].count > keep_count) {
                auto* node = _tls_pop(idx);
                if (node == nullptr) {
                    break;
                }
                _runs().deallocate_bytes(static_cast<void*>(node));
            }
        }

        void _tls_drain_all() noexcept {
            for (size_t idx : std::ranges::iota_view{size_t{0}, tls_run_class_count}) {
                _tls_drain_class(idx, 0);
            }
        }

        [[nodiscard]] T* _allocate_from_tls_run_cache(size_t bytes) {
            size_t idx = _tls_class_index_for(bytes);
            if (auto* node = _tls_pop(idx); node != nullptr) {
                return reinterpret_cast<T*>(node);
            }

            size_t class_bytes = _tls_class_bytes(idx);
            void* raw = _runs().allocate_bytes(class_bytes, alignof(T));
            if (raw == nullptr) {
                _tls_drain_all();
                raw = _runs().allocate_bytes(class_bytes, alignof(T));
                if (raw == nullptr) {
                    return nullptr;
                }
            }

            if constexpr (tls_refill_batch > 1) {
                for (size_t i : std::ranges::iota_view{size_t{1}, tls_refill_batch}) {
                    (void)i;
                    void* extra = _runs().allocate_bytes(class_bytes, alignof(T));
                    if (extra == nullptr) {
                        break;
                    }
                    _tls_push(idx, extra);
                }
            }

            return static_cast<T*>(raw);
        }

        void _deallocate_to_tls_run_cache(T* p, size_t bytes) noexcept {
            size_t idx = _tls_class_index_for(bytes);
            if (_tls_run_cache.classes[idx].count >= tls_high_watermark) {
                _tls_drain_class(idx, tls_drain_low_watermark);
            }
            _tls_push(idx, static_cast<void*>(p));
        }

        static state_type& _default_state()
            requires(heap_backed_mode<Mode>)
        {
            static state_type state{};
            return state;
        }

        state_type* _state{nullptr};
        runs_type* _runs_override{nullptr};
        bool _tls_enabled{false};
    };

}  // namespace allocazam
