#include "runner.hpp"
#include "utils.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {
    constexpr size_t run_alignment = alignof(std::max_align_t);
    constexpr size_t run_header_bytes = sizeof(size_t);
    constexpr size_t huge_page_bytes = size_t{2} << 20;

    using fixed_runs = allocazam::runner::allocator<false>;
    using dynamic_runs = allocazam::runner::allocator<true>;
#if defined(__linux__)
    using huge_fixed_runs = allocazam::runner::allocator<false, false, allocazam::huge_pages::enabled>;
#endif

    [[nodiscard]] std::byte* header_ptr(void* p) noexcept {
        return static_cast<std::byte*>(p) - run_header_bytes;
    }

    [[nodiscard]] size_t payload_bytes(fixed_runs& runs, void* p) noexcept {
        return runs.expand(p, 0);
    }

    [[nodiscard]] size_t payload_bytes(dynamic_runs& runs, void* p) noexcept {
        return runs.expand(p, 0);
    }

#if defined(__linux__)
    [[nodiscard]] size_t payload_bytes(huge_fixed_runs& runs, void* p) noexcept {
        return runs.expand(p, 0);
    }
#endif

    [[nodiscard]] constexpr size_t align_up_pow2(size_t value, size_t alignment) noexcept {
        size_t mask = alignment - 1;
        return (value + mask) & ~mask;
    }

#if defined(__linux__)
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

    void test_expand_null_and_noop() {
        fixed_runs runs(4096);

        require(runs.expand(nullptr, 64) == 0, "expand(nullptr) must return 0");

        void* p = runs.allocate_bytes(80, run_alignment);
        require(p != nullptr, "initial allocation failed");

        size_t current = payload_bytes(runs, p);
        require(current >= 80, "initial payload smaller than requested");

        size_t unchanged = runs.expand(p, current - 1);
        require(unchanged == current, "already-sufficient expand changed payload unexpectedly");

        runs.deallocate_bytes(p);
    }

    void test_expand_fails_when_next_run_is_allocated() {
        fixed_runs runs(4096);

        void* a = runs.allocate_bytes(96, run_alignment);
        void* b = runs.allocate_bytes(96, run_alignment);
        require(a != nullptr && b != nullptr, "setup allocations failed");

        size_t before = payload_bytes(runs, a);
        size_t after = runs.expand(a, before + 64);
        require(after == before, "expand should fail when next run is allocated");

        runs.deallocate_bytes(b);
        void* reused = runs.allocate_bytes(96, run_alignment);
        require(reused != nullptr, "allocator became unusable after failed expand");

        runs.deallocate_bytes(reused);
        runs.deallocate_bytes(a);
    }

    void test_expand_success_with_split_remainder() {
        fixed_runs runs(4096);

        void* a = runs.allocate_bytes(64, run_alignment);
        void* b = runs.allocate_bytes(640, run_alignment);
        void* c = runs.allocate_bytes(64, run_alignment);
        require(a != nullptr && b != nullptr && c != nullptr, "setup allocations failed");

        size_t a_payload = payload_bytes(runs, a);
        size_t b_payload = payload_bytes(runs, b);

        runs.deallocate_bytes(b);

        size_t target = a_payload + (b_payload / 2);
        size_t grown = runs.expand(a, target);
        require(grown >= target, "expand should satisfy split-growth target");

        size_t distance_to_c_header = static_cast<size_t>(header_ptr(c) - header_ptr(a));
        require((grown + run_header_bytes) < distance_to_c_header,
                "expected split remainder between expanded run and next allocated run");

        void* d = runs.allocate_bytes(128, run_alignment);
        require(d != nullptr, "expected alloc from split remainder to succeed");

        runs.deallocate_bytes(d);
        runs.deallocate_bytes(c);
        runs.deallocate_bytes(a);
    }

    void test_expand_success_without_split_remainder() {
        fixed_runs runs(4096);

        void* a = runs.allocate_bytes(64, run_alignment);
        void* b = runs.allocate_bytes(160, run_alignment);
        void* c = runs.allocate_bytes(64, run_alignment);
        require(a != nullptr && b != nullptr && c != nullptr, "setup allocations failed");

        size_t a_payload = payload_bytes(runs, a);
        size_t b_payload = payload_bytes(runs, b);

        size_t max_payload = 0;
        require(allocazam::detail::checked_add(a_payload, b_payload, max_payload), "payload addition overflow");
        require(allocazam::detail::checked_add(max_payload, run_header_bytes, max_payload),
                "payload/header addition overflow");
        size_t slack = align_up_pow2(run_header_bytes, run_alignment) - run_header_bytes;
        require(max_payload > slack, "max payload unexpectedly too small");
        size_t target = max_payload - slack;

        runs.deallocate_bytes(b);

        size_t grown = runs.expand(a, target);
        require(grown >= target, "expand should absorb full adjacent run");

        size_t distance_to_c_header = static_cast<size_t>(header_ptr(c) - header_ptr(a));
        require((grown + run_header_bytes) == distance_to_c_header,
                "unexpected gap remained after full adjacent-run absorb");

        runs.deallocate_bytes(c);
        runs.deallocate_bytes(a);
    }

    void test_expand_fails_when_adjacent_free_is_too_small() {
        fixed_runs runs(4096);

        void* a = runs.allocate_bytes(64, run_alignment);
        void* b = runs.allocate_bytes(64, run_alignment);
        void* c = runs.allocate_bytes(64, run_alignment);
        require(a != nullptr && b != nullptr && c != nullptr, "setup allocations failed");

        size_t a_payload = payload_bytes(runs, a);
        size_t b_payload = payload_bytes(runs, b);

        runs.deallocate_bytes(b);

        size_t unreachable_target = 0;
        require(allocazam::detail::checked_add(a_payload, b_payload, unreachable_target), "target addition overflow");
        require(allocazam::detail::checked_add(unreachable_target, run_header_bytes + 128, unreachable_target),
                "target/header addition overflow");

        size_t grown = runs.expand(a, unreachable_target);
        require(grown == a_payload, "expand should fail when contiguous free bytes are insufficient");

        runs.deallocate_bytes(c);
        runs.deallocate_bytes(a);
    }

    void test_expand_external_backing() {
        std::array<std::byte, 8192> backing{};
        fixed_runs runs(std::span<std::byte>{backing});

        void* a = runs.allocate_bytes(128, run_alignment);
        void* b = runs.allocate_bytes(256, run_alignment);
        void* c = runs.allocate_bytes(128, run_alignment);
        require(a != nullptr && b != nullptr && c != nullptr, "setup allocations failed");

        size_t a_payload = payload_bytes(runs, a);

        runs.deallocate_bytes(b);

        size_t grown = runs.expand(a, a_payload + 96);
        require(grown >= (a_payload + 96), "expand should succeed with external backing");

        runs.deallocate_bytes(c);
        runs.deallocate_bytes(a);
    }

    void test_expand_dynamic_mode_and_post_expand_coalescing() {
        dynamic_runs runs(1024);

        void* a = runs.allocate_bytes(96, run_alignment);
        void* b = runs.allocate_bytes(512, run_alignment);
        void* c = runs.allocate_bytes(96, run_alignment);
        require(a != nullptr && b != nullptr && c != nullptr, "dynamic mode setup allocations failed");

        size_t a_payload = payload_bytes(runs, a);
        size_t b_payload = payload_bytes(runs, b);

        runs.deallocate_bytes(b);

        size_t grown = runs.expand(a, a_payload + (b_payload / 2));
        require(grown >= (a_payload + (b_payload / 2)), "dynamic mode expand should succeed");

        runs.deallocate_bytes(a);
        runs.deallocate_bytes(c);

        void* big = runs.allocate_bytes(2048, run_alignment);
        require(big != nullptr, "allocator failed to coalesce after expand/deallocate sequence");

        runs.deallocate_bytes(big);
    }

    void test_expand_preserves_base_pointer_and_payload() {
        fixed_runs runs(4096);

        void* a = runs.allocate_bytes(128, run_alignment);
        void* b = runs.allocate_bytes(512, run_alignment);
        void* c = runs.allocate_bytes(96, run_alignment);
        require(a != nullptr && b != nullptr && c != nullptr, "setup allocations failed");

        size_t a_payload = payload_bytes(runs, a);
        require(a_payload >= 64, "payload too small for canary check");

        auto* a_bytes = static_cast<std::byte*>(a);
        constexpr size_t canary_len = 64;
        for (size_t i : std::views::iota(size_t{0}, canary_len)) {
            a_bytes[i] = static_cast<std::byte>((i * 29 + 7) & 0xFF);
        }

        void* base_before = a;
        runs.deallocate_bytes(b);

        size_t target = a_payload + 192;
        size_t grown = runs.expand(a, target);
        require(grown >= target, "expand should succeed for pointer invariance test");
        require(a == base_before, "expand changed base pointer on success");

        for (size_t i : std::views::iota(size_t{0}, canary_len)) {
            std::byte expected = static_cast<std::byte>((i * 29 + 7) & 0xFF);
            require(a_bytes[i] == expected, "expand corrupted existing payload bytes");
        }

        void* d = runs.allocate_bytes(128, run_alignment);
        require(d != nullptr, "post-expand allocation failed");

        runs.deallocate_bytes(d);
        runs.deallocate_bytes(c);
        runs.deallocate_bytes(a);

        fixed_runs fail_runs(4096);
        void* x = fail_runs.allocate_bytes(128, run_alignment);
        void* y = fail_runs.allocate_bytes(128, run_alignment);
        require(x != nullptr && y != nullptr, "failure-path setup allocations failed");

        auto* x_bytes = static_cast<std::byte*>(x);
        for (size_t i : std::views::iota(size_t{0}, canary_len)) {
            x_bytes[i] = static_cast<std::byte>((i * 17 + 3) & 0xFF);
        }
        void* x_before = x;
        size_t x_payload = payload_bytes(fail_runs, x);
        size_t failed = fail_runs.expand(x, x_payload + 64);
        require(failed == x_payload, "expected expand failure when next run is allocated");
        require(x == x_before, "expand changed base pointer on failure");
        for (size_t i : std::views::iota(size_t{0}, canary_len)) {
            std::byte expected = static_cast<std::byte>((i * 17 + 3) & 0xFF);
            require(x_bytes[i] == expected, "failed expand corrupted payload bytes");
        }

        fail_runs.deallocate_bytes(y);
        fail_runs.deallocate_bytes(x);
    }

#if defined(__linux__)
    void test_hugetlb_runner_mapping_and_expand() {
        huge_fixed_runs runs(65536);

        void* a = runs.allocate_bytes(128, run_alignment);
        void* b = runs.allocate_bytes(512, run_alignment);
        void* c = runs.allocate_bytes(96, run_alignment);
        require(a != nullptr && b != nullptr && c != nullptr, "hugetlb runner setup allocations failed");

        auto* a_header = header_ptr(a);
        require((reinterpret_cast<uintptr_t>(a_header) % huge_page_bytes) == 0,
                "hugetlb runner header is not 2 MiB-aligned");

        std::optional<mapping_info> info = mapping_for_address(a_header);
        require(info.has_value(), "failed to locate hugetlb runner mapping in /proc/self/smaps");
        require(info->kernel_page_kb == (huge_page_bytes / 1024), "runner mapping kernel page size is not 2 MiB");
        require(info->hugetlb, "runner mapping is missing the hugetlb VmFlag");

        size_t a_payload = payload_bytes(runs, a);
        require(a_payload >= 128, "hugetlb runner payload smaller than requested");

        auto* a_bytes = static_cast<std::byte*>(a);
        constexpr size_t canary_len = 64;
        for (size_t i : std::views::iota(size_t{0}, canary_len)) {
            a_bytes[i] = static_cast<std::byte>((i * 13 + 5) & 0xFF);
        }

        void* base_before = a;
        runs.deallocate_bytes(b);

        size_t grown = runs.expand(a, a_payload + 256);
        require(grown >= (a_payload + 256), "hugetlb runner expand should succeed");
        require(a == base_before, "hugetlb runner expand changed base pointer");

        for (size_t i : std::views::iota(size_t{0}, canary_len)) {
            std::byte expected = static_cast<std::byte>((i * 13 + 5) & 0xFF);
            require(a_bytes[i] == expected, "hugetlb runner expand corrupted payload bytes");
        }

        runs.deallocate_bytes(c);
        runs.deallocate_bytes(a);
    }
#endif

    struct test_case {
        std::string_view name;
        void (*fn)();
    };

}  // namespace

int main() {
    std::vector<test_case> tests{
#if defined(__linux__)
            test_case{"hugetlb_runner_mapping_expand", test_hugetlb_runner_mapping_and_expand},
#endif
            test_case{"expand_null_and_noop", test_expand_null_and_noop},
            test_case{"expand_fail_next_allocated", test_expand_fails_when_next_run_is_allocated},
            test_case{"expand_split_remainder", test_expand_success_with_split_remainder},
            test_case{"expand_absorb_adjacent", test_expand_success_without_split_remainder},
            test_case{"expand_fail_insufficient_contiguous", test_expand_fails_when_adjacent_free_is_too_small},
            test_case{"expand_external_backing", test_expand_external_backing},
            test_case{"expand_dynamic_and_coalesce", test_expand_dynamic_mode_and_post_expand_coalescing},
            test_case{"expand_preserve_pointer_payload", test_expand_preserves_base_pointer_and_payload},
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
        std::cout << "runner expand smoke failures: " << fail_count << "\n";
        return 1;
    }

    std::cout << "runner expand smoke: all tests passed\n";
    return 0;
}
