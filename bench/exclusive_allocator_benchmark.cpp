#include "allocazam.hpp"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace {
    using clock_type = std::chrono::steady_clock;
    using byte = std::byte;

    template <allocazam::memory_mode Memory, allocazam::allocation_model Allocation>
    using state_type = allocazam::allocazam_std_state<byte, Memory, Allocation, allocazam::huge_pages::disabled>;

    template <allocazam::memory_mode Memory, allocazam::allocation_model Allocation>
    using allocator_type =
            allocazam::allocazam_std_allocator<byte, Memory, Allocation, allocazam::huge_pages::disabled>;

    template <typename T>
    inline void do_not_optimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
        asm volatile("" : : "g"(value) : "memory");
#else
        (void)value;
#endif
    }

    struct timing_result {
        std::string_view name;
        size_t capacity_bytes;
        size_t iterations;
        double ns_per_cycle;
    };

    [[nodiscard]] std::span<byte> aligned_backing(std::vector<byte>& backing, size_t bytes) {
        void* base = backing.data();
        size_t space = backing.size();
        if (std::align(alignof(std::max_align_t), bytes, base, space) == nullptr) {
            throw std::bad_alloc{};
        }
        return {static_cast<byte*>(base), bytes};
    }

    template <typename Fn>
    [[nodiscard]] timing_result time_cycles(std::string_view name, size_t capacity_bytes, size_t iterations, Fn&& fn) {
        auto start = clock_type::now();
        for (size_t i = 0; i < iterations; ++i) {
            fn();
        }
        auto end = clock_type::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        return {
                .name = name,
                .capacity_bytes = capacity_bytes,
                .iterations = iterations,
                .ns_per_cycle = static_cast<double>(elapsed) / static_cast<double>(iterations),
        };
    }

    [[nodiscard]] timing_result benchmark_fixed_exclusive(size_t bytes, size_t iterations) {
        state_type<allocazam::memory_mode::fixed, allocazam::allocation_model::exclusive> state{bytes};
        allocator_type<allocazam::memory_mode::fixed, allocazam::allocation_model::exclusive> allocator{state};
        return time_cycles("fixed/exclusive", bytes, iterations, [&] {
            byte* pointer = allocator.allocate(bytes);
            do_not_optimize(pointer);
            allocator.deallocate(pointer, bytes);
        });
    }

    [[nodiscard]] timing_result benchmark_noheap_exclusive(size_t bytes, size_t iterations) {
        std::vector<byte> backing(bytes + alignof(std::max_align_t));
        state_type<allocazam::memory_mode::noheap, allocazam::allocation_model::exclusive> state{
                aligned_backing(backing, bytes)};
        allocator_type<allocazam::memory_mode::noheap, allocazam::allocation_model::exclusive> allocator{state};
        size_t count = state.capacity();
        return time_cycles("noheap/exclusive", count, iterations, [&] {
            byte* pointer = allocator.allocate(count);
            do_not_optimize(pointer);
            allocator.deallocate(pointer, count);
        });
    }

    [[nodiscard]] timing_result benchmark_fixed_suballocated(size_t bytes, size_t iterations) {
        size_t provisioning = std::bit_ceil(bytes + 4096);
        state_type<allocazam::memory_mode::fixed, allocazam::allocation_model::suballocated> state{provisioning};
        allocator_type<allocazam::memory_mode::fixed, allocazam::allocation_model::suballocated> allocator{state};
        return time_cycles("fixed/suballocated", bytes, iterations, [&] {
            byte* pointer = allocator.allocate(bytes);
            do_not_optimize(pointer);
            allocator.deallocate(pointer, bytes);
        });
    }

    [[nodiscard]] timing_result benchmark_std_allocator(size_t bytes, size_t iterations) {
        std::allocator<byte> allocator;
        return time_cycles("std::allocator", bytes, iterations, [&] {
            byte* pointer = allocator.allocate(bytes);
            do_not_optimize(pointer);
            allocator.deallocate(pointer, bytes);
        });
    }

    [[nodiscard]] timing_result benchmark_fixed_construction(size_t bytes, size_t iterations) {
        return time_cycles("fixed/exclusive construct", bytes, iterations, [&] {
            state_type<allocazam::memory_mode::fixed, allocazam::allocation_model::exclusive> state{bytes};
            do_not_optimize(state.capacity_bytes());
        });
    }

    [[nodiscard]] timing_result benchmark_noheap_construction(size_t bytes, size_t iterations) {
        std::vector<byte> backing(bytes + alignof(std::max_align_t));
        std::span<byte> span = aligned_backing(backing, bytes);
        return time_cycles("noheap/exclusive construct", bytes, iterations, [&] {
            state_type<allocazam::memory_mode::noheap, allocazam::allocation_model::exclusive> state{span};
            do_not_optimize(state.capacity_bytes());
        });
    }

    [[nodiscard]] timing_result benchmark_suballocated_construction(size_t bytes, size_t iterations) {
        return time_cycles("fixed/suballocated construct", bytes, iterations, [&] {
            state_type<allocazam::memory_mode::fixed, allocazam::allocation_model::suballocated> state{bytes};
            do_not_optimize(state.pool.capacity());
        });
    }

#if defined(__linux__)
    [[nodiscard]] timing_result benchmark_hugetlb_construction(size_t bytes, size_t iterations) {
        using huge_state = allocazam::allocazam_std_state<
                byte,
                allocazam::memory_mode::fixed,
                allocazam::allocation_model::exclusive,
                allocazam::huge_pages::enabled>;
        return time_cycles("fixed/exclusive hugetlb construct", bytes, iterations, [&] {
            huge_state state{bytes};
            do_not_optimize(state.mapping_bytes());
        });
    }
#endif

    [[nodiscard]] size_t resident_bytes() {
#if defined(__linux__)
        std::ifstream statm{"/proc/self/statm"};
        size_t total_pages = 0;
        size_t resident_pages = 0;
        if (statm >> total_pages >> resident_pages) {
            (void)total_pages;
            return resident_pages * static_cast<size_t>(::sysconf(_SC_PAGESIZE));
        }
#endif
        return 0;
    }

    void print_result(const timing_result& result) {
        std::cout << std::left << std::setw(33) << result.name << std::right << std::setw(12) << result.capacity_bytes
                  << std::setw(14) << result.iterations << std::setw(16) << std::fixed << std::setprecision(3)
                  << result.ns_per_cycle << '\n';
    }

    void verify_fixed_memory(size_t bytes) {
        size_t rss_before = resident_bytes();
        size_t reported_bytes = 0;
        size_t mapping_bytes = 0;
        size_t rss_constructed = 0;
        size_t rss_touched = 0;
        {
            state_type<allocazam::memory_mode::fixed, allocazam::allocation_model::exclusive> state{bytes};
            allocator_type<allocazam::memory_mode::fixed, allocazam::allocation_model::exclusive> allocator{state};
            reported_bytes = state.capacity_bytes();
            mapping_bytes = state.mapping_bytes();
            rss_constructed = resident_bytes();
            byte* pointer = allocator.allocate(state.capacity());
            for (size_t offset = 0; offset < bytes; offset += 4096) {
                pointer[offset] = byte{0x5a};
            }
            pointer[bytes - 1] = byte{0xa5};
            rss_touched = resident_bytes();
            allocator.deallocate(pointer, state.capacity());
        }
        size_t rss_destroyed = resident_bytes();

        std::cout << "\nfixed/exclusive memory verification\n"
                  << "  requested usable bytes: " << bytes << '\n'
                  << "  reported capacity bytes: " << reported_bytes << '\n'
                  << "  mapping bytes:           " << mapping_bytes << '\n'
                  << "  RSS before:              " << rss_before << '\n'
                  << "  RSS after construction:  " << rss_constructed << '\n'
                  << "  RSS after touch:         " << rss_touched << '\n'
                  << "  RSS after destruction:   " << rss_destroyed << '\n';
    }

    void verify_noheap_memory(size_t bytes) {
        using noheap_state = state_type<allocazam::memory_mode::noheap, allocazam::allocation_model::exclusive>;
        using noheap_allocator = allocator_type<allocazam::memory_mode::noheap, allocazam::allocation_model::exclusive>;

        std::vector<byte> backing(bytes + alignof(std::max_align_t));
        std::span<byte> span = aligned_backing(backing, bytes);
        size_t rss_before = resident_bytes();
        size_t reported_bytes = 0;
        size_t rss_constructed = 0;
        size_t rss_touched = 0;
        {
            noheap_state state{span};
            noheap_allocator allocator{state};
            reported_bytes = state.capacity_bytes();
            rss_constructed = resident_bytes();
            byte* pointer = allocator.allocate(state.capacity());
            for (size_t offset = 0; offset < bytes; offset += 4096) {
                pointer[offset] = byte{0x5a};
            }
            pointer[bytes - 1] = byte{0xa5};
            rss_touched = resident_bytes();
            allocator.deallocate(pointer, state.capacity());
        }
        size_t rss_destroyed = resident_bytes();

        std::cout << "\nnoheap/exclusive memory verification\n"
                  << "  caller backing bytes:    " << span.size() << '\n'
                  << "  reported capacity bytes: " << reported_bytes << '\n'
                  << "  out-of-band state bytes: " << sizeof(noheap_state) << '\n'
                  << "  RSS before state:        " << rss_before << '\n'
                  << "  RSS after construction:  " << rss_constructed << '\n'
                  << "  RSS after touch:         " << rss_touched << '\n'
                  << "  RSS after destruction:   " << rss_destroyed << '\n';
    }
}  // namespace

int main(int argc, char** argv) {
    size_t iterations = argc > 1 ? static_cast<size_t>(std::strtoull(argv[1], nullptr, 10)) : 2'000'000;
    size_t construction_iterations = argc > 2 ? static_cast<size_t>(std::strtoull(argv[2], nullptr, 10)) : 2'000;
    bool run_hugetlb = argc > 3 && std::string_view{argv[3]} == "--hugetlb";
    iterations = std::max(iterations, size_t{1});
    construction_iterations = std::max(construction_iterations, size_t{1});

    constexpr std::array capacities{size_t{4096}, size_t{1} << 20, size_t{2} << 20};
    std::cout << "claim/release (one cycle = claim + release)\n";
    std::cout << std::left << std::setw(33) << "case" << std::right << std::setw(12) << "bytes" << std::setw(14)
              << "iterations" << std::setw(16) << "ns/cycle" << '\n';
    for (size_t bytes : capacities) {
        print_result(benchmark_fixed_exclusive(bytes, iterations));
        print_result(benchmark_noheap_exclusive(bytes, iterations));
        print_result(benchmark_fixed_suballocated(bytes, iterations));
        print_result(benchmark_std_allocator(bytes, iterations));
    }

    std::cout << "\nconstruction/destruction\n";
    std::cout << std::left << std::setw(33) << "case" << std::right << std::setw(12) << "bytes" << std::setw(14)
              << "iterations" << std::setw(16) << "ns/cycle" << '\n';
    for (size_t bytes : capacities) {
        print_result(benchmark_fixed_construction(bytes, construction_iterations));
        print_result(benchmark_noheap_construction(bytes, construction_iterations));
    }
    print_result(benchmark_suballocated_construction(4096, construction_iterations));
#if defined(__linux__)
    if (run_hugetlb) {
        print_result(benchmark_hugetlb_construction(size_t{2} << 20, std::min(construction_iterations, size_t{100})));
    }
#else
    (void)run_hugetlb;
#endif

    verify_fixed_memory(size_t{2} << 20);
    verify_noheap_memory(size_t{2} << 20);
    return 0;
}
