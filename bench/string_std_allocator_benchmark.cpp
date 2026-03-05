#include "std_allocator_bench_common.hpp"

namespace {
    using namespace allocazam_bench;

    [[nodiscard]] double string_std_workload(size_t iterations, size_t chars_per_iter, size_t batch_iterations) {
        using ns_t = std::chrono::nanoseconds;
        batch_iterations = at_least_one(batch_iterations);

        ns_t elapsed{};
        size_t total_ops = iterations * chars_per_iter;

        for (size_t base = 0; base < iterations; base += batch_iterations) {
            size_t batch_count = std::min(batch_iterations, iterations - base);
            std::vector<std::string> batch;
            batch.resize(batch_count);

            auto start = allocazam_bench::clock_t::now();
            for (size_t b : std::ranges::iota_view{size_t{0}, batch_count}) {
                auto& s = batch[b];
                for (size_t i : std::ranges::iota_view{size_t{0}, chars_per_iter}) {
                    s.push_back(static_cast<char>('a' + ((base + b + i) % 26)));
                }
            }
            auto end = allocazam_bench::clock_t::now();
            elapsed += std::chrono::duration_cast<ns_t>(end - start);

            for (auto& s : batch) {
                sink() += static_cast<uint64_t>(s.back());
            }
        }

        return total_ops == 0 ? 0.0 : static_cast<double>(elapsed.count()) / static_cast<double>(total_ops);
    }

    [[nodiscard]] double string_allocazam_workload(size_t iterations, size_t chars_per_iter, size_t batch_iterations) {
        using alloc_t = allocazam::allocazam_std_allocator<char, allocazam::memory_mode::dynamic>;
        using string_t = std::basic_string<char, std::char_traits<char>, alloc_t>;
        using ns_t = std::chrono::nanoseconds;

        batch_iterations = at_least_one(batch_iterations);
        ns_t elapsed{};
        size_t total_ops = iterations * chars_per_iter;

        for (size_t base = 0; base < iterations; base += batch_iterations) {
            size_t batch_count = std::min(batch_iterations, iterations - base);
            std::vector<string_t> batch;
            batch.resize(batch_count);

            auto start = allocazam_bench::clock_t::now();
            for (size_t b : std::ranges::iota_view{size_t{0}, batch_count}) {
                auto& s = batch[b];
                for (size_t i : std::ranges::iota_view{size_t{0}, chars_per_iter}) {
                    s.push_back(static_cast<char>('a' + ((base + b + i) % 26)));
                }
            }
            auto end = allocazam_bench::clock_t::now();
            elapsed += std::chrono::duration_cast<ns_t>(end - start);

            for (auto& s : batch) {
                sink() += static_cast<uint64_t>(s.back());
            }
        }

        return total_ops == 0 ? 0.0 : static_cast<double>(elapsed.count()) / static_cast<double>(total_ops);
    }
}  // namespace

int main(int argc, char** argv) {
    using namespace allocazam_bench;

    size_t iterations = parse_size_arg(argc, argv, 1, 25000);
    size_t chars_per_iter = parse_size_arg(argc, argv, 2, 512);
    size_t repeats = parse_size_arg(argc, argv, 3, 9);
    size_t warmup_divisor = parse_size_arg(argc, argv, 4, 8);
    size_t batch_iterations = parse_size_arg(argc, argv, 5, 64);

    chars_per_iter = at_least_one(chars_per_iter);
    repeats = at_least_one(repeats);
    warmup_divisor = at_least_one(warmup_divisor);
    batch_iterations = at_least_one(batch_iterations);

    size_t ops = iterations * chars_per_iter;
    size_t warmup_iterations = std::ranges::max(iterations / warmup_divisor, size_t{1});

    std::cout << "string std allocator benchmark config\n";
    std::cout << "  iterations:          " << iterations << "\n";
    std::cout << "  chars_per_iter:      " << chars_per_iter << "\n";
    std::cout << "  repeats:             " << repeats << "\n";
    std::cout << "  warmup_divisor:      " << warmup_divisor << "\n";
    std::cout << "  batch_iterations:    " << batch_iterations << "\n";

    (void)string_std_workload(warmup_iterations, chars_per_iter, batch_iterations);
    (void)string_allocazam_workload(warmup_iterations, chars_per_iter, batch_iterations);
    run_stats stats = run_robust_benchmark(
            repeats,
            ops,
            [&] { return string_std_workload(iterations, chars_per_iter, batch_iterations); },
            [&] { return string_allocazam_workload(iterations, chars_per_iter, batch_iterations); });

    std::cout << "\nrobust benchmark summary\n";
    print_run_stats("string push_back workload", stats);

    std::cout << "\npercent improvement vs std::allocator (higher is better, based on median ns/op)\n";
    std::cout << "string\n";
    print_percent_improvement("allocazam_std_allocator", stats.std_median, stats.allocazam_median);

    std::vector<size_t> sizes = build_granular_sizes(chars_per_iter);
    sizes.push_back(4096);
    std::ranges::sort(sizes);
    sizes.erase(std::ranges::unique(sizes).begin(), sizes.end());

    std::vector<granular_row> rows;
    rows.reserve(sizes.size());

    std::cout << "\nrunning granular string matrix (" << sizes.size() << " sizes)\n";
    size_t idx = 0;
    for (size_t units : sizes) {
        ++idx;
        std::cout << "  string size " << units << " (" << idx << "/" << sizes.size() << ")\n" << std::flush;
        size_t granular_iterations = std::ranges::max(size_t{1}, ops / units);
        size_t granular_warmup = std::ranges::max(size_t{1}, granular_iterations / warmup_divisor);
        size_t granular_ops = granular_iterations * units;

        (void)string_std_workload(granular_warmup, units, batch_iterations);
        (void)string_allocazam_workload(granular_warmup, units, batch_iterations);
        run_stats granular_stats = run_robust_benchmark(
                repeats,
                granular_ops,
                [&] { return string_std_workload(granular_iterations, units, batch_iterations); },
                [&] { return string_allocazam_workload(granular_iterations, units, batch_iterations); });

        rows.push_back(
                granular_row{
                        "string",
                        units,
                        units * sizeof(char),
                        granular_iterations,
                        granular_ops,
                        granular_stats.std_median,
                        granular_stats.allocazam_median,
                        percent_improvement(granular_stats.std_median, granular_stats.allocazam_median),
                });
    }

    print_granular_table("granular string matrix", rows);

    std::cout << "\nsink=" << sink() << "\n";
    return 0;
}
