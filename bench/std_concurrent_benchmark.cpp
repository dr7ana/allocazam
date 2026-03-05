#include "std_allocator_bench_common.hpp"

#include <barrier>
#include <limits>
#include <thread>

namespace {
    using namespace allocazam_bench;

    struct concurrent_row {
        std::string workload;
        size_t threads{};
        size_t ops{};
        double std_median{};
        double allocazam_median{};
        double improvement{};
    };

    [[nodiscard]] std::vector<size_t> build_thread_counts(size_t max_threads) {
        std::vector<size_t> counts;
        counts.reserve(16);

        size_t threads = 1;
        while (threads < max_threads) {
            counts.push_back(threads);
            if (threads > (std::numeric_limits<size_t>::max() / 2)) {
                break;
            }
            threads *= 2;
        }

        if (counts.empty() || counts.back() != max_threads) {
            counts.push_back(max_threads);
        }
        return counts;
    }

    template <typename ThreadFn>
    [[nodiscard]] double run_concurrent_thread_timed_ns(size_t threads, ThreadFn&& fn) {
        std::barrier ready{static_cast<std::ptrdiff_t>(threads + 1)};
        std::barrier start{static_cast<std::ptrdiff_t>(threads + 1)};

        std::vector<double> thread_ns(threads, 0.0);
        std::vector<std::thread> workers;
        workers.reserve(threads);

        for (size_t tid : std::views::iota(size_t{0}, threads)) {
            workers.emplace_back([&, tid] {
                ready.arrive_and_wait();
                start.arrive_and_wait();
                thread_ns[tid] = fn(tid);
            });
        }

        ready.arrive_and_wait();
        start.arrive_and_wait();
        std::ranges::for_each(workers, [](std::thread& worker) { worker.join(); });

        return std::accumulate(thread_ns.begin(), thread_ns.end(), 0.0);
    }

    [[nodiscard]] double std_vector_reserved_workload(
            size_t threads, size_t iterations, size_t count_per_iter, size_t batch_iterations) {
        using ns_t = std::chrono::nanoseconds;
        batch_iterations = at_least_one(batch_iterations);

        std::vector<uint64_t> local_sink(threads, 0);
        double total_ns = run_concurrent_thread_timed_ns(threads, [&](size_t tid) {
            ns_t elapsed{};
            for (size_t base = 0; base < iterations; base += batch_iterations) {
                size_t batch_count = std::min(batch_iterations, iterations - base);

                std::vector<std::vector<int>> batch;
                batch.reserve(batch_count);
                for (size_t b : std::views::iota(size_t{0}, batch_count)) {
                    (void)b;
                    batch.emplace_back();
                    batch.back().reserve(count_per_iter);
                }

                auto t0 = allocazam_bench::clock_t::now();
                for (size_t b : std::views::iota(size_t{0}, batch_count)) {
                    auto& values = batch[b];
                    for (size_t i : std::views::iota(size_t{0}, count_per_iter)) {
                        values.emplace_back(static_cast<int>(base + b + i + tid));
                    }
                }
                auto t1 = allocazam_bench::clock_t::now();
                elapsed += std::chrono::duration_cast<ns_t>(t1 - t0);

                std::ranges::for_each(
                        batch, [&](const auto& values) { local_sink[tid] ^= static_cast<uint64_t>(values.back()); });
            }
            return static_cast<double>(elapsed.count());
        });

        std::ranges::for_each(local_sink, [](uint64_t v) { sink() ^= v; });
        size_t total_ops = threads * iterations * count_per_iter;
        return total_ops == 0 ? 0.0 : total_ns / static_cast<double>(total_ops);
    }

    [[nodiscard]] double allocazam_vector_reserved_workload(
            size_t threads,
            size_t iterations,
            size_t count_per_iter,
            size_t batch_iterations,
            size_t pool_size,
            size_t runner_bytes) {
        using state_t = allocazam::allocazam_std_state<int, allocazam::memory_mode::dynamic>;
        using alloc_t = allocazam::allocazam_std_allocator<int, allocazam::memory_mode::dynamic>;
        using container_t = std::vector<int, alloc_t>;
        using ns_t = std::chrono::nanoseconds;

        batch_iterations = at_least_one(batch_iterations);
        std::vector<uint64_t> local_sink(threads, 0);

        double total_ns = run_concurrent_thread_timed_ns(threads, [&](size_t tid) {
            state_t state{pool_size, runner_bytes};
            const alloc_t alloc{state};

            ns_t elapsed{};
            for (size_t base = 0; base < iterations; base += batch_iterations) {
                size_t batch_count = std::min(batch_iterations, iterations - base);

                std::vector<container_t> batch;
                batch.reserve(batch_count);
                for (size_t b : std::views::iota(size_t{0}, batch_count)) {
                    (void)b;
                    batch.emplace_back(alloc);
                    batch.back().reserve(count_per_iter);
                }

                auto t0 = allocazam_bench::clock_t::now();
                for (size_t b : std::views::iota(size_t{0}, batch_count)) {
                    auto& values = batch[b];
                    for (size_t i : std::views::iota(size_t{0}, count_per_iter)) {
                        values.emplace_back(static_cast<int>(base + b + i + tid));
                    }
                }
                auto t1 = allocazam_bench::clock_t::now();
                elapsed += std::chrono::duration_cast<ns_t>(t1 - t0);

                std::ranges::for_each(
                        batch, [&](const auto& values) { local_sink[tid] ^= static_cast<uint64_t>(values.back()); });
            }
            return static_cast<double>(elapsed.count());
        });

        std::ranges::for_each(local_sink, [](uint64_t v) { sink() ^= v; });
        size_t total_ops = threads * iterations * count_per_iter;
        return total_ops == 0 ? 0.0 : total_ns / static_cast<double>(total_ops);
    }

    [[nodiscard]] double std_vector_growth_workload(
            size_t threads, size_t iterations, size_t count_per_iter, size_t batch_iterations) {
        using ns_t = std::chrono::nanoseconds;
        batch_iterations = at_least_one(batch_iterations);

        std::vector<uint64_t> local_sink(threads, 0);
        double total_ns = run_concurrent_thread_timed_ns(threads, [&](size_t tid) {
            ns_t elapsed{};
            for (size_t base = 0; base < iterations; base += batch_iterations) {
                size_t batch_count = std::min(batch_iterations, iterations - base);

                std::vector<std::vector<int>> batch;
                batch.reserve(batch_count);
                for (size_t b : std::views::iota(size_t{0}, batch_count)) {
                    (void)b;
                    batch.emplace_back();
                }

                auto t0 = allocazam_bench::clock_t::now();
                for (size_t b : std::views::iota(size_t{0}, batch_count)) {
                    auto& values = batch[b];
                    for (size_t i : std::views::iota(size_t{0}, count_per_iter)) {
                        values.emplace_back(static_cast<int>(base + b + i + tid));
                    }
                }
                auto t1 = allocazam_bench::clock_t::now();
                elapsed += std::chrono::duration_cast<ns_t>(t1 - t0);

                std::ranges::for_each(
                        batch, [&](const auto& values) { local_sink[tid] ^= static_cast<uint64_t>(values.back()); });
            }
            return static_cast<double>(elapsed.count());
        });

        std::ranges::for_each(local_sink, [](uint64_t v) { sink() ^= v; });
        size_t total_ops = threads * iterations * count_per_iter;
        return total_ops == 0 ? 0.0 : total_ns / static_cast<double>(total_ops);
    }

    [[nodiscard]] double allocazam_vector_growth_workload(
            size_t threads,
            size_t iterations,
            size_t count_per_iter,
            size_t batch_iterations,
            size_t pool_size,
            size_t runner_bytes) {
        using state_t = allocazam::allocazam_std_state<int, allocazam::memory_mode::dynamic>;
        using alloc_t = allocazam::allocazam_std_allocator<int, allocazam::memory_mode::dynamic>;
        using container_t = std::vector<int, alloc_t>;
        using ns_t = std::chrono::nanoseconds;

        batch_iterations = at_least_one(batch_iterations);
        std::vector<uint64_t> local_sink(threads, 0);

        double total_ns = run_concurrent_thread_timed_ns(threads, [&](size_t tid) {
            state_t state{pool_size, runner_bytes};
            const alloc_t alloc{state};

            ns_t elapsed{};
            for (size_t base = 0; base < iterations; base += batch_iterations) {
                size_t batch_count = std::min(batch_iterations, iterations - base);

                std::vector<container_t> batch;
                batch.reserve(batch_count);
                for (size_t b : std::views::iota(size_t{0}, batch_count)) {
                    (void)b;
                    batch.emplace_back(alloc);
                }

                auto t0 = allocazam_bench::clock_t::now();
                for (size_t b : std::views::iota(size_t{0}, batch_count)) {
                    auto& values = batch[b];
                    for (size_t i : std::views::iota(size_t{0}, count_per_iter)) {
                        values.emplace_back(static_cast<int>(base + b + i + tid));
                    }
                }
                auto t1 = allocazam_bench::clock_t::now();
                elapsed += std::chrono::duration_cast<ns_t>(t1 - t0);

                std::ranges::for_each(
                        batch, [&](const auto& values) { local_sink[tid] ^= static_cast<uint64_t>(values.back()); });
            }
            return static_cast<double>(elapsed.count());
        });

        std::ranges::for_each(local_sink, [](uint64_t v) { sink() ^= v; });
        size_t total_ops = threads * iterations * count_per_iter;
        return total_ops == 0 ? 0.0 : total_ns / static_cast<double>(total_ops);
    }

    void print_concurrent_table(std::string_view title, const std::vector<concurrent_row>& rows) {
        std::cout << "\n" << title << "\n";
        std::cout << std::left;
        std::cout << std::setw(16) << "workload" << std::setw(10) << "threads" << std::setw(14) << "ops/run"
                  << std::setw(14) << "std median" << std::setw(16) << "allocazam med" << std::setw(10) << "improv %"
                  << "\n";

        std::ranges::for_each(rows, [](const concurrent_row& row) {
            std::cout << std::setw(16) << row.workload << std::setw(10) << row.threads << std::setw(14) << row.ops
                      << std::setw(14) << std::fixed << std::setprecision(6) << row.std_median << std::setw(16)
                      << row.allocazam_median << std::setw(10) << std::setprecision(2) << row.improvement << "\n";
        });
    }
}  // namespace

int main(int argc, char** argv) {
    using namespace allocazam_bench;

    size_t hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) {
        hw_threads = 8;
    }

    size_t max_threads = parse_size_arg(argc, argv, 1, std::ranges::min(hw_threads, size_t{16}));
    size_t vector_iterations_per_thread = parse_size_arg(argc, argv, 2, 2000);
    size_t vector_count_per_iter = parse_size_arg(argc, argv, 3, 1024);
    size_t repeats = parse_size_arg(argc, argv, 4, 7);
    size_t warmup_runs = parse_size_arg(argc, argv, 5, 1);
    size_t batch_iterations = parse_size_arg(argc, argv, 6, 64);
    size_t pool_size = parse_size_arg(argc, argv, 7, 4096);
    size_t runner_bytes = parse_size_arg(argc, argv, 8, 1 << 18);

    max_threads = at_least_one(max_threads);
    vector_iterations_per_thread = at_least_one(vector_iterations_per_thread);
    vector_count_per_iter = at_least_one(vector_count_per_iter);
    repeats = at_least_one(repeats);
    warmup_runs = at_least_one(warmup_runs);
    batch_iterations = at_least_one(batch_iterations);
    pool_size = at_least_one(pool_size);
    runner_bytes = at_least_one(runner_bytes);

    std::vector<size_t> thread_counts = build_thread_counts(max_threads);
    std::vector<concurrent_row> rows;
    rows.reserve(thread_counts.size() * 2);

    std::cout << "std concurrent vector allocator benchmark config\n";
    std::cout << "  max_threads:                 " << max_threads << "\n";
    std::cout << "  vector_iterations_per_thread:" << vector_iterations_per_thread << "\n";
    std::cout << "  vector_count_per_iter:       " << vector_count_per_iter << "\n";
    std::cout << "  repeats:                     " << repeats << "\n";
    std::cout << "  warmup_runs:                 " << warmup_runs << "\n";
    std::cout << "  batch_iterations:            " << batch_iterations << "\n";
    std::cout << "  pool_size:                   " << pool_size << "\n";
    std::cout << "  runner_bytes:                " << runner_bytes << "\n";
    std::cout << "  thread_points:               " << thread_counts.size() << "\n";

    size_t point_index = 0;
    for (size_t threads : thread_counts) {
        ++point_index;
        std::cout << "\nthread point " << threads << " (" << point_index << "/" << thread_counts.size() << ")\n"
                  << std::flush;

        size_t ops = threads * vector_iterations_per_thread * vector_count_per_iter;

        for (size_t i : std::views::iota(size_t{0}, warmup_runs)) {
            (void)i;
            (void)std_vector_reserved_workload(
                    threads, vector_iterations_per_thread, vector_count_per_iter, batch_iterations);
            (void)allocazam_vector_reserved_workload(
                    threads,
                    vector_iterations_per_thread,
                    vector_count_per_iter,
                    batch_iterations,
                    pool_size,
                    runner_bytes);
        }

        run_stats reserve_stats = run_robust_benchmark(
                repeats,
                ops,
                [&] {
                    return std_vector_reserved_workload(
                            threads, vector_iterations_per_thread, vector_count_per_iter, batch_iterations);
                },
                [&] {
                    return allocazam_vector_reserved_workload(
                            threads,
                            vector_iterations_per_thread,
                            vector_count_per_iter,
                            batch_iterations,
                            pool_size,
                            runner_bytes);
                });

        rows.push_back(
                concurrent_row{
                        "vector_reserve",
                        threads,
                        ops,
                        reserve_stats.std_median,
                        reserve_stats.allocazam_median,
                        percent_improvement(reserve_stats.std_median, reserve_stats.allocazam_median),
                });

        for (size_t i : std::views::iota(size_t{0}, warmup_runs)) {
            (void)i;
            (void)std_vector_growth_workload(
                    threads, vector_iterations_per_thread, vector_count_per_iter, batch_iterations);
            (void)allocazam_vector_growth_workload(
                    threads,
                    vector_iterations_per_thread,
                    vector_count_per_iter,
                    batch_iterations,
                    pool_size,
                    runner_bytes);
        }

        run_stats growth_stats = run_robust_benchmark(
                repeats,
                ops,
                [&] {
                    return std_vector_growth_workload(
                            threads, vector_iterations_per_thread, vector_count_per_iter, batch_iterations);
                },
                [&] {
                    return allocazam_vector_growth_workload(
                            threads,
                            vector_iterations_per_thread,
                            vector_count_per_iter,
                            batch_iterations,
                            pool_size,
                            runner_bytes);
                });

        rows.push_back(
                concurrent_row{
                        "vector_growth",
                        threads,
                        ops,
                        growth_stats.std_median,
                        growth_stats.allocazam_median,
                        percent_improvement(growth_stats.std_median, growth_stats.allocazam_median),
                });
    }

    print_concurrent_table("concurrent vector allocator matrix", rows);
    std::cout << "\nsink=" << sink() << "\n";
    return 0;
}
