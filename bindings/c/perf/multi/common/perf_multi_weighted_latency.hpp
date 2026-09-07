#ifndef PERF_MULTI_WEIGHTED_LATENCY_HPP
#define PERF_MULTI_WEIGHTED_LATENCY_HPP

#include "perf_multi_metrics.hpp"

#include <algorithm>
#include <cmath>
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

// PERF_POLICY.md § 1.1: the sample that occupies position `p` on the
// cumulative-weight axis is the first sample whose `c_i - 1 >= p`, where
// `c_i` is the running weight sum over the value-sorted samples.
inline double weighted_sample_at (
  const std::vector<weighted_sample_t> &samples,
  double position)
{
    double cumulative = 0.0;
    for (size_t i = 0; i < samples.size (); ++i) {
        cumulative += samples[i].weight;
        if (cumulative - 1.0 >= position)
            return samples[i].value;
    }
    return samples.back ().value;
}

// PERF_POLICY.md § 1.1: percentiles use one interpolation formula on every
// path. The merged reservoir path performs the same linear interpolation as
// the single-process path, but on the cumulative-weight axis: with `W` the
// total weight, `pos = (W - 1) * q`, `lo = floor(pos)`, `hi = min(lo + 1,
// W - 1)`, `value = s(lo) + (s(hi) - s(lo)) * (pos - lo)`. When every weight
// is 1 this is identical to `pos = (n - 1) * q` over the sorted samples.
inline double weighted_percentile (
  const std::vector<weighted_sample_t> &samples,
  double quantile)
{
    if (samples.empty ())
        return 0.0;

    double total_weight = 0.0;
    for (size_t i = 0; i < samples.size (); ++i)
        total_weight += samples[i].weight;
    if (total_weight <= 0.0)
        return 0.0;

    const double max_position = total_weight - 1.0;
    if (max_position <= 0.0)
        return samples.front ().value;
    if (quantile <= 0.0)
        return samples.front ().value;
    if (quantile >= 1.0)
        return samples.back ().value;

    const double pos = max_position * quantile;
    const double lo_position = std::floor (pos);
    const double hi_position =
      lo_position + 1.0 < max_position ? lo_position + 1.0 : max_position;
    const double lo_value = weighted_sample_at (samples, lo_position);
    const double hi_value = weighted_sample_at (samples, hi_position);
    const double frac = pos - lo_position;
    return lo_value + (hi_value - lo_value) * frac;
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
