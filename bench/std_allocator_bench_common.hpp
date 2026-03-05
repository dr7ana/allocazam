#pragma once

#include "utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <numeric>

namespace allocazam_bench {
    using clock_t = std::chrono::steady_clock;

    struct run_stats {
        std::vector<double> std_samples;
        std::vector<double> allocazam_samples;
        size_t ops{};
        double std_mean{};
        double allocazam_mean{};
        double std_median{};
        double allocazam_median{};
    };

    struct granular_row {
        std::string workload;
        size_t unit_count{};
        size_t bytes_per_iter{};
        size_t iterations{};
        size_t ops{};
        double std_median{};
        double allocazam_median{};
        double improvement{};
    };

    inline uint64_t& sink() {
        static uint64_t value = 0;
        return value;
    }

    inline void print_percent_improvement(
            std::string_view label, double baseline_ns_per_op, double candidate_ns_per_op) {
        double percent = ((baseline_ns_per_op - candidate_ns_per_op) / baseline_ns_per_op) * 100.0;
        std::cout << "  " << label << ": ";
        std::cout << std::showpos << std::fixed << std::setprecision(2) << percent << "%" << std::noshowpos << "\n";
    }

    [[nodiscard]] inline double percent_improvement(double baseline_ns_per_op, double candidate_ns_per_op) {
        return ((baseline_ns_per_op - candidate_ns_per_op) / baseline_ns_per_op) * 100.0;
    }

    inline double mean_of(const std::vector<double>& values) {
        if (values.empty()) {
            return 0.0;
        }
        return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    }

    inline double median_of(std::vector<double> values) {
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

    template <typename std_fn_t, typename allocazam_fn_t>
    run_stats run_robust_benchmark(size_t repeats, size_t ops, std_fn_t&& std_fn, allocazam_fn_t&& allocazam_fn) {
        run_stats stats{};
        stats.ops = ops;
        stats.std_samples.reserve(repeats);
        stats.allocazam_samples.reserve(repeats);

        for (size_t rep : std::ranges::iota_view{size_t{0}, repeats}) {
            bool std_first = (rep & 1U) == 0U;
            if (std_first) {
                stats.std_samples.push_back(std_fn());
                stats.allocazam_samples.push_back(allocazam_fn());
            } else {
                stats.allocazam_samples.push_back(allocazam_fn());
                stats.std_samples.push_back(std_fn());
            }
        }

        stats.std_mean = mean_of(stats.std_samples);
        stats.allocazam_mean = mean_of(stats.allocazam_samples);
        stats.std_median = median_of(stats.std_samples);
        stats.allocazam_median = median_of(stats.allocazam_samples);
        return stats;
    }

    inline void print_run_stats(std::string_view label, const run_stats& stats) {
        std::cout << label << "\n";
        std::cout << "  ops per run:                 " << stats.ops << "\n";
        std::cout << "  std mean ns/op:              " << stats.std_mean << "\n";
        std::cout << "  allocazam mean ns/op:           " << stats.allocazam_mean << "\n";
        std::cout << "  std median ns/op:            " << stats.std_median << "\n";
        std::cout << "  allocazam median ns/op:         " << stats.allocazam_median << "\n";
    }

    [[nodiscard]] inline std::vector<size_t> build_granular_sizes(size_t base) {
        std::array<std::pair<size_t, size_t>, 8> ratios{
                std::pair{size_t{1}, size_t{4}},
                std::pair{size_t{1}, size_t{2}},
                std::pair{size_t{3}, size_t{4}},
                std::pair{size_t{1}, size_t{1}},
                std::pair{size_t{5}, size_t{4}},
                std::pair{size_t{3}, size_t{2}},
                std::pair{size_t{2}, size_t{1}},
                std::pair{size_t{3}, size_t{1}},
        };

        std::vector<size_t> sizes;
        sizes.reserve(ratios.size() + 4);

        for (const auto& ratio : ratios) {
            size_t value = std::ranges::max(size_t{1}, (base * ratio.first) / ratio.second);
            sizes.push_back(value);
        }

        sizes.push_back(base);
        std::ranges::sort(sizes);
        auto unique_end = std::ranges::unique(sizes).begin();
        sizes.erase(unique_end, sizes.end());
        return sizes;
    }

    inline void print_granular_table(std::string_view title, const std::vector<granular_row>& rows) {
        std::cout << "\n" << title << "\n";
        std::cout << std::left;
        std::cout << std::setw(12) << "workload" << std::setw(12) << "units/iter" << std::setw(12) << "bytes/iter"
                  << std::setw(12) << "iterations" << std::setw(13) << "ops" << std::setw(14) << "std median"
                  << std::setw(14) << "allocazam med" << std::setw(11) << "improv %" << "\n";

        for (const auto& row : rows) {
            std::cout << std::setw(12) << row.workload << std::setw(12) << row.unit_count << std::setw(12)
                      << row.bytes_per_iter << std::setw(12) << row.iterations << std::setw(13) << row.ops
                      << std::setw(14) << std::fixed << std::setprecision(6) << row.std_median << std::setw(14)
                      << row.allocazam_median << std::setw(11) << std::setprecision(2) << row.improvement << "\n";
        }
    }

    [[nodiscard]] inline size_t parse_size_arg(int argc, char** argv, int index, size_t fallback) {
        if (argc > index) {
            return static_cast<size_t>(std::strtoull(argv[index], nullptr, 10));
        }
        return fallback;
    }

    [[nodiscard]] inline size_t at_least_one(size_t value) {
        return std::ranges::max(value, size_t{1});
    }
}  // namespace allocazam_bench
