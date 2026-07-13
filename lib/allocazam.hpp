#pragma once

#include "exclusive_resource.hpp"
#include "owned_memory.hpp"
#include "runner.hpp"
#include "types.hpp"

#include <algorithm>
#include <atomic>
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

#if defined(__linux__)
    inline constexpr bool explicit_huge_pages_supported = true;
#else
    inline constexpr bool explicit_huge_pages_supported = false;
#endif

    template <memory_mode Memory, allocation_model Allocation, huge_pages Huge>
    concept supported_std_allocator_configuration = (Huge == huge_pages::disabled || explicit_huge_pages_supported) &&
                                                    (Memory != memory_mode::noheap || Huge == huge_pages::disabled) &&
                                                    (Allocation == allocation_model::suballocated ||
                                                     (Allocation == allocation_model::exclusive &&
                                                      (Memory == memory_mode::fixed || Memory == memory_mode::noheap)));

    template <typename T, memory_mode Mode = memory_mode::fixed, bool OwnerHeaders = false>
    class allocazam {
        using node = node_t<T>;
        using chunk = chunk_t<T, heap_backed_mode<Mode>, OwnerHeaders>;

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

        constexpr explicit allocazam(size_t pool_size, lazy_init_t)
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

            // first chunk deferred to the empty-free-list path in allocate()
            _next_growth = pool_size;
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
            if constexpr (OwnerHeaders) {
                // destruction is single-threaded by contract; without this drain a pool
                // that received routed frees would assert spuriously on nodes that were
                // correctly freed
                _drain_remote();
            }
            // compiled out in release mode
            assert(_size == 0 && "outstanding objects at pool destruction");
        }

        allocazam(const allocazam&) = delete;
        allocazam& operator=(const allocazam&) = delete;
        // headered pools are immovable: granule headers hold the pool's address
        allocazam(allocazam&&) noexcept
            requires(!OwnerHeaders)
        = default;
        allocazam& operator=(allocazam&&) noexcept
            requires(!OwnerHeaders)
        = default;

        constexpr T* allocate() {
            node* n = _pop_free_node();
            if (n == nullptr) {
                // drain-before-fail is structural: in fixed mode every node may be parked
                // remotely — exhaustion is not real until the remote stack is drained
                if constexpr (OwnerHeaders) {
                    if (_drain_remote()) {
                        n = _pop_free_node();
                    }
                }
                if (n == nullptr) {
                    if constexpr (heap_backed_mode<Mode>) {
                        // lazily-constructed pool: first chunk lands here, on the slow
                        // path that already exists — zero hot-path branches
                        if (_capacity == 0 && _next_growth != 0) {
                            _add_chunk(_next_growth);
                            n = _pop_free_node();
                        }
                    }
                }
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

        // the only thread-safe entry point. parks a node on the remote stack; the owner
        // splices it back on its next empty-free-list event (or at destruction). may not
        // touch _size, the free list, or the chunk list — the owner mutates those
        // concurrently
        void remote_push(T* ptr) noexcept
            requires(OwnerHeaders)
        {
            assert(ptr != nullptr && "remote_push requires a node pointer");
            node* n = _node_from_value(ptr);
            node* head = _remote.head.load(std::memory_order_relaxed);
            do {
                std::construct_at(_next_ptr(n), head);
            } while (
                    !_remote.head.compare_exchange_weak(head, n, std::memory_order_release, std::memory_order_relaxed));
        }

        [[nodiscard]] static constexpr memory_mode mode() noexcept { return Mode; }
        [[nodiscard]] static constexpr size_t granule_bytes(size_t page_size) noexcept
            requires(OwnerHeaders)
        {
            return chunk::granule_for(page_size);
        }
        [[nodiscard]] constexpr size_t size() const noexcept { return _size; }
        [[nodiscard]] constexpr size_t capacity() const noexcept { return _capacity; }
        [[nodiscard]] constexpr size_t free_count() const noexcept { return _capacity - _size; }
        [[nodiscard]] constexpr bool owns(const T* ptr) const noexcept { return ptr != nullptr && _owns_pointer(ptr); }

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

        // owner-only. takes over the whole stack in one acquire exchange (no per-node
        // pops from a shared stack — no ABA) and splices it into the local free list;
        // the walk touches nodes about to be handed out, doubling as prefetch
        bool _drain_remote() noexcept
            requires(OwnerHeaders)
        {
            node* n = _remote.head.exchange(nullptr, std::memory_order_acquire);
            if (n == nullptr) {
                return false;
            }
            while (n != nullptr) {
                node* next = *std::launder(_next_ptr(n));
                // the remote path has no free-time canaries (the pusher may not touch
                // owner state), so drain time is where its validation lives. incremental
                // pushes mean the free-list check also catches intra-batch duplicates
                assert(_owns_pointer(_value_ptr(n)) && "remotely freed pointer does not belong to this pool");
                assert(!_is_in_free_list(_value_ptr(n)) && "remote double free detected");
                _push_free_node(n);
                --_size;
                n = next;
            }
            return true;
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

            if constexpr (OwnerHeaders) {
                c->stamp_owner(this);
            }

            std::ranges::for_each(std::views::iota(size_t{0}, c->count), [&](size_t i) { _push_free_node(c->at(i)); });

            _capacity += c->count;
        }

        constexpr void _add_external_chunk(node* nodes, size_t slot_count)
            requires(noheap_mode<Mode>)
        {
            assert(_capacity == 0 && "noheap mode can only create one chunk");
            _chunks[0] = chunk(nodes, slot_count);
            chunk* c = &_chunks[0];

            std::ranges::for_each(std::views::iota(size_t{0}, c->count), [&](size_t i) { _push_free_node(c->at(i)); });

            _capacity += c->count;
        }

        constexpr void _grow()
            requires(dynamic_mode<Mode>)
        {
            size_t doubled_growth = 0;
            if (!detail::checked_mul(_next_growth, size_t{2}, doubled_growth)) {
                throw std::bad_alloc{};
            }
            _next_growth = std::ranges::max(static_cast<size_t>(1), doubled_growth);
            _add_chunk(_next_growth);
        }

        constexpr bool _owns_pointer(const T* ptr) const noexcept {
            auto* target_node = _node_from_bytes(reinterpret_cast<const std::byte*>(ptr));
            return std::ranges::any_of(_chunks, [target_node](const auto& c) { return c.contains(target_node); });
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

        struct remote_stack_t {
            // isolated: remote pushers hammer this line; the owner's hot fields must not
            // share it
            alignas(detail::cache_line_size) std::atomic<node*> head{nullptr};
        };
        struct no_remote_stack_t {};

        [[no_unique_address]] std::conditional_t<OwnerHeaders, remote_stack_t, no_remote_stack_t> _remote{};
    };

    template <typename T>
    using fixed_allocazam = allocazam<T, memory_mode::fixed>;

    template <typename T>
    using dynamic_allocazam = allocazam<T, memory_mode::dynamic>;

    template <typename T>
    using noheap_allocazam = allocazam<T, memory_mode::noheap>;

    // which resource an allocazam_std_allocator draws from:
    // - default_state: the process-default state (per-thread post-flip)
    // - explicit_state: a caller-bound allocazam_std_state, confined semantics
    // - runs_override: rebound from another allocator; pinned to that allocator's runner
    enum class resource_mode : uint8_t { default_state, explicit_state, runs_override };

    template <
            typename T,
            memory_mode Mode = memory_mode::dynamic,
            allocation_model Allocation = allocation_model::suballocated,
            huge_pages Huge = huge_pages::disabled>
        requires supported_std_allocator_configuration<Mode, Allocation, Huge>
    struct allocazam_std_state;

    // heap-backed std_state pools are headered: they are the routable targets of
    // owner-routed deallocation. noheap stays raw — external backing cannot guarantee
    // granule alignment, and noheap states are explicit-only (unreachable by routing)
    template <typename T, huge_pages Huge>
    struct alignas(detail::cache_line_size)
            allocazam_std_state<T, memory_mode::fixed, allocation_model::suballocated, Huge> {
        allocazam<T, memory_mode::fixed, true> pool;
        runner::allocator<false, false, Huge> runs;
        // intrusive link for the thread-state registry (default-created states only)
        allocazam_std_state* registry_next{nullptr};

        explicit allocazam_std_state(size_t pool_size = 4096)
                : pool(pool_size), runs(std::ranges::max(pool_size * sizeof(T), size_t{4096})) {}

        explicit allocazam_std_state(lazy_init_t)
                : pool(4096, lazy_init), runs(std::ranges::max(size_t{4096} * sizeof(T), size_t{4096}), lazy_init) {}
    };

    template <typename T, huge_pages Huge>
    struct alignas(detail::cache_line_size)
            allocazam_std_state<T, memory_mode::dynamic, allocation_model::suballocated, Huge> {
        allocazam<T, memory_mode::dynamic, true> pool;
        runner::allocator<true, false, Huge> runs;
        // intrusive link for the thread-state registry (default-created states only)
        allocazam_std_state* registry_next{nullptr};

        explicit allocazam_std_state(size_t pool_size = 4096, size_t runner_bytes = 65536)
                : pool(pool_size), runs(runner_bytes) {}

        explicit allocazam_std_state(lazy_init_t) : pool(4096, lazy_init), runs(65536, lazy_init) {}
    };

    template <typename T, huge_pages Huge>
    struct alignas(detail::cache_line_size)
            allocazam_std_state<T, memory_mode::noheap, allocation_model::suballocated, Huge> {
        allocazam<T, memory_mode::noheap> pool;
        runner::allocator<false, false, Huge> runs;

        explicit allocazam_std_state(std::span<std::byte> node_backing, std::span<std::byte> run_backing)
                : pool(node_backing), runs(run_backing) {}

        explicit allocazam_std_state(std::span<std::byte> backing)
                : allocazam_std_state(
                          backing.first(backing.size() / 2),
                          backing.subspan(backing.size() / 2, backing.size() - (backing.size() / 2))) {}
    };

    template <typename T, huge_pages Huge>
    struct allocazam_std_state<T, memory_mode::fixed, allocation_model::exclusive, Huge> {
        explicit allocazam_std_state(size_t element_capacity) : allocazam_std_state{_config_for(element_capacity)} {}

        allocazam_std_state(const allocazam_std_state&) = delete;
        allocazam_std_state& operator=(const allocazam_std_state&) = delete;
        allocazam_std_state(allocazam_std_state&&) = delete;
        allocazam_std_state& operator=(allocazam_std_state&&) = delete;

        [[nodiscard]] size_t capacity() const noexcept { return _element_capacity; }
        [[nodiscard]] size_t capacity_bytes() const noexcept { return _resource.capacity_bytes(); }
        [[nodiscard]] size_t mapping_bytes() const noexcept { return _backing.bytes(); }
        [[nodiscard]] bool claimed() const noexcept { return _resource.claimed(); }

      private:
        struct config {
            size_t element_capacity;
            size_t usable_bytes;
        };

        [[nodiscard]] static config _config_for(size_t element_capacity) {
            if (element_capacity == 0) {
                throw std::invalid_argument{"exclusive state capacity must not be zero"};
            }

            size_t usable_bytes = 0;
            if (!detail::checked_mul(element_capacity, sizeof(T), usable_bytes)) {
                throw std::bad_array_new_length{};
            }
            return {.element_capacity = element_capacity, .usable_bytes = usable_bytes};
        }

        explicit allocazam_std_state(config cfg)
                : _backing{cfg.usable_bytes, std::ranges::max(alignof(T), alignof(std::max_align_t))},
                  _resource{_backing.base(), cfg.usable_bytes, _backing.alignment()},
                  _element_capacity{cfg.element_capacity} {}

        template <typename U, memory_mode OtherMode, allocation_model OtherAllocation, huge_pages OtherHuge>
            requires supported_std_allocator_configuration<OtherMode, OtherAllocation, OtherHuge>
        friend class allocazam_std_allocator;

        detail::owned_memory_owner<Huge> _backing;
        detail::exclusive_resource _resource;
        size_t _element_capacity;
    };

    template <typename T>
    struct allocazam_std_state<T, memory_mode::noheap, allocation_model::exclusive, huge_pages::disabled> {
        explicit allocazam_std_state(std::span<std::byte> backing) : allocazam_std_state{_config_for(backing)} {}

        allocazam_std_state(const allocazam_std_state&) = delete;
        allocazam_std_state& operator=(const allocazam_std_state&) = delete;
        allocazam_std_state(allocazam_std_state&&) = delete;
        allocazam_std_state& operator=(allocazam_std_state&&) = delete;

        [[nodiscard]] size_t capacity() const noexcept { return _element_capacity; }
        [[nodiscard]] size_t capacity_bytes() const noexcept { return _resource.capacity_bytes(); }
        [[nodiscard]] bool claimed() const noexcept { return _resource.claimed(); }

      private:
        struct config {
            std::byte* base;
            size_t usable_bytes;
            size_t alignment;
            size_t element_capacity;
        };

        [[nodiscard]] static config _config_for(std::span<std::byte> backing) {
            if (backing.empty()) {
                throw std::invalid_argument{"exclusive state backing must not be empty"};
            }

            constexpr size_t required_alignment = std::ranges::max(alignof(T), alignof(std::max_align_t));
            void* aligned = backing.data();
            size_t space = backing.size();
            if (std::align(required_alignment, sizeof(T), aligned, space) == nullptr) {
                throw std::invalid_argument{"exclusive state backing cannot satisfy value alignment"};
            }

            size_t element_capacity = space / sizeof(T);
            if (element_capacity == 0) {
                throw std::invalid_argument{"exclusive state backing is too small for one value"};
            }
            size_t usable_bytes = element_capacity * sizeof(T);
            return {
                    .base = static_cast<std::byte*>(aligned),
                    .usable_bytes = usable_bytes,
                    .alignment = required_alignment,
                    .element_capacity = element_capacity,
            };
        }

        explicit allocazam_std_state(config cfg)
                : _resource{cfg.base, cfg.usable_bytes, cfg.alignment}, _element_capacity{cfg.element_capacity} {}

        template <typename U, memory_mode OtherMode, allocation_model OtherAllocation, huge_pages OtherHuge>
            requires supported_std_allocator_configuration<OtherMode, OtherAllocation, OtherHuge>
        friend class allocazam_std_allocator;

        detail::exclusive_resource _resource;
        size_t _element_capacity;
    };

    template <
            typename T,
            memory_mode Mode = memory_mode::dynamic,
            allocation_model Allocation = allocation_model::suballocated,
            huge_pages Huge = huge_pages::disabled>
        requires supported_std_allocator_configuration<Mode, Allocation, Huge>
    class allocazam_std_allocator;

    template <typename T, memory_mode Mode, huge_pages Huge>
    class allocazam_std_allocator<T, Mode, allocation_model::suballocated, Huge> {
      public:
        using value_type = T;
        using state_type = allocazam_std_state<T, Mode, allocation_model::suballocated, Huge>;
        using runs_type = std::conditional_t<
                dynamic_mode<Mode>,
                runner::allocator<true, false, Huge>,
                runner::allocator<false, false, Huge>>;
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
            using other = allocazam_std_allocator<U, Mode, allocation_model::suballocated, Huge>;
        };

        // stateless: the calling thread's state is resolved per call (created on first
        // touch). three trivial inits — the throw surface lives on first allocation now
        constexpr allocazam_std_allocator() noexcept
            requires(heap_backed_mode<Mode> && Huge == huge_pages::disabled)
                : _state{nullptr}, _runs_override{nullptr}, _mode{resource_mode::default_state} {}

        constexpr explicit allocazam_std_allocator(state_type& state) noexcept
                : _state{&state}, _runs_override{nullptr}, _mode{resource_mode::explicit_state} {}

        // rebind, two behaviors: a stateless source propagates statelessness (n==1 lands
        // in the calling thread's U pool); an explicit source pins its runner — it can
        // bind neither U's default pool (violates confinement) nor the source's state
        // (wrong type)
        template <typename U>
        constexpr allocazam_std_allocator(
                const allocazam_std_allocator<U, Mode, allocation_model::suballocated, Huge>& other) noexcept
                : _state{nullptr}, _runs_override{nullptr}, _mode{resource_mode::default_state} {
            if (other._mode != resource_mode::default_state) {
                _runs_override = other._runs_ptr();
                _mode = resource_mode::runs_override;
                assert(_runs_override != nullptr && "rebind source allocator must be initialized");
            }
        }

        template <typename U>
        constexpr allocazam_std_allocator(
                const allocazam_std_allocator<U, Mode, allocation_model::suballocated, Huge>&,
                state_type& state) noexcept
                : _state{&state}, _runs_override{nullptr}, _mode{resource_mode::explicit_state} {}

        [[nodiscard]] T* allocate(size_t n) {
            assert(_has_allocation_resource() && "allocator state must be initialized");

            if (n == 0) {
                return nullptr;
            }

            if (n > max_size()) {
                throw std::bad_array_new_length{};
            }

            if (n == 1) [[likely]] {
                state_type* s = _alloc_state();
                if (s != nullptr) {
                    T* p = s->pool.allocate();
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

            size_t bytes = 0;
            if (!detail::checked_mul(n, sizeof(T), bytes)) {
                throw std::bad_array_new_length{};
            }

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
                if (_mode == resource_mode::default_state || _state != nullptr) {
                    _free_node_routed(p);
                } else {
                    _free_run_routed(static_cast<void*>(p));
                }
                return;
            }

            size_t bytes = 0;
            if (!detail::checked_mul(n, sizeof(T), bytes)) {
                _free_run_routed(static_cast<void*>(p));
                return;
            }

            if (_can_use_tls_run_cache(bytes)) [[likely]] {
                _deallocate_to_tls_run_cache(p, bytes);
                return;
            }

            _free_run_routed(static_cast<void*>(p));
        }

        [[nodiscard]] size_t expand(T* p, size_t min_new_bytes) noexcept {
            if (p == nullptr) {
                return 0;
            }

            assert(_has_allocation_resource() && "allocator state must be initialized");

            if (_mode == resource_mode::default_state) {
                // the four-case classifier — sound against byte collisions; every step
                // either touches local-only state or is validated before use. the one
                // formal wart is the owner-word probe itself (race-mitigated, not
                // race-free — see runner::owner_of). expansion is an optimization;
                // callers fall back to relocate
                state_type* local = _local_state_or_null();
                if (local == nullptr) {
                    return 0;
                }
                if (local->pool.owns(p)) {
                    return sizeof(T);
                }
                auto* local_runs = const_cast<runs_type*>(std::addressof(local->runs));
                if (runs_type::owner_of(static_cast<void*>(p)) == local_runs &&
                    local_runs->owns_run(static_cast<const void*>(p))) {
                    // the local chunk-range check makes a coincidental owner-word
                    // collision harmless before any header surgery
                    return local_runs->expand(static_cast<void*>(p), min_new_bytes);
                }
                // foreign-owned: current payload is unrepresentable from here by design
                return 0;
            }

            // bound allocators keep today's exact behavior
            // n==1 allocate path routes to node pool, not runner; ensure node pool nodes are not expanded
            if (_state != nullptr && _state->pool.owns(p)) {
                return sizeof(T);
            }

            return _runs().expand(static_cast<void*>(p), min_new_bytes);
        }

        [[nodiscard]] size_t expand(void* p, size_t min_new_bytes) noexcept {
            return expand(static_cast<T*>(p), min_new_bytes);
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

        // the state this allocator would use for its next allocation on the calling
        // thread — created on first touch for stateless instances; nullptr only for
        // rebound-from-explicit (no state_type, runs pointer only). not noexcept: first
        // touch allocates
        [[nodiscard]] state_type* state() const {
            if (_mode == resource_mode::default_state) {
                if constexpr (heap_backed_mode<Mode> && Huge == huge_pages::disabled) {
                    return &_thread_state();
                }
            }
            return _state;
        }

        // head of the push-only registry of every default thread state created for this
        // (T, Mode) instantiation; walk via state->registry_next
        [[nodiscard]] static state_type* thread_state_registry() noexcept {
            return _state_registry.load(std::memory_order_acquire);
        }

        friend constexpr bool operator==(
                const allocazam_std_allocator& lhs, const allocazam_std_allocator& rhs) noexcept {
            return lhs._state == rhs._state && lhs._runs_override == rhs._runs_override;
        }

        friend constexpr bool operator!=(
                const allocazam_std_allocator& lhs, const allocazam_std_allocator& rhs) noexcept {
            return !(lhs == rhs);
        }

      private:
        template <typename U, memory_mode OtherMode, allocation_model OtherAllocation, huge_pages OtherHuge>
            requires supported_std_allocator_configuration<OtherMode, OtherAllocation, OtherHuge>
        friend class allocazam_std_allocator;

        struct tls_run_node {
            tls_run_node* next;
            runs_type* owner;
        };

        struct tls_run_class {
            tls_run_node* head{};
            size_t count{};
        };

        static constexpr size_t tls_run_quantum{alignof(std::max_align_t)};
        static constexpr size_t tls_run_cutoff{linear_cache_cutoff};
        static constexpr size_t tls_run_class_count{tls_run_cutoff / tls_run_quantum};
        static constexpr size_t tls_refill_batch{4};
        static constexpr size_t tls_high_watermark{32};
        static constexpr size_t tls_drain_low_watermark{16};
        static_assert(std::has_single_bit(tls_run_quantum));
        static_assert((tls_run_cutoff % tls_run_quantum) == 0);

        struct tls_run_cache {
            std::array<tls_run_class, tls_run_class_count> classes{};

            // runs at thread exit: locally-owned entries free directly (the TLS pointer
            // is still valid — it is never nulled and states outlive their thread);
            // foreign-owned entries route home over their owner's remote stack (a
            // cross-thread deallocate_bytes would race the owner)
            ~tls_run_cache() {
                runs_type* local = nullptr;
                if constexpr (heap_backed_mode<Mode> && Huge == huge_pages::disabled) {
                    state_type* s = _tls_state;
                    local = s != nullptr ? std::addressof(s->runs) : nullptr;
                }
                std::ranges::for_each(classes, [local](tls_run_class& cls) {
                    tls_run_node* node = cls.head;
                    while (node != nullptr) {
                        tls_run_node* next = node->next;
                        if (node->owner != nullptr) {
                            if (node->owner == local) {
                                node->owner->deallocate_bytes(static_cast<void*>(node));
                            } else {
                                node->owner->remote_push(static_cast<void*>(node));
                            }
                        }
                        node = next;
                    }
                    cls.head = nullptr;
                    cls.count = 0;
                });
            }
        };

        static tls_run_cache& thread_local_cache() {
            static thread_local tls_run_cache _tls_run_cache{};
            return _tls_run_cache;
        }

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
                return _mode == resource_mode::default_state && (bytes <= tls_run_cutoff);
            } else {
                return false;
            }
        }

        [[nodiscard]] constexpr bool _has_allocation_resource() const noexcept {
            switch (_mode) {
                case resource_mode::default_state:
                    return true;  // resolved (and created) per call
                case resource_mode::runs_override:
                    return _runs_override != nullptr;
                case resource_mode::explicit_state:
                    break;
            }
            return _state != nullptr;
        }

        // allocation-context resolution: creates the calling thread's state for the
        // default mode. deallocation paths must use _local_runs_or_null instead
        [[nodiscard]] runs_type* _runs_ptr() const {
            if (_mode == resource_mode::default_state) {
                if constexpr (heap_backed_mode<Mode> && Huge == huge_pages::disabled) {
                    return const_cast<runs_type*>(std::addressof(_thread_state().runs));
                }
            }
            if (_state != nullptr) {
                return const_cast<runs_type*>(std::addressof(_state->runs));
            }
            return _runs_override;
        }

        // owner-routed n==1 free. default-state mode resolves the owning pool from the
        // granule header (pre-flip: always the bound global state, so behavior is
        // identical); other modes keep their confined direct path
        void _free_node_routed(T* p) noexcept {
            if constexpr (heap_backed_mode<Mode>) {
                if (_mode == resource_mode::default_state) {
                    using pool_t = allocazam<T, Mode, true>;
                    size_t granule = pool_t::granule_bytes(detail::detect_page_size());
                    auto* header =
                            reinterpret_cast<const granule_header*>(reinterpret_cast<uintptr_t>(p) & ~(granule - 1));
                    assert(header->debug_tag == granule_debug_tag && "default-path free of non-pool memory");
                    auto* owner = static_cast<pool_t*>(header->owner);
                    assert(owner != nullptr && "granule header owner must be stamped");
                    // null TLS state means "definitely not mine": pure-consumer threads
                    // never materialize a state to free
                    state_type* local = _local_state_or_null();
                    if (local != nullptr && owner == &local->pool) {
                        local->pool.deallocate(p);
                    } else {
                        owner->remote_push(p);
                    }
                    return;
                }
            }
            assert(_state != nullptr && "pool-side free requires a bound state");
            _state->pool.deallocate(p);
        }

        // owner-routed runner-side free. default-state mode reads the run header's owner
        // word — the only foreign-readable word — and routes home; other modes keep their
        // confined direct path
        void _free_run_routed(void* p) noexcept {
            if (_mode == resource_mode::default_state) {
                runs_type* owner = runs_type::owner_of(p);
                assert(owner != nullptr && "run header owner must be stamped");
                if (owner == _local_runs_or_null()) {
                    owner->deallocate_bytes(p);
                } else {
                    owner->remote_push(p);
                }
                return;
            }
            _runs().deallocate_bytes(p);
        }

        [[nodiscard]] runs_type& _runs() const {
            runs_type* runs = _runs_ptr();
            assert(runs != nullptr && "allocator runs must be initialized");
            return *runs;
        }

        static void _tls_push(size_t idx, void* p, runs_type* owner) noexcept {
            assert(owner != nullptr && "tls cache owner must not be null");
            auto& cls = thread_local_cache().classes[idx];
            auto* node = std::construct_at(static_cast<tls_run_node*>(p), cls.head, owner);
            cls.head = node;
            ++cls.count;
        }

        [[nodiscard]] static tls_run_node* _tls_pop_for_owner(size_t idx, runs_type* owner) noexcept {
            auto& cls = thread_local_cache().classes[idx];
            tls_run_node* prev = nullptr;
            for (tls_run_node* node = cls.head; node != nullptr; node = node->next) {
                if (node->owner != owner) {
                    prev = node;
                    continue;
                }

                if (prev == nullptr) {
                    cls.head = node->next;
                } else {
                    prev->next = node->next;
                }
                --cls.count;
                return node;
            }
            return nullptr;
        }

        [[nodiscard]] static tls_run_node* _tls_pop_any(size_t idx) noexcept {
            auto& cls = thread_local_cache().classes[idx];
            if (cls.head == nullptr) {
                return nullptr;
            }

            tls_run_node* node = cls.head;
            cls.head = node->next;
            --cls.count;
            return node;
        }

        void _tls_drain_class(size_t idx, size_t keep_count) noexcept {
            while (thread_local_cache().classes[idx].count > keep_count) {
                auto* node = _tls_pop_any(idx);
                if (node == nullptr) {
                    break;
                }
                assert(node->owner != nullptr && "tls cache node owner must not be null");
                // foreign-stamped entries ride in the cache until drain; only the local
                // runner may be mutated from this thread
                if (node->owner == _local_runs_or_null()) {
                    node->owner->deallocate_bytes(static_cast<void*>(node));
                } else {
                    node->owner->remote_push(static_cast<void*>(node));
                }
            }
        }

        void _tls_drain_all() noexcept {
            std::ranges::for_each(
                    std::views::iota(size_t{0}, tls_run_class_count), [&](size_t idx) { _tls_drain_class(idx, 0); });
        }

        [[nodiscard]] T* _allocate_from_tls_run_cache(size_t bytes) {
            size_t idx = _tls_class_index_for(bytes);
            runs_type* owner = _runs_ptr();
            assert(owner != nullptr && "allocator runs must be initialized");

            if (auto* node = _tls_pop_for_owner(idx, owner); node != nullptr) {
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

            if constexpr (dynamic_mode<Mode>) {
                for (size_t i : std::views::iota(size_t{1}, tls_refill_batch)) {
                    (void)i;
                    void* extra = _runs().allocate_bytes(class_bytes, alignof(T));
                    if (extra == nullptr) {
                        break;
                    }
                    _tls_push(idx, extra, owner);
                }
            }

            return static_cast<T*>(raw);
        }

        void _deallocate_to_tls_run_cache(T* p, size_t bytes) noexcept {
            size_t idx = _tls_class_index_for(bytes);
            if (thread_local_cache().classes[idx].count >= tls_high_watermark) {
                _tls_drain_class(idx, tls_drain_low_watermark);
            }
            // stamp from the run header's owner word, not from this allocator's runner:
            // a stateless allocator may be freeing a buffer another thread's runner owns
            // (identical value pre-flip)
            _tls_push(idx, static_cast<void*>(p), runs_type::owner_of(static_cast<void*>(p)));
        }

        // constant-initialized, trivially-destructible: a plain TLS load plus null branch
        // on the hot path, no init guard, and no TLS destruction-order hazard — the
        // pointer is never nulled and v1 never destroys states, so allocations during
        // other thread_local destructors (including the TLS run cache's own) stay safe
        constinit static inline thread_local state_type* _tls_state{nullptr};

        // process registry of every thread state ever created: reachability (LSan),
        // test introspection, and the v2 reclamation worklist. push-only Treiber list
        constinit static inline std::atomic<state_type*> _state_registry{nullptr};

        static state_type& _thread_state()
            requires(heap_backed_mode<Mode> && Huge == huge_pages::disabled)
        {
            state_type* slot = _tls_state;
            if (slot == nullptr) [[unlikely]] {
                auto* fresh = new state_type{lazy_init};
                state_type* head = _state_registry.load(std::memory_order_relaxed);
                do {
                    fresh->registry_next = head;
                } while (!_state_registry.compare_exchange_weak(
                        head, fresh, std::memory_order_release, std::memory_order_relaxed));
                _tls_state = fresh;
                slot = fresh;
            }
            return *slot;
        }

        // the state this allocator's next allocation on this thread would draw from —
        // creating it if the mode says so — or null. never creates for deallocation
        // callers; those use the *_or_null peers
        [[nodiscard]] state_type* _alloc_state() const {
            if (_mode == resource_mode::default_state) {
                if constexpr (heap_backed_mode<Mode> && Huge == huge_pages::disabled) {
                    return &_thread_state();
                }
            }
            return _state;
        }

        [[nodiscard]] static state_type* _local_state_or_null() noexcept {
            if constexpr (heap_backed_mode<Mode> && Huge == huge_pages::disabled) {
                return _tls_state;
            } else {
                return nullptr;
            }
        }

        [[nodiscard]] runs_type* _local_runs_or_null() const noexcept {
            if (_mode == resource_mode::default_state) {
                state_type* local = _local_state_or_null();
                return local != nullptr ? std::addressof(local->runs) : nullptr;
            }
            return _runs_ptr();
        }

        state_type* _state{nullptr};
        runs_type* _runs_override{nullptr};
        resource_mode _mode{resource_mode::explicit_state};
    };

    template <typename T, memory_mode Mode, huge_pages Huge>
        requires fixed_like_mode<Mode>
    class allocazam_std_allocator<T, Mode, allocation_model::exclusive, Huge> {
      public:
        using value_type = T;
        using state_type = allocazam_std_state<T, Mode, allocation_model::exclusive, Huge>;

        using propagate_on_container_copy_assignment = std::false_type;
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
            using other = allocazam_std_allocator<U, Mode, allocation_model::exclusive, Huge>;
        };

        constexpr explicit allocazam_std_allocator(state_type& state) noexcept : _resource{&state._resource} {}

        constexpr allocazam_std_allocator(const allocazam_std_allocator&) noexcept = default;
        constexpr allocazam_std_allocator& operator=(const allocazam_std_allocator&) noexcept = default;
        constexpr allocazam_std_allocator(allocazam_std_allocator&&) noexcept = default;
        constexpr allocazam_std_allocator& operator=(allocazam_std_allocator&&) noexcept = default;

        template <typename U>
        constexpr allocazam_std_allocator(
                const allocazam_std_allocator<U, Mode, allocation_model::exclusive, Huge>& other) noexcept
                : _resource{other._resource} {}

        [[nodiscard]] T* allocate(size_t n) {
            if (n == 0) {
                return nullptr;
            }
            if (n > _theoretical_max_size()) {
                throw std::bad_array_new_length{};
            }

            size_t bytes = 0;
            if (!detail::checked_mul(n, sizeof(T), bytes)) {
                throw std::bad_array_new_length{};
            }
            void* raw = _resource->claim(bytes, bytes, alignof(T));
            if (raw == nullptr) {
                throw std::bad_alloc{};
            }
            return static_cast<T*>(raw);
        }

        [[nodiscard]] allocate_at_least_result allocate_at_least(size_t n) {
            if (n == 0) {
                return {nullptr, 0};
            }
            if (n > _theoretical_max_size()) {
                throw std::bad_array_new_length{};
            }

            size_t minimum_bytes = 0;
            if (!detail::checked_mul(n, sizeof(T), minimum_bytes)) {
                throw std::bad_array_new_length{};
            }

            size_t returned_count = _resource->capacity_bytes() / sizeof(T);
            if (n > returned_count || alignof(T) > _resource->alignment()) {
                throw std::bad_alloc{};
            }
            size_t maximum_bytes = returned_count * sizeof(T);
            void* raw = _resource->claim(minimum_bytes, maximum_bytes, alignof(T));
            if (raw == nullptr) {
                throw std::bad_alloc{};
            }
            return {static_cast<T*>(raw), returned_count};
        }

        void deallocate(T* pointer, size_t n) noexcept {
            if (pointer == nullptr && n == 0) {
                return;
            }
            if (pointer == nullptr || n == 0) {
                assert(false && "exclusive allocator requires matching pointer and count");
                return;
            }

            size_t bytes = 0;
            bool valid_count = detail::checked_mul(n, sizeof(T), bytes);
            assert(valid_count && "exclusive allocator deallocation count overflow");
            if (!valid_count) {
                return;
            }
            _resource->release(static_cast<void*>(pointer), bytes, alignof(T));
        }

        [[nodiscard]] size_t expand(T* pointer, size_t minimum_new_bytes) noexcept {
            size_t representable_capacity_bytes = (_resource->capacity_bytes() / sizeof(T)) * sizeof(T);
            return _resource->expand(static_cast<void*>(pointer), minimum_new_bytes, representable_capacity_bytes);
        }

        [[nodiscard]] size_t expand(void* pointer, size_t minimum_new_bytes) noexcept {
            return expand(static_cast<T*>(pointer), minimum_new_bytes);
        }

        template <typename U, typename... Args>
        constexpr void construct(U* pointer, Args&&... args) {
            std::construct_at(pointer, std::forward<Args>(args)...);
        }

        template <typename U>
        constexpr void destroy(U* pointer) {
            std::destroy_at(pointer);
        }

        [[nodiscard]] constexpr size_t max_size() const noexcept {
            return alignof(T) <= _resource->alignment() ? _resource->capacity_bytes() / sizeof(T) : 0;
        }

        template <typename U>
        [[nodiscard]] constexpr bool operator==(
                const allocazam_std_allocator<U, Mode, allocation_model::exclusive, Huge>& other) const noexcept {
            return _resource == other._resource;
        }

        template <typename U>
        [[nodiscard]] constexpr bool operator!=(
                const allocazam_std_allocator<U, Mode, allocation_model::exclusive, Huge>& other) const noexcept {
            return !(*this == other);
        }

      private:
        template <typename U, memory_mode OtherMode, allocation_model OtherAllocation, huge_pages OtherHuge>
            requires supported_std_allocator_configuration<OtherMode, OtherAllocation, OtherHuge>
        friend class allocazam_std_allocator;

        [[nodiscard]] static constexpr size_t _theoretical_max_size() noexcept {
            return static_cast<size_t>(-1) / sizeof(T);
        }

        detail::exclusive_resource* _resource;
    };

}  // namespace allocazam
