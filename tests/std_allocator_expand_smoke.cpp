#include "allocazam.hpp"
#include "mapping_info.hpp"
#include "utils.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>

namespace {
#if defined(__linux__)
    using huge_state_t = allocazam::allocazam_std_state<
            char,
            allocazam::memory_mode::dynamic,
            allocazam::allocation_model::suballocated,
            allocazam::huge_pages::enabled>;
    using huge_alloc_t = allocazam::allocazam_std_allocator<
            char,
            allocazam::memory_mode::dynamic,
            allocazam::allocation_model::suballocated,
            allocazam::huge_pages::enabled>;
    using huge_rebind_t = huge_alloc_t::rebind<int>::other;

    static_assert(std::is_constructible_v<huge_alloc_t, huge_state_t&>);
    static_assert(!std::is_default_constructible_v<huge_alloc_t>);
    static_assert(std::is_same_v<
                  huge_rebind_t,
                  allocazam::allocazam_std_allocator<
                          int,
                          allocazam::memory_mode::dynamic,
                          allocazam::allocation_model::suballocated,
                          allocazam::huge_pages::enabled>>);
#endif

#if defined(__linux__)
    constexpr size_t huge_page_kb = 2048;
    using allocazam_test::mapping_for_address;
    using allocazam_test::mapping_info;
#endif

    void test_expand_run_backed_preserves_pointer_and_payload() {
        using state_t = allocazam::allocazam_std_state<char, allocazam::memory_mode::dynamic>;
        using alloc_t = allocazam::allocazam_std_allocator<char, allocazam::memory_mode::dynamic>;

        state_t state{4096, 65536};
        alloc_t alloc{state};

        char* a = alloc.allocate(128);
        char* b = alloc.allocate(512);
        char* c = alloc.allocate(96);
        require(a != nullptr && b != nullptr && c != nullptr, "setup allocations failed");

        constexpr size_t canary_len = 64;
        for (size_t i : std::views::iota(size_t{0}, canary_len)) {
            a[i] = static_cast<char>((i * 31 + 11) & 0x7F);
        }

        char* base_before = a;
        alloc.deallocate(b, 512);

        size_t grown_bytes = alloc.expand(a, 320);
        require(grown_bytes >= 320, "expand via std allocator should succeed");
        require(a == base_before, "std allocator expand changed base pointer");

        for (size_t i : std::views::iota(size_t{0}, canary_len)) {
            char expected = static_cast<char>((i * 31 + 11) & 0x7F);
            require(a[i] == expected, "std allocator expand corrupted payload bytes");
        }

        alloc.deallocate(c, 96);
        alloc.deallocate(a, grown_bytes);
    }

    void test_expand_pool_pointer_graceful_failure() {
        using state_t = allocazam::allocazam_std_state<int, allocazam::memory_mode::dynamic>;
        using alloc_t = allocazam::allocazam_std_allocator<int, allocazam::memory_mode::dynamic>;

        state_t state{4096, 65536};
        alloc_t alloc{state};

        int* p = alloc.allocate(1);
        require(p != nullptr, "single-object allocation failed");
        *p = 42;

        int* before = p;
        size_t bytes = alloc.expand(p, sizeof(int) * 2);
        require(bytes == sizeof(int), "pool-backed pointer should report single-object payload");
        require(p == before, "pool-backed expand changed pointer");
        require(*p == 42, "pool-backed expand changed value");

        size_t bytes_void = alloc.expand(static_cast<void*>(p), sizeof(int) * 2);
        require(bytes_void == sizeof(int), "void* expand overload mismatch for pool-backed pointer");

        alloc.deallocate(p, 1);
    }

    // the four stateless-classifier cases, in order: null TLS state -> 0 (and no state
    // materializes); local pool node -> sizeof(T); local runner run -> expands;
    // foreign-owned run -> 0 with the foreign runner untouched
    void test_expand_stateless_classifier() {
        using alloc_t = allocazam::allocazam_std_allocator<int, allocazam::memory_mode::dynamic>;
        using state_t = alloc_t::state_type;

        auto registry_count = [] {
            size_t n = 0;
            for (state_t* s = alloc_t::thread_state_registry(); s != nullptr; s = s->registry_next) {
                ++n;
            }
            return n;
        };

        alloc_t main_alloc{};

        int* buf = main_alloc.allocate(64);
        require(buf != nullptr, "runner allocation failed");
        size_t payload = main_alloc.expand(buf, 64 * sizeof(int));
        require(payload >= 64 * sizeof(int), "local runner expand must report at least the current payload");

        int* node = main_alloc.allocate(1);
        require(node != nullptr, "pool allocation failed");
        require(main_alloc.expand(node, sizeof(int) * 4) == sizeof(int),
                "local pool node must report single-object payload");

        size_t registry_before = registry_count();
        size_t foreign_no_state = 1;
        std::thread consumer{[&] {
            alloc_t worker_alloc{};
            foreign_no_state = worker_alloc.expand(buf, 16);
        }};
        consumer.join();
        require(foreign_no_state == 0, "null-TLS-state expand must return 0");
        require(registry_count() == registry_before, "expand must never create a thread state");

        buf[0] = 1234;
        size_t foreign_with_state = 1;
        std::thread holder{[&] {
            alloc_t worker_alloc{};
            int* own = worker_alloc.allocate(4);
            require(own != nullptr, "worker allocation failed");
            foreign_with_state = worker_alloc.expand(buf, 16);
            worker_alloc.deallocate(own, 4);
        }};
        holder.join();
        require(foreign_with_state == 0, "foreign-owned run expand must return 0");
        require(buf[0] == 1234, "foreign expand must not touch the run");

        main_alloc.deallocate(node, 1);
        main_alloc.deallocate(buf, 64);
    }

    void test_expand_null_returns_zero() {
        using alloc_t = allocazam::allocazam_std_allocator<char, allocazam::memory_mode::dynamic>;
        alloc_t alloc{};

        require(alloc.expand(static_cast<char*>(nullptr), 64) == 0, "typed nullptr expand must return zero");
        require(alloc.expand(static_cast<void*>(nullptr), 64) == 0, "void nullptr expand must return zero");
    }

#if defined(__linux__)
    void test_expand_hugetlb_state_preserves_pointer_and_mapping() {
        huge_state_t state{4096, 65536};
        huge_alloc_t alloc{state};

        char* a = alloc.allocate(128);
        char* b = alloc.allocate(512);
        char* c = alloc.allocate(96);
        require(a != nullptr && b != nullptr && c != nullptr, "hugetlb std allocator setup allocations failed");

        auto* header = reinterpret_cast<std::byte*>(a) - sizeof(size_t);
        std::optional<mapping_info> info = mapping_for_address(header);
        require(info.has_value(), "failed to locate hugetlb std allocator mapping in /proc/self/smaps");
        require(info->kernel_page_kb == huge_page_kb, "std allocator mapping kernel page size is not 2 MiB");
        require(info->hugetlb, "std allocator mapping is missing the hugetlb VmFlag");

        constexpr size_t canary_len = 64;
        for (size_t i : std::views::iota(size_t{0}, canary_len)) {
            a[i] = static_cast<char>((i * 37 + 9) & 0x7F);
        }

        char* base_before = a;
        alloc.deallocate(b, 512);

        size_t grown_bytes = alloc.expand(a, 320);
        require(grown_bytes >= 320, "hugetlb std allocator expand should succeed");
        require(a == base_before, "hugetlb std allocator expand changed base pointer");

        for (size_t i : std::views::iota(size_t{0}, canary_len)) {
            char expected = static_cast<char>((i * 37 + 9) & 0x7F);
            require(a[i] == expected, "hugetlb std allocator expand corrupted payload bytes");
        }

        alloc.deallocate(c, 96);
        alloc.deallocate(a, grown_bytes);
    }
#endif

    struct test_case {
        std::string_view name;
        void (*fn)();
    };

}  // namespace

int main() {
#if defined(__linux__)
    std::vector<test_case> tests{
            test_case{"std_expand_hugetlb_state", test_expand_hugetlb_state_preserves_pointer_and_mapping},
#endif
            test_case{"std_expand_run_backed", test_expand_run_backed_preserves_pointer_and_payload},
            test_case{"std_expand_pool_pointer", test_expand_pool_pointer_graceful_failure},
            test_case{"std_expand_stateless_classifier", test_expand_stateless_classifier},
            test_case{"std_expand_null", test_expand_null_returns_zero},
    };

    size_t fail_count = 0;
    std::ranges::for_each(tests, [&](const test_case& tc) {
        try {
            tc.fn();
            std::cout << "[PASS] " << tc.name << "\n";
        } catch (const std::exception& e) {
            ++fail_count;
            std::cout << "[FAIL] " << tc.name << ": " << e.what() << "\n";
        } catch (...) {
            ++fail_count;
            std::cout << "[FAIL] " << tc.name << ": unknown exception\n";
        }
    });

    if (fail_count != 0) {
        std::cout << "std allocator expand smoke failures: " << fail_count << "\n";
        return 1;
    }

    std::cout << "std allocator expand smoke: all tests passed\n";
    return 0;
}
