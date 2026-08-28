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

inline bool append_weighted_samples (
  uint64_t represented_count,
  const double *values,
  size_t sample_count,
  std::vector<weighted_sample_t> *out)
{
    if (!out || sample_count > represented_count
        || (sample_count > 0 && !values)) {
        return false;
    }
    // A zero-cap reservoir deliberately carries no percentile samples while
    // represented_count/total_sum still preserve the exact mean.
    if (sample_count == 0)
        return true;

    const double weight = static_cast<double> (represented_count)
                          / static_cast<double> (sample_count);
    for (size_t i = 0; i < sample_count; ++i) {
        weighted_sample_t sample;
        sample.value = values[i];
        sample.weight = weight;
        out->push_back (sample);
    }
    return true;
}

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
