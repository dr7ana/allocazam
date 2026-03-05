#include "utils.hpp"

//
static constexpr size_t pool_size{4096};

template <typename alloc_t>
void print_state(
        const char* label, const alloc_t& alloc, size_t expected_size, size_t expected_capacity, size_t expected_free) {
    std::string_view mode_str = memory_mode_to_string(alloc_t::mode());

    std::cout << label << '\n';
    std::cout << "  mode: " << mode_str << '\n';
    std::cout << "  size:     " << alloc.size() << " (expected " << expected_size << ")\n";
    std::cout << "  capacity: " << alloc.capacity() << " (expected " << expected_capacity << ")\n";
    std::cout << "  free:     " << alloc.free_count() << " (expected " << expected_free << ")\n";
}

int main() {
    {
        // Fixed mode: can allocate up to pool_size, then must return nullptr.
        allocazam::fixed_allocazam<test_object> fixed_mempool{pool_size};
        print_state("fixed: after construction", fixed_mempool, 0, pool_size, pool_size);

        std::vector<test_object*> fixed_allocated;
        fixed_allocated.reserve(pool_size);

        // Fill the pool completely and verify constructed values.
        for (size_t i = 0; i < pool_size; ++i) {
            auto* p = fixed_mempool.allocate();
            require(p != nullptr, "fixed: allocation failed before reaching pool capacity");
            p = fixed_mempool.construct(p, static_cast<int>(i), static_cast<int>(i + 1), static_cast<int>(i + 2));
            require(p->x == static_cast<int>(i), "fixed: x mismatch");
            require(p->y == static_cast<int>(i + 1), "fixed: y mismatch");
            require(p->z == static_cast<int>(i + 2), "fixed: z mismatch");
            fixed_allocated.push_back(p);
        }

        // One more allocation in fixed mode must fail.
        auto* fixed_overflow = fixed_mempool.allocate();
        require(fixed_overflow == nullptr, "fixed: overflow allocation should return nullptr");
        print_state("fixed: after full allocation + overflow", fixed_mempool, pool_size, pool_size, 0);

        // Return all objects and verify free-list restoration.
        for (auto* p : fixed_allocated) {
            fixed_mempool.destroy(p);
            fixed_mempool.deallocate(p);
        }
        print_state("fixed: after deallocate", fixed_mempool, 0, pool_size, pool_size);
    }

    {
        // Dynamic mode: force growth by allocating one past initial capacity.
        allocazam::dynamic_allocazam<test_object> dynamic_mempool{pool_size};
        print_state("dynamic: after construction", dynamic_mempool, 0, pool_size, pool_size);

        std::vector<test_object*> dynamic_allocated;
        dynamic_allocated.reserve(pool_size + 1);

        // Allocate pool_size + 1 objects to trigger one growth step.
        for (size_t i = 0; i < pool_size + 1; ++i) {
            auto* p = dynamic_mempool.allocate();
            require(p != nullptr, "dynamic: allocation unexpectedly returned nullptr");
            p = dynamic_mempool.construct(p, static_cast<int>(i), static_cast<int>(i + 1), static_cast<int>(i + 2));
            require(p->x == static_cast<int>(i), "dynamic: x mismatch");
            require(p->y == static_cast<int>(i + 1), "dynamic: y mismatch");
            require(p->z == static_cast<int>(i + 2), "dynamic: z mismatch");
            dynamic_allocated.push_back(p);
        }

        // Growth doubles next chunk (4096 -> +8192), total capacity becomes 12288.
        constexpr size_t dynamic_capacity_after_growth = pool_size + (pool_size * 2);
        print_state(
                "dynamic: after pool_size + 1 allocations",
                dynamic_mempool,
                pool_size + 1,
                dynamic_capacity_after_growth,
                dynamic_capacity_after_growth - (pool_size + 1));

        // Return all objects; dynamic mode keeps expanded capacity.
        for (auto* p : dynamic_allocated) {
            dynamic_mempool.destroy(p);
            dynamic_mempool.deallocate(p);
        }
        print_state(
                "dynamic: after deallocate",
                dynamic_mempool,
                0,
                dynamic_capacity_after_growth,
                dynamic_capacity_after_growth);
    }

    {
        // noheap mode: pool storage comes entirely from caller-provided bytes.
        std::array<std::byte, pool_size> backing{};

        allocazam::noheap_allocazam<test_object> noheap_mempool{std::span<std::byte>{backing}};
        size_t noheap_capacity = noheap_mempool.capacity();
        print_state("noheap: after construction", noheap_mempool, 0, noheap_capacity, noheap_capacity);

        std::vector<test_object*> noheap_allocated;
        noheap_allocated.reserve(noheap_capacity);

        // Fill all externally-backed nodes and verify constructed values.
        for (size_t i = 0; i < noheap_capacity; ++i) {
            auto* p = noheap_mempool.allocate();
            require(p != nullptr, "noheap: allocation failed before reaching buffer capacity");
            p = noheap_mempool.construct(p, static_cast<int>(i), static_cast<int>(i + 1), static_cast<int>(i + 2));
            require(p->x == static_cast<int>(i), "noheap: x mismatch");
            require(p->y == static_cast<int>(i + 1), "noheap: y mismatch");
            require(p->z == static_cast<int>(i + 2), "noheap: z mismatch");
            noheap_allocated.push_back(p);
        }

        auto* noheap_overflow = noheap_mempool.allocate();
        require(noheap_overflow == nullptr, "noheap: overflow allocation should return nullptr");
        print_state("noheap: after full allocation + overflow", noheap_mempool, noheap_capacity, noheap_capacity, 0);

        for (auto* p : noheap_allocated) {
            noheap_mempool.destroy(p);
            noheap_mempool.deallocate(p);
        }
        print_state("noheap: after deallocate", noheap_mempool, 0, noheap_capacity, noheap_capacity);
    }

    return 0;
}
