#ifndef PERF_SINGLE_LATENCY_HPP
#define PERF_SINGLE_LATENCY_HPP

#include "../../common/perf_latency_histogram.hpp"

struct latency_stats_t
{
    latency_stats_t () : mean_ns (0.0), p95_ns (0.0), p99_ns (0.0) {}
    double mean_ns;
    double p95_ns;
    double p99_ns;
};

class latency_stats_builder_t
{
  public:
    void add (double latency_ns_) { _histogram.add (latency_ns_); }
    unsigned long long count () const { return _histogram.count (); }
    void reset () { _histogram.reset (); }

    latency_stats_t snapshot () const
    {
        latency_stats_t out;
        if (_histogram.count () == 0)
            return out;
        out.mean_ns = _histogram.sum_ns () / static_cast<double> (_histogram.count ());
        out.p95_ns = _histogram.percentile (0.95);
        out.p99_ns = _histogram.percentile (0.99);
        return out;
    }

  private:
    perf_latency_histogram_t _histogram;
};

#endif
