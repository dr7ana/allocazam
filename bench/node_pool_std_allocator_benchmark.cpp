#include "std_allocator_bench_common.hpp"

#include <barrier>
#include <limits>
#include <thread>

// bench #3 from the thread-caching plan: the node-pool (n==1) analogue of the
// std_concurrent runner workloads. default-constructed allocators, per-thread churn.
// the thread=1 configuration is the direct allocate(1)/deallocate default-pool
// microbench — the only bench isolating the pure TLS + owner-check delta.

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

    // steady node churn over a bounded live set: slot i%live_set is freed (if live) and
    // reallocated each iteration, so the free list stays warm and every op is one
    // allocate(1) or one deallocate(1)
    template <typename Alloc>
    [[nodiscard]] double node_churn_thread_pass(Alloc& alloc, size_t iterations, size_t live_set, size_t tid) {
        using ns_t = std::chrono::nanoseconds;

        std::vector<int*> slots(live_set, nullptr);
        uint64_t local = 0;

        auto t0 = allocazam_bench::clock_t::now();
        for (size_t i : std::views::iota(size_t{0}, iterations)) {
            int*& slot = slots[i % live_set];
            if (slot != nullptr) {
                local ^= static_cast<uint64_t>(*slot);
                alloc.deallocate(slot, 1);
            }
            slot = alloc.allocate(1);
            *slot = static_cast<int>(i + tid);
        }
        std::ranges::for_each(slots, [&](int*& slot) {
            if (slot != nullptr) {
                local ^= static_cast<uint64_t>(*slot);
                alloc.deallocate(slot, 1);
                slot = nullptr;
            }
        });
        auto t1 = allocazam_bench::clock_t::now();

        sink() ^= local;
        return static_cast<double>(std::chrono::duration_cast<ns_t>(t1 - t0).count());
    }

    [[nodiscard]] double std_node_churn_workload(size_t threads, size_t iterations, size_t live_set) {
        double total_ns = run_concurrent_thread_timed_ns(threads, [&](size_t tid) {
            std::allocator<int> alloc{};
            return node_churn_thread_pass(alloc, iterations, live_set, tid);
        });

        size_t total_ops = threads * iterations;
        return total_ops == 0 ? 0.0 : total_ns / static_cast<double>(total_ops);
    }

    [[nodiscard]] double allocazam_node_churn_workload(size_t threads, size_t iterations, size_t live_set) {
        using alloc_t = allocazam::allocazam_std_allocator<int, allocazam::memory_mode::dynamic>;

        double total_ns = run_concurrent_thread_timed_ns(threads, [&](size_t tid) {
            alloc_t alloc{};
            return node_churn_thread_pass(alloc, iterations, live_set, tid);
        });

        size_t total_ops = threads * iterations;
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
        hw_threads = 4;
    }

    size_t max_threads = parse_size_arg(argc, argv, 1, std::ranges::min(hw_threads, size_t{16}));
    size_t iterations_per_thread = parse_size_arg(argc, argv, 2, 2'000'000);
    size_t live_set = parse_size_arg(argc, argv, 3, 1024);
    size_t repeats = parse_size_arg(argc, argv, 4, 7);
    size_t warmup_runs = parse_size_arg(argc, argv, 5, 1);

    max_threads = at_least_one(max_threads);
    iterations_per_thread = at_least_one(iterations_per_thread);
    live_set = at_least_one(live_set);
    repeats = at_least_one(repeats);
    warmup_runs = at_least_one(warmup_runs);

    std::vector<size_t> thread_counts = build_thread_counts(max_threads);
    std::vector<concurrent_row> rows;
    rows.reserve(thread_counts.size());

    std::cout << "node pool allocator benchmark config\n";
    std::cout << "  max_threads:           " << max_threads << "\n";
    std::cout << "  iterations_per_thread: " << iterations_per_thread << "\n";
    std::cout << "  live_set:              " << live_set << "\n";
    std::cout << "  repeats:               " << repeats << "\n";
    std::cout << "  warmup_runs:           " << warmup_runs << "\n";
    std::cout << "  thread_points:         " << thread_counts.size() << "\n";

    size_t point_index = 0;
    for (size_t threads : thread_counts) {
        ++point_index;
        std::cout << "\nthread point " << threads << " (" << point_index << "/" << thread_counts.size() << ")\n"
                  << std::flush;

        size_t ops = threads * iterations_per_thread;

        for (size_t i : std::views::iota(size_t{0}, warmup_runs)) {
            (void)i;
            (void)std_node_churn_workload(threads, iterations_per_thread, live_set);
            (void)allocazam_node_churn_workload(threads, iterations_per_thread, live_set);
        }

        run_stats churn_stats = run_robust_benchmark(
                repeats,
                ops,
                [&] { return std_node_churn_workload(threads, iterations_per_thread, live_set); },
                [&] { return allocazam_node_churn_workload(threads, iterations_per_thread, live_set); });

        rows.push_back(
                concurrent_row{
                        "node_churn",
                        threads,
                        ops,
                        churn_stats.std_median,
                        churn_stats.allocazam_median,
                        percent_improvement(churn_stats.std_median, churn_stats.allocazam_median),
                });
    }

    print_concurrent_table("node pool allocator matrix", rows);
    std::cout << "\nsink=" << sink() << "\n";
    return 0;
}
