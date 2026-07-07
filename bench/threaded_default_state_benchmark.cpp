#include "std_allocator_bench_common.hpp"

#include <barrier>
#include <limits>
#include <thread>
#include <unordered_map>

// bench #1 from the thread-caching plan: N threads churning node containers through
// default-constructed allocators — the configuration that corrupted the pre-flip
// process-global default state, which is why this bench could not exist until now.
// post-flip every thread resolves its own default state, so this measures contention on
// nothing and pins the scaling curve.

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

    // unordered_map-style node churn: each iteration builds and destroys a map of
    // `elements` entries through a default-constructed allocator, so every node passes
    // through the thread's own state (rebound stateless -> per-thread pool post-flip)
    template <typename PairAlloc>
    [[nodiscard]] double umap_churn_thread_pass(size_t iterations, size_t elements, size_t tid) {
        using ns_t = std::chrono::nanoseconds;
        using umap_t = std::unordered_map<int, int, std::hash<int>, std::equal_to<int>, PairAlloc>;

        uint64_t local = 0;

        auto t0 = allocazam_bench::clock_t::now();
        for (size_t iter : std::views::iota(size_t{0}, iterations)) {
            umap_t values{0, std::hash<int>{}, std::equal_to<int>{}, PairAlloc{}};
            for (size_t i : std::views::iota(size_t{0}, elements)) {
                values.emplace(static_cast<int>(i), static_cast<int>(i + iter + tid));
            }
            local ^= static_cast<uint64_t>(values.at(static_cast<int>(elements - 1)));
        }
        auto t1 = allocazam_bench::clock_t::now();

        sink() ^= local;
        return static_cast<double>(std::chrono::duration_cast<ns_t>(t1 - t0).count());
    }

    template <typename PairAlloc>
    [[nodiscard]] double umap_churn_workload(size_t threads, size_t iterations, size_t elements) {
        double total_ns = run_concurrent_thread_timed_ns(
                threads, [&](size_t tid) { return umap_churn_thread_pass<PairAlloc>(iterations, elements, tid); });

        size_t total_ops = threads * iterations * elements;
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
    using std_pair_alloc = std::allocator<std::pair<const int, int>>;
    using allocazam_pair_alloc =
            allocazam::allocazam_std_allocator<std::pair<const int, int>, allocazam::memory_mode::dynamic>;

    size_t hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) {
        hw_threads = 4;
    }

    size_t max_threads = parse_size_arg(argc, argv, 1, std::ranges::min(hw_threads, size_t{16}));
    size_t iterations_per_thread = parse_size_arg(argc, argv, 2, 2000);
    size_t elements_per_iter = parse_size_arg(argc, argv, 3, 512);
    size_t repeats = parse_size_arg(argc, argv, 4, 7);
    size_t warmup_runs = parse_size_arg(argc, argv, 5, 1);

    max_threads = at_least_one(max_threads);
    iterations_per_thread = at_least_one(iterations_per_thread);
    elements_per_iter = at_least_one(elements_per_iter);
    repeats = at_least_one(repeats);
    warmup_runs = at_least_one(warmup_runs);

    std::vector<size_t> thread_counts = build_thread_counts(max_threads);
    std::vector<concurrent_row> rows;
    rows.reserve(thread_counts.size());

    std::cout << "threaded default-state benchmark config\n";
    std::cout << "  max_threads:           " << max_threads << "\n";
    std::cout << "  iterations_per_thread: " << iterations_per_thread << "\n";
    std::cout << "  elements_per_iter:     " << elements_per_iter << "\n";
    std::cout << "  repeats:               " << repeats << "\n";
    std::cout << "  warmup_runs:           " << warmup_runs << "\n";
    std::cout << "  thread_points:         " << thread_counts.size() << "\n";

    size_t point_index = 0;
    for (size_t threads : thread_counts) {
        ++point_index;
        std::cout << "\nthread point " << threads << " (" << point_index << "/" << thread_counts.size() << ")\n"
                  << std::flush;

        size_t ops = threads * iterations_per_thread * elements_per_iter;

        for (size_t i : std::views::iota(size_t{0}, warmup_runs)) {
            (void)i;
            (void)umap_churn_workload<std_pair_alloc>(threads, iterations_per_thread, elements_per_iter);
            (void)umap_churn_workload<allocazam_pair_alloc>(threads, iterations_per_thread, elements_per_iter);
        }

        run_stats churn_stats = run_robust_benchmark(
                repeats,
                ops,
                [&] { return umap_churn_workload<std_pair_alloc>(threads, iterations_per_thread, elements_per_iter); },
                [&] {
                    return umap_churn_workload<allocazam_pair_alloc>(threads, iterations_per_thread, elements_per_iter);
                });

        rows.push_back(
                concurrent_row{
                        "umap_churn",
                        threads,
                        ops,
                        churn_stats.std_median,
                        churn_stats.allocazam_median,
                        percent_improvement(churn_stats.std_median, churn_stats.allocazam_median),
                });
    }

    print_concurrent_table("threaded default-state matrix", rows);
    std::cout << "\nsink=" << sink() << "\n";
    return 0;
}
