#ifndef PERF_MULTI_WEIGHTED_LATENCY_HPP
#define PERF_MULTI_WEIGHTED_LATENCY_HPP

#include "perf_multi_metrics.hpp"

#include <algorithm>
#include <vector>

namespace perf_multi_latency
{

struct weighted_sample_t
{
    double value;
    double weight;
};

inline double weighted_percentile (
  const std::vector<weighted_sample_t> &samples,
  double quantile)
{
    double total_weight = 0.0;
    for (size_t i = 0; i < samples.size (); ++i)
        total_weight += samples[i].weight;
    if (total_weight <= 0.0)
        return 0.0;

    const double target = total_weight * quantile;
    double cumulative = 0.0;
    for (size_t i = 0; i < samples.size (); ++i) {
        cumulative += samples[i].weight;
        if (cumulative >= target)
            return samples[i].value;
    }
    return samples.back ().value;
}

inline bench_latency_stats_t aggregate (
  uint64_t total_count,
  double total_sum,
  std::vector<weighted_sample_t> *samples)
{
    bench_latency_stats_t stats;
    if (total_count == 0)
        return stats;
    stats.mean_ns = total_sum / static_cast<double> (total_count);
    if (!samples || samples->empty ()) {
        stats.p95_ns = stats.mean_ns;
        stats.p99_ns = stats.mean_ns;
        return stats;
    }
    std::sort (
      samples->begin (), samples->end (),
      [] (const weighted_sample_t &lhs, const weighted_sample_t &rhs) {
          return lhs.value < rhs.value;
      });
    stats.p95_ns = weighted_percentile (*samples, 0.95);
    stats.p99_ns = weighted_percentile (*samples, 0.99);
    return stats;
}

}

#endif
