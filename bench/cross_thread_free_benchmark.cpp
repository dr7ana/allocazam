#include "std_allocator_bench_common.hpp"

#include <atomic>
#include <barrier>
#include <limits>
#include <mutex>
#include <thread>

// bench #2 from the thread-caching plan: producer-consumer remote frees. the producer
// allocates batches through its default state; the consumer frees them on another
// thread, which routes every node onto the producer pool's remote stack; the producer's
// next empty-free-list event drains them back. this isolates the only genuinely new
// costs of the design — the remote CAS push and the drain splice. only the allocate and
// free phases are timed; handoff waits are excluded.

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

    [[nodiscard]] std::vector<size_t> build_pair_counts(size_t max_pairs) {
        std::vector<size_t> counts;
        counts.reserve(16);

        size_t pairs = 1;
        while (pairs < max_pairs) {
            counts.push_back(pairs);
            if (pairs > (std::numeric_limits<size_t>::max() / 2)) {
                break;
            }
            pairs *= 2;
        }

        if (counts.empty() || counts.back() != max_pairs) {
            counts.push_back(max_pairs);
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

    // one-slot handoff channel per producer/consumer pair. spin+yield keeps the
    // synchronization out of the timed sections without a condition variable
    struct handoff_channel {
        std::mutex guard;
        std::vector<int*> batch;
    };

    template <typename Alloc>
    [[nodiscard]] double producer_pass(handoff_channel& channel, size_t batches, size_t batch_size) {
        using ns_t = std::chrono::nanoseconds;

        Alloc alloc{};
        std::vector<int*> nodes;
        nodes.reserve(batch_size);
        double timed_ns = 0.0;

        for (size_t b : std::views::iota(size_t{0}, batches)) {
            auto t0 = allocazam_bench::clock_t::now();
            for (size_t i : std::views::iota(size_t{0}, batch_size)) {
                int* p = alloc.allocate(1);
                *p = static_cast<int>(b + i);
                nodes.push_back(p);
            }
            auto t1 = allocazam_bench::clock_t::now();
            timed_ns += static_cast<double>(std::chrono::duration_cast<ns_t>(t1 - t0).count());

            // untimed: wait for the consumer to empty the slot, then publish
            while (true) {
                {
                    std::lock_guard lock{channel.guard};
                    if (channel.batch.empty()) {
                        channel.batch.swap(nodes);
                        break;
                    }
                }
                std::this_thread::yield();
            }
            nodes.clear();
        }

        return timed_ns;
    }

    template <typename Alloc>
    [[nodiscard]] double consumer_pass(handoff_channel& channel, size_t batches, size_t batch_size) {
        using ns_t = std::chrono::nanoseconds;

        Alloc alloc{};
        std::vector<int*> nodes;
        nodes.reserve(batch_size);
        uint64_t local = 0;
        double timed_ns = 0.0;

        for (size_t b : std::views::iota(size_t{0}, batches)) {
            (void)b;
            // untimed: wait for a full slot, then take it
            while (true) {
                {
                    std::lock_guard lock{channel.guard};
                    if (!channel.batch.empty()) {
                        nodes.swap(channel.batch);
                        break;
                    }
                }
                std::this_thread::yield();
            }

            auto t0 = allocazam_bench::clock_t::now();
            for (int* p : nodes) {
                local ^= static_cast<uint64_t>(*p);
                alloc.deallocate(p, 1);
            }
            auto t1 = allocazam_bench::clock_t::now();
            timed_ns += static_cast<double>(std::chrono::duration_cast<ns_t>(t1 - t0).count());
            nodes.clear();
        }

        sink() ^= local;
        return timed_ns;
    }

    // even tids produce, odd tids consume; pair p = tids {2p, 2p+1}. ns/op counts each
    // node twice (one alloc, one free)
    template <typename Alloc>
    [[nodiscard]] double cross_thread_free_workload(size_t pairs, size_t batches, size_t batch_size) {
        std::vector<handoff_channel> channels(pairs);

        double total_ns = run_concurrent_thread_timed_ns(pairs * 2, [&](size_t tid) {
            handoff_channel& channel = channels[tid / 2];
            if ((tid % 2) == 0) {
                return producer_pass<Alloc>(channel, batches, batch_size);
            }
            return consumer_pass<Alloc>(channel, batches, batch_size);
        });

        size_t total_ops = pairs * batches * batch_size * 2;
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
    using std_alloc = std::allocator<int>;
    using allocazam_alloc = allocazam::allocazam_std_allocator<int, allocazam::memory_mode::dynamic>;

    size_t hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) {
        hw_threads = 4;
    }

    size_t max_pairs = parse_size_arg(argc, argv, 1, std::ranges::min(hw_threads / 2, size_t{4}));
    size_t batches_per_pair = parse_size_arg(argc, argv, 2, 2000);
    size_t repeats = parse_size_arg(argc, argv, 3, 7);
    size_t warmup_runs = parse_size_arg(argc, argv, 4, 1);

    max_pairs = at_least_one(max_pairs);
    batches_per_pair = at_least_one(batches_per_pair);
    repeats = at_least_one(repeats);
    warmup_runs = at_least_one(warmup_runs);

    constexpr std::array<size_t, 2> batch_sizes{64, 1024};

    std::vector<size_t> pair_counts = build_pair_counts(max_pairs);
    std::vector<concurrent_row> rows;
    rows.reserve(pair_counts.size() * batch_sizes.size());

    std::cout << "cross-thread free benchmark config\n";
    std::cout << "  max_pairs:        " << max_pairs << "\n";
    std::cout << "  batches_per_pair: " << batches_per_pair << "\n";
    std::cout << "  batch_sizes:      64, 1024\n";
    std::cout << "  repeats:          " << repeats << "\n";
    std::cout << "  warmup_runs:      " << warmup_runs << "\n";
    std::cout << "  pair_points:      " << pair_counts.size() << "\n";

    size_t point_index = 0;
    for (size_t pairs : pair_counts) {
        for (size_t batch_size : batch_sizes) {
            ++point_index;
            std::cout << "\npair point " << pairs << " batch " << batch_size << " (" << point_index << "/"
                      << (pair_counts.size() * batch_sizes.size()) << ")\n"
                      << std::flush;

            size_t ops = pairs * batches_per_pair * batch_size * 2;

            for (size_t i : std::views::iota(size_t{0}, warmup_runs)) {
                (void)i;
                (void)cross_thread_free_workload<std_alloc>(pairs, batches_per_pair, batch_size);
                (void)cross_thread_free_workload<allocazam_alloc>(pairs, batches_per_pair, batch_size);
            }

            run_stats stats = run_robust_benchmark(
                    repeats,
                    ops,
                    [&] { return cross_thread_free_workload<std_alloc>(pairs, batches_per_pair, batch_size); },
                    [&] { return cross_thread_free_workload<allocazam_alloc>(pairs, batches_per_pair, batch_size); });

            rows.push_back(
                    concurrent_row{
                            "xfree_b" + std::to_string(batch_size),
                            pairs * 2,
                            ops,
                            stats.std_median,
                            stats.allocazam_median,
                            percent_improvement(stats.std_median, stats.allocazam_median),
                    });
        }
    }

    print_concurrent_table("cross-thread free matrix", rows);
    std::cout << "\nsink=" << sink() << "\n";
    return 0;
}
