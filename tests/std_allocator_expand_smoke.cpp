#include "allocazam.hpp"
#include "utils.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

namespace {
#if defined(__linux__)
    using huge_state_t =
            allocazam::allocazam_std_state<char, allocazam::memory_mode::dynamic, allocazam::huge_pages::enabled>;
    using huge_alloc_t =
            allocazam::allocazam_std_allocator<char, allocazam::memory_mode::dynamic, allocazam::huge_pages::enabled>;
    using huge_rebind_t = huge_alloc_t::rebind<int>::other;

    static_assert(std::is_constructible_v<huge_alloc_t, huge_state_t&>);
    static_assert(!std::is_default_constructible_v<huge_alloc_t>);
    static_assert(std::is_same_v<
                  huge_rebind_t,
                  allocazam::allocazam_std_allocator<
                          int,
                          allocazam::memory_mode::dynamic,
                          allocazam::huge_pages::enabled>>);
#endif

#if defined(__linux__)
    constexpr size_t huge_page_kb = 2048;

    struct mapping_info {
        size_t kernel_page_kb{};
        bool hugetlb{};
    };

    [[nodiscard]] bool parse_mapping_header(const std::string& line, uintptr_t& begin, uintptr_t& end) noexcept {
        size_t dash = line.find('-');
        size_t space = line.find(' ');
        if (dash == std::string::npos || space == std::string::npos || dash >= space) {
            return false;
        }

        const char* data = line.data();
        auto [begin_end, begin_ec] = std::from_chars(data, data + dash, begin, 16);
        auto [end_end, end_ec] = std::from_chars(data + dash + 1, data + space, end, 16);
        return begin_ec == std::errc{} && end_ec == std::errc{} && begin_end == (data + dash) &&
               end_end == (data + space);
    }

    [[nodiscard]] size_t parse_kb_value(const std::string& line) noexcept {
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return 0;
        }

        const char* first = line.data() + colon + 1;
        const char* last = line.data() + line.size();
        while (first != last && *first == ' ') {
            ++first;
        }

        size_t value = 0;
        auto [end, ec] = std::from_chars(first, last, value);
        if (ec != std::errc{} || end == first) {
            return 0;
        }
        return value;
    }

    [[nodiscard]] bool vmflags_contains(const std::string& line, std::string_view needle) noexcept {
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return false;
        }

        std::string_view rest{line.data() + colon + 1, line.size() - colon - 1};
        while (!rest.empty()) {
            size_t first = rest.find_first_not_of(' ');
            if (first == std::string_view::npos) {
                return false;
            }
            rest.remove_prefix(first);

            size_t end = rest.find(' ');
            std::string_view token = rest.substr(0, end);
            if (token == needle) {
                return true;
            }
            if (end == std::string_view::npos) {
                return false;
            }
            rest.remove_prefix(end + 1);
        }
        return false;
    }

    [[nodiscard]] std::optional<mapping_info> mapping_for_address(const void* p) {
        std::ifstream smaps{"/proc/self/smaps"};
        if (!smaps.is_open()) {
            return std::nullopt;
        }

        uintptr_t target = reinterpret_cast<uintptr_t>(p);
        std::string line;
        bool in_target = false;
        mapping_info info{};

        while (std::getline(smaps, line)) {
            uintptr_t begin = 0;
            uintptr_t end = 0;
            if (parse_mapping_header(line, begin, end)) {
                if (in_target) {
                    return info;
                }
                in_target = begin <= target && target < end;
                if (in_target) {
                    info = {};
                }
                continue;
            }

            if (!in_target) {
                continue;
            }

            std::string_view view{line};
            if (view.starts_with("KernelPageSize:")) {
                info.kernel_page_kb = parse_kb_value(line);
            } else if (view.starts_with("VmFlags:")) {
                info.hugetlb = vmflags_contains(line, "ht");
            }
        }

        if (in_target) {
            return info;
        }
        return std::nullopt;
    }
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
