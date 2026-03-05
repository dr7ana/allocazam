#include "std_allocator_bench_common.hpp"

namespace {
    using namespace allocazam_bench;

    [[nodiscard]] double vector_std_reserved_workload(
            size_t iterations, size_t count_per_iter, size_t batch_iterations) {
        using ns_t = std::chrono::nanoseconds;
        batch_iterations = at_least_one(batch_iterations);

        ns_t elapsed{};
        size_t total_ops = iterations * count_per_iter;

        for (size_t base = 0; base < iterations; base += batch_iterations) {
            size_t batch_count = std::min(batch_iterations, iterations - base);

            std::vector<std::vector<int>> batch;
            batch.reserve(batch_count);
            for (size_t b : std::ranges::iota_view{size_t{0}, batch_count}) {
                (void)b;
                batch.emplace_back();
                batch.back().reserve(count_per_iter);
            }

            auto start = allocazam_bench::clock_t::now();
            for (size_t b : std::ranges::iota_view{size_t{0}, batch_count}) {
                auto& values = batch[b];
                for (size_t i : std::ranges::iota_view{size_t{0}, count_per_iter}) {
                    values.emplace_back(static_cast<int>(base + b + i));
                }
            }
            auto end = allocazam_bench::clock_t::now();
            elapsed += std::chrono::duration_cast<ns_t>(end - start);

            for (auto& values : batch) {
                sink() += static_cast<uint64_t>(values.back());
            }
        }

        return total_ops == 0 ? 0.0 : static_cast<double>(elapsed.count()) / static_cast<double>(total_ops);
    }

    [[nodiscard]] double vector_allocazam_reserved_workload(
            size_t iterations, size_t count_per_iter, size_t batch_iterations) {
        using alloc_t = allocazam::allocazam_std_allocator<int, allocazam::memory_mode::dynamic>;
        using container_t = std::vector<int, alloc_t>;
        using ns_t = std::chrono::nanoseconds;

        batch_iterations = at_least_one(batch_iterations);
        ns_t elapsed{};
        size_t total_ops = iterations * count_per_iter;

        for (size_t base = 0; base < iterations; base += batch_iterations) {
            size_t batch_count = std::min(batch_iterations, iterations - base);

            std::vector<container_t> batch;
            batch.reserve(batch_count);
            for (size_t b : std::ranges::iota_view{size_t{0}, batch_count}) {
                (void)b;
                batch.emplace_back();
                batch.back().reserve(count_per_iter);
            }

            auto start = allocazam_bench::clock_t::now();
            for (size_t b : std::ranges::iota_view{size_t{0}, batch_count}) {
                auto& values = batch[b];
                for (size_t i : std::ranges::iota_view{size_t{0}, count_per_iter}) {
                    values.emplace_back(static_cast<int>(base + b + i));
                }
            }
            auto end = allocazam_bench::clock_t::now();
            elapsed += std::chrono::duration_cast<ns_t>(end - start);

            for (auto& values : batch) {
                sink() += static_cast<uint64_t>(values.back());
            }
        }

        return total_ops == 0 ? 0.0 : static_cast<double>(elapsed.count()) / static_cast<double>(total_ops);
    }

    [[nodiscard]] double vector_std_growth_workload(size_t iterations, size_t count_per_iter, size_t batch_iterations) {
        using ns_t = std::chrono::nanoseconds;
        batch_iterations = at_least_one(batch_iterations);

        ns_t elapsed{};
        size_t total_ops = iterations * count_per_iter;

        for (size_t base = 0; base < iterations; base += batch_iterations) {
            size_t batch_count = std::min(batch_iterations, iterations - base);

            std::vector<std::vector<int>> batch;
            batch.reserve(batch_count);
            for (size_t b : std::ranges::iota_view{size_t{0}, batch_count}) {
                (void)b;
                batch.emplace_back();
            }

            auto start = allocazam_bench::clock_t::now();
            for (size_t b : std::ranges::iota_view{size_t{0}, batch_count}) {
                auto& values = batch[b];
                for (size_t i : std::ranges::iota_view{size_t{0}, count_per_iter}) {
                    values.emplace_back(static_cast<int>(base + b + i));
                }
            }
            auto end = allocazam_bench::clock_t::now();
            elapsed += std::chrono::duration_cast<ns_t>(end - start);

            for (auto& values : batch) {
                sink() += static_cast<uint64_t>(values.back());
            }
        }

        return total_ops == 0 ? 0.0 : static_cast<double>(elapsed.count()) / static_cast<double>(total_ops);
    }

    [[nodiscard]] double vector_allocazam_growth_workload(
            size_t iterations, size_t count_per_iter, size_t batch_iterations) {
        using alloc_t = allocazam::allocazam_std_allocator<int, allocazam::memory_mode::dynamic>;
        using container_t = std::vector<int, alloc_t>;
        using ns_t = std::chrono::nanoseconds;

        batch_iterations = at_least_one(batch_iterations);
        ns_t elapsed{};
        size_t total_ops = iterations * count_per_iter;

        for (size_t base = 0; base < iterations; base += batch_iterations) {
            size_t batch_count = std::min(batch_iterations, iterations - base);

            std::vector<container_t> batch;
            batch.reserve(batch_count);
            for (size_t b : std::ranges::iota_view{size_t{0}, batch_count}) {
                (void)b;
                batch.emplace_back();
            }

            auto start = allocazam_bench::clock_t::now();
            for (size_t b : std::ranges::iota_view{size_t{0}, batch_count}) {
                auto& values = batch[b];
                for (size_t i : std::ranges::iota_view{size_t{0}, count_per_iter}) {
                    values.emplace_back(static_cast<int>(base + b + i));
                }
            }
            auto end = allocazam_bench::clock_t::now();
            elapsed += std::chrono::duration_cast<ns_t>(end - start);

            for (auto& values : batch) {
                sink() += static_cast<uint64_t>(values.back());
            }
        }

        return total_ops == 0 ? 0.0 : static_cast<double>(elapsed.count()) / static_cast<double>(total_ops);
    }
}  // namespace

int main(int argc, char** argv) {
    using namespace allocazam_bench;

    size_t iterations = parse_size_arg(argc, argv, 1, 15000);
    size_t count_per_iter = parse_size_arg(argc, argv, 2, 2048);
    size_t repeats = parse_size_arg(argc, argv, 3, 9);
    size_t warmup_divisor = parse_size_arg(argc, argv, 4, 8);
    size_t batch_iterations = parse_size_arg(argc, argv, 5, 64);

    count_per_iter = at_least_one(count_per_iter);
    repeats = at_least_one(repeats);
    warmup_divisor = at_least_one(warmup_divisor);
    batch_iterations = at_least_one(batch_iterations);

    size_t ops = iterations * count_per_iter;
    size_t warmup_iterations = std::ranges::max(iterations / warmup_divisor, size_t{1});

    std::cout << "vector std allocator benchmark config\n";
    std::cout << "  iterations:          " << iterations << "\n";
    std::cout << "  count_per_iter:      " << count_per_iter << "\n";
    std::cout << "  repeats:             " << repeats << "\n";
    std::cout << "  warmup_divisor:      " << warmup_divisor << "\n";
    std::cout << "  batch_iterations:    " << batch_iterations << "\n";

    (void)vector_std_reserved_workload(warmup_iterations, count_per_iter, batch_iterations);
    (void)vector_allocazam_reserved_workload(warmup_iterations, count_per_iter, batch_iterations);
    run_stats reserved_stats = run_robust_benchmark(
            repeats,
            ops,
            [&] { return vector_std_reserved_workload(iterations, count_per_iter, batch_iterations); },
            [&] { return vector_allocazam_reserved_workload(iterations, count_per_iter, batch_iterations); });

    (void)vector_std_growth_workload(warmup_iterations, count_per_iter, batch_iterations);
    (void)vector_allocazam_growth_workload(warmup_iterations, count_per_iter, batch_iterations);
    run_stats growth_stats = run_robust_benchmark(
            repeats,
            ops,
            [&] { return vector_std_growth_workload(iterations, count_per_iter, batch_iterations); },
            [&] { return vector_allocazam_growth_workload(iterations, count_per_iter, batch_iterations); });

    std::cout << "\nrobust benchmark summary\n";
    print_run_stats("vector reserve workload", reserved_stats);
    print_run_stats("vector growth workload", growth_stats);

    std::cout << "\npercent improvement vs std::allocator (higher is better, based on median ns/op)\n";
    std::cout << "vector_reserve\n";
    print_percent_improvement("allocazam_std_allocator", reserved_stats.std_median, reserved_stats.allocazam_median);
    std::cout << "vector_growth\n";
    print_percent_improvement("allocazam_std_allocator", growth_stats.std_median, growth_stats.allocazam_median);

    std::vector<size_t> sizes = build_granular_sizes(count_per_iter);
    sizes.push_back(1024);
    std::ranges::sort(sizes);
    sizes.erase(std::ranges::unique(sizes).begin(), sizes.end());

    std::vector<granular_row> reserve_rows;
    std::vector<granular_row> growth_rows;
    reserve_rows.reserve(sizes.size());
    growth_rows.reserve(sizes.size());

    std::cout << "\nrunning granular vector_reserve matrix (" << sizes.size() << " sizes)\n";
    size_t idx = 0;
    for (size_t units : sizes) {
        ++idx;
        std::cout << "  vector_reserve size " << units << " (" << idx << "/" << sizes.size() << ")\n" << std::flush;
        size_t granular_iterations = std::ranges::max(size_t{1}, ops / units);
        size_t granular_warmup = std::ranges::max(size_t{1}, granular_iterations / warmup_divisor);
        size_t granular_ops = granular_iterations * units;

        (void)vector_std_reserved_workload(granular_warmup, units, batch_iterations);
        (void)vector_allocazam_reserved_workload(granular_warmup, units, batch_iterations);
        run_stats stats = run_robust_benchmark(
                repeats,
                granular_ops,
                [&] { return vector_std_reserved_workload(granular_iterations, units, batch_iterations); },
                [&] { return vector_allocazam_reserved_workload(granular_iterations, units, batch_iterations); });

        reserve_rows.push_back(
                granular_row{
                        "vector_res",
                        units,
                        units * sizeof(int),
                        granular_iterations,
                        granular_ops,
                        stats.std_median,
                        stats.allocazam_median,
                        percent_improvement(stats.std_median, stats.allocazam_median),
                });
    }

    std::cout << "\nrunning granular vector_growth matrix (" << sizes.size() << " sizes)\n";
    idx = 0;
    for (size_t units : sizes) {
        ++idx;
        std::cout << "  vector_growth size " << units << " (" << idx << "/" << sizes.size() << ")\n" << std::flush;
        size_t granular_iterations = std::ranges::max(size_t{1}, ops / units);
        size_t granular_warmup = std::ranges::max(size_t{1}, granular_iterations / warmup_divisor);
        size_t granular_ops = granular_iterations * units;

        (void)vector_std_growth_workload(granular_warmup, units, batch_iterations);
        (void)vector_allocazam_growth_workload(granular_warmup, units, batch_iterations);
        run_stats stats = run_robust_benchmark(
                repeats,
                granular_ops,
                [&] { return vector_std_growth_workload(granular_iterations, units, batch_iterations); },
                [&] { return vector_allocazam_growth_workload(granular_iterations, units, batch_iterations); });

        growth_rows.push_back(
                granular_row{
                        "vector_grow",
                        units,
                        units * sizeof(int),
                        granular_iterations,
                        granular_ops,
                        stats.std_median,
                        stats.allocazam_median,
                        percent_improvement(stats.std_median, stats.allocazam_median),
                });
    }

    print_granular_table("granular vector reserve matrix", reserve_rows);
    print_granular_table("granular vector growth matrix", growth_rows);

    std::cout << "\nsink=" << sink() << "\n";
    return 0;
}
