#include "allocazam.hpp"
#include "utils.hpp"

#include <iomanip>
#include <new>

namespace {
    uint64_t sink = 0;

    using clock_t = std::chrono::steady_clock;

    struct benchmark_result {
        double ns_per_op;
    };

    benchmark_result print_result(
            std::string_view name, size_t ops, clock_t::time_point start, clock_t::time_point end) {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        double ns_per_op = ops == 0 ? 0.0 : static_cast<double>(ns) / static_cast<double>(ops);
        std::cout << name << "\n";
        std::cout << "  ops:      " << ops << "\n";
        std::cout << "  total ns: " << ns << "\n";
        std::cout << "  ns/op:    " << ns_per_op << "\n";
        return {ns_per_op};
    }

    void print_percent_improvement(std::string_view name, double baseline_ns_per_op, double candidate_ns_per_op) {
        double percent = ((baseline_ns_per_op - candidate_ns_per_op) / baseline_ns_per_op) * 100.0;
        std::cout << "  " << name << ": ";
        std::cout << std::showpos << std::fixed << std::setprecision(2) << percent << "%" << std::noshowpos << "\n";
    }

    template <typename alloc_t>
    benchmark_result benchmark_scalar_allocazam(std::string_view name, alloc_t& alloc, size_t iterations) {
        auto start = clock_t::now();
        for (size_t i : std::views::iota(size_t{0}, iterations)) {
            auto* p = alloc.allocate();
            if (p == nullptr) {
                throw std::bad_alloc{};
            }
            p = alloc.construct(p, static_cast<int>(i), static_cast<int>(i + 1), static_cast<int>(i + 2));
            sink += static_cast<uint64_t>(p->x);
            alloc.destroy(p);
            alloc.deallocate(p);
        }
        auto end = clock_t::now();
        return print_result(name, iterations, start, end);
    }

    template <typename alloc_t>
    benchmark_result benchmark_burst_allocazam(
            std::string_view name, alloc_t& alloc, size_t iterations, size_t burst_size) {
        std::vector<test_object*> ptrs(burst_size, nullptr);

        auto start = clock_t::now();
        for (size_t i : std::views::iota(size_t{0}, iterations)) {
            for (size_t j : std::views::iota(size_t{0}, burst_size)) {
                auto idx = i * burst_size + j;
                auto* p = alloc.allocate();
                if (p == nullptr) {
                    throw std::bad_alloc{};
                }
                p = alloc.construct(p, static_cast<int>(idx), static_cast<int>(idx + 1), static_cast<int>(idx + 2));
                ptrs[j] = p;
            }

            for (size_t j : std::views::iota(size_t{0}, burst_size)) {
                sink += static_cast<uint64_t>(ptrs[j]->y);
                alloc.destroy(ptrs[j]);
                alloc.deallocate(ptrs[j]);
            }
        }
        auto end = clock_t::now();
        return print_result(name, iterations * burst_size, start, end);
    }

    benchmark_result benchmark_scalar_malloc(size_t iterations) {
        auto start = clock_t::now();
        for (size_t i : std::views::iota(size_t{0}, iterations)) {
            void* raw = std::malloc(sizeof(test_object));
            if (raw == nullptr) {
                throw std::bad_alloc{};
            }
            auto* p = ::new (raw) test_object(static_cast<int>(i), static_cast<int>(i + 1), static_cast<int>(i + 2));
            sink += static_cast<uint64_t>(p->x);
            p->~test_object();
            std::free(raw);
        }
        auto end = clock_t::now();
        return print_result("malloc scalar", iterations, start, end);
    }

    benchmark_result benchmark_burst_malloc(size_t iterations, size_t burst_size) {
        std::vector<test_object*> ptrs(burst_size, nullptr);

        auto start = clock_t::now();
        for (size_t i : std::views::iota(size_t{0}, iterations)) {
            for (size_t j : std::views::iota(size_t{0}, burst_size)) {
                auto idx = i * burst_size + j;
                void* raw = std::malloc(sizeof(test_object));
                if (raw == nullptr) {
                    throw std::bad_alloc{};
                }
                ptrs[j] = ::new (raw)
                        test_object(static_cast<int>(idx), static_cast<int>(idx + 1), static_cast<int>(idx + 2));
            }

            for (size_t j : std::views::iota(size_t{0}, burst_size)) {
                sink += static_cast<uint64_t>(ptrs[j]->y);
                ptrs[j]->~test_object();
                std::free(ptrs[j]);
            }
        }
        auto end = clock_t::now();
        return print_result("malloc burst", iterations * burst_size, start, end);
    }

}  // namespace

int main(int argc, char** argv) {
    size_t scalar_iterations = 2'000'000;
    size_t burst_iterations = 20'000;
    size_t burst_size = 512;

    if (argc > 1) {
        scalar_iterations = static_cast<size_t>(std::strtoull(argv[1], nullptr, 10));
    }
    if (argc > 2) {
        burst_iterations = static_cast<size_t>(std::strtoull(argv[2], nullptr, 10));
    }
    if (argc > 3) {
        burst_size = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
    }

    constexpr size_t pool_size = 4096;

    allocazam::fixed_allocazam<test_object> fixed_alloc{pool_size};
    allocazam::dynamic_allocazam<test_object> dynamic_alloc{pool_size};

    std::array<std::byte, 1 << 16> backing{};
    allocazam::noheap_allocazam<test_object> noheap_alloc{std::span<std::byte>{backing}};

    std::cout << "benchmark config\n";
    std::cout << "  scalar_iterations: " << scalar_iterations << "\n";
    std::cout << "  burst_iterations:  " << burst_iterations << "\n";
    std::cout << "  burst_size:        " << burst_size << "\n\n";

    benchmark_result malloc_scalar = benchmark_scalar_malloc(scalar_iterations);
    benchmark_result fixed_scalar =
            benchmark_scalar_allocazam("allocazam fixed scalar", fixed_alloc, scalar_iterations);
    benchmark_result dynamic_scalar =
            benchmark_scalar_allocazam("allocazam dynamic scalar", dynamic_alloc, scalar_iterations);
    benchmark_result noheap_scalar =
            benchmark_scalar_allocazam("allocazam noheap scalar", noheap_alloc, scalar_iterations);

    std::cout << "\n";

    benchmark_result malloc_burst = benchmark_burst_malloc(burst_iterations, burst_size);
    benchmark_result fixed_burst =
            benchmark_burst_allocazam("allocazam fixed burst", fixed_alloc, burst_iterations, burst_size);
    benchmark_result dynamic_burst =
            benchmark_burst_allocazam("allocazam dynamic burst", dynamic_alloc, burst_iterations, burst_size);
    benchmark_result noheap_burst =
            benchmark_burst_allocazam("allocazam noheap burst", noheap_alloc, burst_iterations, burst_size);

    std::cout << "\npercent improvement vs malloc (higher is better, based on ns/op)\n";
    std::cout << "scalar\n";
    print_percent_improvement("fixed", malloc_scalar.ns_per_op, fixed_scalar.ns_per_op);
    print_percent_improvement("dynamic", malloc_scalar.ns_per_op, dynamic_scalar.ns_per_op);
    print_percent_improvement("noheap", malloc_scalar.ns_per_op, noheap_scalar.ns_per_op);

    std::cout << "burst\n";
    print_percent_improvement("fixed", malloc_burst.ns_per_op, fixed_burst.ns_per_op);
    print_percent_improvement("dynamic", malloc_burst.ns_per_op, dynamic_burst.ns_per_op);
    print_percent_improvement("noheap", malloc_burst.ns_per_op, noheap_burst.ns_per_op);

    std::cout << "\nsink=" << sink << "\n";
    return 0;
}
