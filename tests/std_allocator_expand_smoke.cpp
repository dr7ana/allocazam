#include "allocazam.hpp"
#include "utils.hpp"

#include <array>
#include <iostream>
#include <ranges>
#include <string_view>

namespace {
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

    void test_expand_null_returns_zero() {
        using alloc_t = allocazam::allocazam_std_allocator<char, allocazam::memory_mode::dynamic>;
        alloc_t alloc{};

        require(alloc.expand(static_cast<char*>(nullptr), 64) == 0, "typed nullptr expand must return zero");
        require(alloc.expand(static_cast<void*>(nullptr), 64) == 0, "void nullptr expand must return zero");
    }

    struct test_case {
        std::string_view name;
        void (*fn)();
    };

}  // namespace

int main() {
    constexpr std::array<test_case, 3> tests{
            test_case{"std_expand_run_backed", test_expand_run_backed_preserves_pointer_and_payload},
            test_case{"std_expand_pool_pointer", test_expand_pool_pointer_graceful_failure},
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
