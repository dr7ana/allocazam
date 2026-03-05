#include "std_allocator_bench_common.hpp"

#include <map>

namespace {
    using namespace allocazam_bench;

    [[nodiscard]] constexpr size_t map_find_count(size_t count_per_iter) noexcept {
        return (count_per_iter + 3) / 4;
    }

    [[nodiscard]] constexpr size_t map_erase_count(size_t count_per_iter) noexcept {
        return count_per_iter / 2;
    }

    [[nodiscard]] constexpr size_t map_ops_per_iter(size_t count_per_iter) noexcept {
        return count_per_iter + map_find_count(count_per_iter) + map_erase_count(count_per_iter);
    }

    [[nodiscard]] double map_std_workload(size_t iterations, size_t count_per_iter, size_t batch_iterations) {
        using ns_t = std::chrono::nanoseconds;
        batch_iterations = at_least_one(batch_iterations);

        ns_t elapsed{};
        size_t total_ops = iterations * map_ops_per_iter(count_per_iter);

        for (size_t base = 0; base < iterations; base += batch_iterations) {
            size_t batch_count = std::min(batch_iterations, iterations - base);
            std::vector<std::map<int, int>> batch;
            batch.resize(batch_count);

            auto start = allocazam_bench::clock_t::now();
            for (size_t b : std::views::iota(size_t{0}, batch_count)) {
                auto& values = batch[b];

                for (size_t i : std::views::iota(size_t{0}, count_per_iter)) {
                    values.emplace(static_cast<int>(i), static_cast<int>(base + b + i));
                }

                for (size_t i : std::views::iota(size_t{0}, count_per_iter)) {
                    if ((i & 3U) != 0U) {
                        continue;
                    }
                    auto it = values.find(static_cast<int>(i));
                    if (it != values.end()) {
                        sink() += static_cast<uint64_t>(it->second);
                    }
                }

                size_t erase_count = map_erase_count(count_per_iter);
                for (size_t i : std::views::iota(size_t{0}, erase_count)) {
                    values.erase(static_cast<int>(i));
                }
            }
            auto end = allocazam_bench::clock_t::now();
            elapsed += std::chrono::duration_cast<ns_t>(end - start);

            for (auto& values : batch) {
                sink() += static_cast<uint64_t>(values.size());
            }
        }

        return total_ops == 0 ? 0.0 : static_cast<double>(elapsed.count()) / static_cast<double>(total_ops);
    }

    [[nodiscard]] double map_allocazam_workload(size_t iterations, size_t count_per_iter, size_t batch_iterations) {
        using pair_t = std::pair<const int, int>;
        using alloc_t = allocazam::allocazam_std_allocator<pair_t, allocazam::memory_mode::dynamic>;
        using container_t = std::map<int, int, std::less<int>, alloc_t>;
        using ns_t = std::chrono::nanoseconds;

        batch_iterations = at_least_one(batch_iterations);
        ns_t elapsed{};
        size_t total_ops = iterations * map_ops_per_iter(count_per_iter);

        for (size_t base = 0; base < iterations; base += batch_iterations) {
            size_t batch_count = std::min(batch_iterations, iterations - base);
            std::vector<container_t> batch;
            batch.resize(batch_count);

            auto start = allocazam_bench::clock_t::now();
            for (size_t b : std::views::iota(size_t{0}, batch_count)) {
                auto& values = batch[b];

                for (size_t i : std::views::iota(size_t{0}, count_per_iter)) {
                    values.emplace(static_cast<int>(i), static_cast<int>(base + b + i));
                }

                for (size_t i : std::views::iota(size_t{0}, count_per_iter)) {
                    if ((i & 3U) != 0U) {
                        continue;
                    }
                    auto it = values.find(static_cast<int>(i));
                    if (it != values.end()) {
                        sink() += static_cast<uint64_t>(it->second);
                    }
                }

                size_t erase_count = map_erase_count(count_per_iter);
                for (size_t i : std::views::iota(size_t{0}, erase_count)) {
                    values.erase(static_cast<int>(i));
                }
            }
            auto end = allocazam_bench::clock_t::now();
            elapsed += std::chrono::duration_cast<ns_t>(end - start);

            for (auto& values : batch) {
                sink() += static_cast<uint64_t>(values.size());
            }
        }

        return total_ops == 0 ? 0.0 : static_cast<double>(elapsed.count()) / static_cast<double>(total_ops);
    }
}  // namespace

int main(int argc, char** argv) {
    using namespace allocazam_bench;

    size_t iterations = parse_size_arg(argc, argv, 1, 3500);
    size_t count_per_iter = parse_size_arg(argc, argv, 2, 1024);
    size_t repeats = parse_size_arg(argc, argv, 3, 9);
    size_t warmup_divisor = parse_size_arg(argc, argv, 4, 8);
    size_t batch_iterations = parse_size_arg(argc, argv, 5, 64);

    count_per_iter = at_least_one(count_per_iter);
    repeats = at_least_one(repeats);
    warmup_divisor = at_least_one(warmup_divisor);
    batch_iterations = at_least_one(batch_iterations);

    size_t ops_per_iter = map_ops_per_iter(count_per_iter);
    size_t ops = iterations * ops_per_iter;
    size_t warmup_iterations = std::ranges::max(iterations / warmup_divisor, size_t{1});

    std::cout << "map std allocator benchmark config\n";
    std::cout << "  iterations:          " << iterations << "\n";
    std::cout << "  count_per_iter:      " << count_per_iter << "\n";
    std::cout << "  repeats:             " << repeats << "\n";
    std::cout << "  warmup_divisor:      " << warmup_divisor << "\n";
    std::cout << "  batch_iterations:    " << batch_iterations << "\n";

    (void)map_std_workload(warmup_iterations, count_per_iter, batch_iterations);
    (void)map_allocazam_workload(warmup_iterations, count_per_iter, batch_iterations);
    run_stats stats = run_robust_benchmark(
            repeats,
            ops,
            [&] { return map_std_workload(iterations, count_per_iter, batch_iterations); },
            [&] { return map_allocazam_workload(iterations, count_per_iter, batch_iterations); });

    std::cout << "\nrobust benchmark summary\n";
    print_run_stats("map insert/find/erase workload", stats);

    std::cout << "\npercent improvement vs std::allocator (higher is better, based on median ns/op)\n";
    std::cout << "map\n";
    print_percent_improvement("allocazam_std_allocator", stats.std_median, stats.allocazam_median);

    std::vector<size_t> sizes = build_granular_sizes(count_per_iter);
    sizes.push_back(1536);
    std::ranges::sort(sizes);
    sizes.erase(std::ranges::unique(sizes).begin(), sizes.end());

    std::vector<granular_row> rows;
    rows.reserve(sizes.size());

    std::cout << "\nrunning granular map matrix (" << sizes.size() << " sizes)\n";
    size_t idx = 0;
    for (size_t units : sizes) {
        ++idx;
        std::cout << "  map size " << units << " (" << idx << "/" << sizes.size() << ")\n" << std::flush;
        size_t granular_ops_per_iter = map_ops_per_iter(units);
        size_t granular_iterations = std::ranges::max(size_t{1}, ops / granular_ops_per_iter);
        size_t granular_warmup = std::ranges::max(size_t{1}, granular_iterations / warmup_divisor);
        size_t granular_ops = granular_iterations * granular_ops_per_iter;

        (void)map_std_workload(granular_warmup, units, batch_iterations);
        (void)map_allocazam_workload(granular_warmup, units, batch_iterations);
        run_stats granular_stats = run_robust_benchmark(
                repeats,
                granular_ops,
                [&] { return map_std_workload(granular_iterations, units, batch_iterations); },
                [&] { return map_allocazam_workload(granular_iterations, units, batch_iterations); });

        rows.push_back(
                granular_row{
                        "map",
                        units,
                        units * sizeof(std::pair<const int, int>),
                        granular_iterations,
                        granular_ops,
                        granular_stats.std_median,
                        granular_stats.allocazam_median,
                        percent_improvement(granular_stats.std_median, granular_stats.allocazam_median),
                });
    }

    print_granular_table("granular map matrix", rows);

    std::cout << "\nsink=" << sink() << "\n";
    return 0;
}
