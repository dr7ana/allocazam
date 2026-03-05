#include "runner.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using clock_t = std::chrono::steady_clock;

    uint64_t sink = 0;

    enum class op_kind : uint8_t { allocate, deallocate };

    struct op {
        op_kind kind;
        size_t slot;
        size_t bytes;
    };

    struct trace_case {
        std::string name;
        std::vector<op> ops;
        size_t slot_count{};
    };

    struct result_row {
        std::string workload;
        std::string mode;
        size_t ops{};
        double mean_ns_per_op{};
        double median_ns_per_op{};
        double alloc_success{};
        double alloc_fail{};
        double scan_per_find{};
        double split_count{};
        double coalesce_count{};
        double peak_live_bytes{};
    };

    double mean_of(const std::vector<double>& values) {
        if (values.empty()) {
            return 0.0;
        }
        double sum = 0.0;
        for (double value : values) {
            sum += value;
        }
        return sum / static_cast<double>(values.size());
    }

    double median_of(std::vector<double> values) {
        if (values.empty()) {
            return 0.0;
        }
        std::ranges::sort(values);
        size_t n = values.size();
        if ((n & 1U) != 0U) {
            return values[n / 2];
        }
        return (values[n / 2 - 1] + values[n / 2]) * 0.5;
    }

    template <typename SizeFn>
    trace_case make_trace(
            std::string name, size_t steps, size_t max_live, double alloc_bias, uint64_t seed, SizeFn&& size_fn) {
        std::mt19937_64 rng(seed);
        std::uniform_real_distribution<double> coin(0.0, 1.0);

        trace_case trace{};
        trace.name = std::move(name);
        trace.ops.reserve(steps * 2);

        std::vector<size_t> live_slots;
        live_slots.reserve(max_live);

        std::vector<size_t> free_slots;
        free_slots.reserve(max_live);

        size_t next_slot = 0;

        for (size_t step : std::ranges::iota_view{size_t{0}, steps}) {
            bool can_alloc = live_slots.size() < max_live;
            bool do_alloc = live_slots.empty() || (can_alloc && (coin(rng) < alloc_bias));

            if (do_alloc) {
                size_t slot = 0;
                if (!free_slots.empty()) {
                    slot = free_slots.back();
                    free_slots.pop_back();
                } else {
                    slot = next_slot++;
                }

                trace.ops.push_back(op{op_kind::allocate, slot, size_fn(rng, step)});
                live_slots.push_back(slot);
                continue;
            }

            std::uniform_int_distribution<size_t> pick(0, live_slots.size() - 1);
            size_t idx = pick(rng);
            size_t slot = live_slots[idx];
            live_slots[idx] = live_slots.back();
            live_slots.pop_back();
            free_slots.push_back(slot);
            trace.ops.push_back(op{op_kind::deallocate, slot, 0});
        }

        while (!live_slots.empty()) {
            size_t slot = live_slots.back();
            live_slots.pop_back();
            trace.ops.push_back(op{op_kind::deallocate, slot, 0});
        }

        trace.slot_count = next_slot;
        return trace;
    }

    constexpr size_t aligned_random(std::mt19937_64& rng, size_t lo, size_t hi) {
        std::uniform_int_distribution<size_t> pick(lo, hi);
        std::array<size_t, 1> sample{};
        std::ranges::generate(sample, [&] { return pick(rng); });
        size_t value = sample[0];
        size_t rem = value % 16;
        if (rem == 0) {
            return value;
        }
        return value + (16 - rem);
    }

    constexpr size_t aligned_random_to(std::mt19937_64& rng, size_t lo, size_t hi, size_t multiple) {
        std::uniform_int_distribution<size_t> pick(lo, hi);
        std::array<size_t, 1> sample{};
        std::ranges::generate(sample, [&] { return pick(rng); });
        size_t value = sample[0];
        size_t rem = value % multiple;
        if (rem == 0) {
            return value;
        }
        return value + (multiple - rem);
    }

    trace_case small_steady_trace() {
        static constexpr std::array<size_t, 16> classes{
                16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256};
        return make_trace("small_steady", 220000, 2048, 0.62, 0xA11CEULL, [&](auto& rng, size_t) -> size_t {
            std::uniform_int_distribution<size_t> pick(0, classes.size() - 1);
            return classes[pick(rng)];
        });
    }

    trace_case medium_mixed_trace() {
        return make_trace("medium_mixed", 180000, 1400, 0.58, 0xBADA55ULL, [&](auto& rng, size_t) -> size_t {
            return aligned_random(rng, 272, 4096);
        });
    }

    trace_case mid_band_trace(std::string name, size_t lo, size_t hi, uint64_t seed) {
        return make_trace(std::move(name), 180000, 1400, 0.58, seed, [=](auto& rng, size_t) -> size_t {
            return aligned_random(rng, lo, hi);
        });
    }

    trace_case bursty_mixed_trace() {
        return make_trace("bursty_mixed", 220000, 1800, 0.60, 0xC0FFEEULL, [&](auto& rng, size_t step) -> size_t {
            size_t phase = step & 127U;
            if (phase < 96) {
                return aligned_random(rng, 16, 192);
            }
            return aligned_random(rng, 512, 3072);
        });
    }

    trace_case fragmentation_trace() {
        static constexpr std::array<size_t, 17> sizes{
                16, 32, 48, 64, 80, 96, 128, 160, 192, 256, 320, 384, 512, 768, 1024, 1536, 2048};
        return make_trace("fragmentation", 260000, 3500, 0.50, 0xF00DULL, [&](auto& rng, size_t) -> size_t {
            std::uniform_int_distribution<size_t> pick(0, sizes.size() - 1);
            return sizes[pick(rng)];
        });
    }

    trace_case growth_trace() {
        return make_trace("growth", 240000, 5000, 0.72, 0xDEADBEEFULL, [&](auto& rng, size_t step) -> size_t {
            if (step < 80000) {
                return aligned_random(rng, 64, 512);
            }
            if (step < 160000) {
                return aligned_random(rng, 128, 1024);
            }
            return aligned_random(rng, 256, 2048);
        });
    }

    trace_case large_run_pressure_trace() {
        return make_trace("large_run", 160000, 128, 0.64, 0x1A6EULL, [&](auto& rng, size_t step) -> size_t {
            if (step < 53333) {
                return aligned_random_to(rng, 4096, 16384, 4096);
            }
            if (step < 106666) {
                return aligned_random_to(rng, 8192, 32768, 4096);
            }
            return aligned_random_to(rng, 16384, 65536, 4096);
        });
    }

    template <typename Pool>
    void execute_trace(Pool& pool, const trace_case& trace) {
        std::vector<void*> slots(trace.slot_count, nullptr);

        for (const op& event : trace.ops) {
            if (event.kind == op_kind::allocate) {
                void* p = pool.allocate_bytes(event.bytes, alignof(std::max_align_t));
                slots[event.slot] = p;
                sink ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
                continue;
            }

            void* p = slots[event.slot];
            if (p != nullptr) {
                pool.deallocate_bytes(p);
                slots[event.slot] = nullptr;
            }
        }
    }

    template <typename PoolState>
    result_row run_workload_mode(
            const trace_case& trace,
            std::string_view mode_name,
            size_t pool_bytes,
            size_t warmup_runs,
            size_t repeats) {
        for (size_t i : std::ranges::iota_view{size_t{0}, warmup_runs}) {
            (void)i;
            PoolState state(pool_bytes);
            execute_trace(state.pool, trace);
        }

        std::vector<double> ns_samples;
        std::vector<double> alloc_ok_samples;
        std::vector<double> alloc_fail_samples;
        std::vector<double> scan_per_find_samples;
        std::vector<double> split_samples;
        std::vector<double> coalesce_samples;
        std::vector<double> peak_live_samples;

        ns_samples.reserve(repeats);
        alloc_ok_samples.reserve(repeats);
        alloc_fail_samples.reserve(repeats);
        scan_per_find_samples.reserve(repeats);
        split_samples.reserve(repeats);
        coalesce_samples.reserve(repeats);
        peak_live_samples.reserve(repeats);

        for (size_t rep : std::ranges::iota_view{size_t{0}, repeats}) {
            (void)rep;
            PoolState state(pool_bytes);
            state.pool.reset_stats();

            auto start = clock_t::now();
            execute_trace(state.pool, trace);
            auto end = clock_t::now();

            double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) /
                        static_cast<double>(trace.ops.size());
            ns_samples.push_back(ns);

            const auto& s = state.pool.stats();
            alloc_ok_samples.push_back(static_cast<double>(s.allocate_success));
            alloc_fail_samples.push_back(static_cast<double>(s.allocate_fail));
            split_samples.push_back(static_cast<double>(s.split_count));
            coalesce_samples.push_back(static_cast<double>(s.coalesce_next_count + s.coalesce_prev_count));
            peak_live_samples.push_back(static_cast<double>(s.peak_live_bytes));

            double scan_per_find = 0.0;
            if (s.find_fit_calls != 0) {
                scan_per_find = static_cast<double>(s.scanned_nodes) / static_cast<double>(s.find_fit_calls);
            }
            scan_per_find_samples.push_back(scan_per_find);
        }

        return result_row{
                trace.name,
                std::string{mode_name},
                trace.ops.size(),
                mean_of(ns_samples),
                median_of(ns_samples),
                mean_of(alloc_ok_samples),
                mean_of(alloc_fail_samples),
                mean_of(scan_per_find_samples),
                mean_of(split_samples),
                mean_of(coalesce_samples),
                mean_of(peak_live_samples),
        };
    }

    struct dynamic_pool_state {
        explicit dynamic_pool_state(size_t bytes) : pool(bytes) {}
        allocazam::runner::allocator<true, true> pool;
    };

    struct fixed_pool_state {
        explicit fixed_pool_state(size_t bytes) : pool(bytes) {}
        allocazam::runner::allocator<false, true> pool;
    };

    struct noheap_pool_state {
        explicit noheap_pool_state(size_t bytes) : backing(bytes), pool(std::span<std::byte>{backing}) {}
        std::vector<std::byte> backing;
        allocazam::runner::allocator<false, true> pool;
    };

    constexpr std::string_view workload_label_for_table(std::string_view workload) noexcept {
        using namespace std::string_view_literals;
        if (workload == "low_mid_a1")
            return "272-704"sv;
        if (workload == "low_mid_a2")
            return "720-1152"sv;
        if (workload == "low_mid_b1")
            return "1168-1600"sv;
        if (workload == "low_mid_b2")
            return "1616-2048"sv;
        if (workload == "high_mid_a1")
            return "2048-2560"sv;
        if (workload == "high_mid_a2")
            return "2576-3072"sv;
        if (workload == "high_mid_b1")
            return "3088-3584"sv;
        if (workload == "high_mid_b2")
            return "3600-4096"sv;
        return workload;
    }

    void print_results_table(const std::vector<result_row>& rows) {
        std::cout << "\nrunner benchmark summary\n";
        std::cout << std::left;
        std::cout << std::setw(15) << "workload" << std::setw(9) << "mode" << std::setw(10) << "ops" << std::setw(12)
                  << "mean ns/op" << std::setw(14) << "median ns/op" << std::setw(11) << "alloc ok" << std::setw(11)
                  << "alloc fail" << std::setw(11) << "scan/find" << std::setw(10) << "splits" << std::setw(12)
                  << "coalesce" << std::setw(12) << "peak live" << "\n";

        for (const auto& row : rows) {
            std::cout << std::setw(15) << workload_label_for_table(row.workload) << std::setw(9) << row.mode
                      << std::setw(10) << row.ops << std::setw(12) << std::fixed << std::setprecision(3)
                      << row.mean_ns_per_op << std::setw(14) << row.median_ns_per_op << std::setw(11)
                      << std::setprecision(1) << row.alloc_success << std::setw(11) << row.alloc_fail << std::setw(11)
                      << std::setprecision(3) << row.scan_per_find << std::setw(10) << std::setprecision(1)
                      << row.split_count << std::setw(12) << row.coalesce_count << std::setw(12) << row.peak_live_bytes
                      << "\n";
        }
    }
}  // namespace

int main(int argc, char** argv) {
    size_t pool_bytes = 8 * 1024 * 1024;
    size_t warmup_runs = 2;
    size_t repeats = 7;

    if (argc > 1) {
        pool_bytes = static_cast<size_t>(std::strtoull(argv[1], nullptr, 10));
    }
    if (argc > 2) {
        warmup_runs = static_cast<size_t>(std::strtoull(argv[2], nullptr, 10));
    }
    if (argc > 3) {
        repeats = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
    }

    warmup_runs = std::ranges::max(warmup_runs, size_t{1});
    repeats = std::ranges::max(repeats, size_t{1});

    std::vector<trace_case> traces;
    traces.reserve(14);
    traces.push_back(small_steady_trace());
    traces.push_back(medium_mixed_trace());
    traces.push_back(mid_band_trace("low_mid_a1", 272, 704, 0xBADA56ULL));
    traces.push_back(mid_band_trace("low_mid_a2", 720, 1152, 0xBADA57ULL));
    traces.push_back(mid_band_trace("low_mid_b1", 1168, 1600, 0xBADA58ULL));
    traces.push_back(mid_band_trace("low_mid_b2", 1616, 2048, 0xBADA59ULL));
    traces.push_back(mid_band_trace("high_mid_a1", 2048, 2560, 0xBADA5AULL));
    traces.push_back(mid_band_trace("high_mid_a2", 2576, 3072, 0xBADA5BULL));
    traces.push_back(mid_band_trace("high_mid_b1", 3088, 3584, 0xBADA5CULL));
    traces.push_back(mid_band_trace("high_mid_b2", 3600, 4096, 0xBADA5DULL));
    traces.push_back(large_run_pressure_trace());
    traces.push_back(bursty_mixed_trace());
    traces.push_back(fragmentation_trace());
    traces.push_back(growth_trace());

    std::vector<result_row> rows;
    rows.reserve(traces.size() * 3);

    std::cout << "runner benchmark config\n";
    std::cout << "  pool_bytes:   " << pool_bytes << "\n";
    std::cout << "  warmup_runs:  " << warmup_runs << "\n";
    std::cout << "  repeats:      " << repeats << "\n";
    std::cout << "  traces:       " << traces.size() << "\n";

    for (const auto& trace : traces) {
        rows.push_back(run_workload_mode<dynamic_pool_state>(trace, "dynamic", pool_bytes, warmup_runs, repeats));
        rows.push_back(run_workload_mode<fixed_pool_state>(trace, "fixed", pool_bytes, warmup_runs, repeats));
        rows.push_back(run_workload_mode<noheap_pool_state>(trace, "noheap", pool_bytes, warmup_runs, repeats));
    }

    print_results_table(rows);
    std::cout << "\nsink=" << sink << "\n";
    return 0;
}
