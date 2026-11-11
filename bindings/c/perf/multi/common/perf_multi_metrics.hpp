#ifndef PERF_MULTI_METRICS_HPP
#define PERF_MULTI_METRICS_HPP

#include "perf_common_multi.hpp"
#include "../../common/perf_latency_histogram.hpp"

#include <iomanip>
#include <iostream>
#include <string>

struct bench_latency_stats_t
{
    bench_latency_stats_t () : mean_ns (0.0), p95_ns (0.0), p99_ns (0.0) {}
    union
    {
        double mean_ns;
        double mean_us;
    };
    union
    {
        double p95_ns;
        double p95_us;
    };
    union
    {
        double p99_ns;
        double p99_us;
    };
};

class bench_latency_sampler_t
{
  public:
    void add (double latency_ns_) { _histogram.add (latency_ns_); }
    void reset () { _histogram.reset (); }
    unsigned long long count () const { return _histogram.count (); }
    double sum_ns () const { return _histogram.sum_ns (); }
    double min_ns () const { return _histogram.min_ns (); }
    double max_ns () const { return _histogram.max_ns (); }
    const uint64_t *buckets () const { return _histogram.buckets (); }
    void merge_buckets (const uint64_t *buckets_,
                        uint64_t count_,
                        double sum_ns_,
                        double min_ns_,
                        double max_ns_)
    {
        _histogram.merge_buckets (buckets_, count_, sum_ns_, min_ns_, max_ns_);
    }
    void merge (const bench_latency_sampler_t &other)
    {
        _histogram.merge (other._histogram);
    }

    bench_latency_stats_t snapshot () const
    {
        bench_latency_stats_t out;
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

inline bool is_echo_pattern (const std::string &pattern)
{
    std::string normalized = pattern;
    if (normalized == "DEALER_ROUTER")
        normalized = "DEALER_ROUTER_SENDSEND";
    else if (normalized == "ROUTER_ROUTER")
        normalized = "ROUTER_ROUTER_SENDSEND";
    return normalized == "DEALER_ROUTER_SENDSEND"
           || normalized == "ROUTER_ROUTER_SENDSEND"
           || normalized == "DEALER_ROUTER_REQREP"
           || normalized == "ROUTER_ROUTER_REQREP"
           || normalized == "STREAM";
}

inline double latency_sample_ns (const std::string &pattern, uint64_t elapsed_ns)
{
    // Echo patterns measure an estimated one-way latency from a round trip.
    // One-way patterns already carry a single-leg elapsed time.
    return static_cast<double> (elapsed_ns) * (is_echo_pattern (pattern) ? 0.5 : 1.0);
}

inline double throughput_per_second (uint64_t count, double duration_seconds)
{
    if (duration_seconds <= 0.0)
        return 0.0;
    return static_cast<double> (count) / duration_seconds;
}

inline double bandwidth_mb_per_second (
  const std::string &pattern,
  size_t payload_size,
  double throughput)
{
    const double direction_factor = is_echo_pattern (pattern) ? 2.0 : 1.0;
    return (throughput * static_cast<double> (payload_size) * direction_factor)
           / 1000000.0;
}

inline void print_result (const std::string &lib_type,
                          const std::string &pattern,
                          const std::string &transport,
                          size_t size,
                          double throughput,
                          double latency_ns,
                          double latency_p95_ns,
                          double latency_p99_ns)
{
    std::string normalized_pattern = pattern;
    if (normalized_pattern == "DEALER_ROUTER")
        normalized_pattern = "DEALER_ROUTER_SENDSEND";
    else if (normalized_pattern == "ROUTER_ROUTER")
        normalized_pattern = "ROUTER_ROUTER_SENDSEND";

    const double bandwidth_mb_s =
      bandwidth_mb_per_second (normalized_pattern, size, throughput);
    const double latency_ms = latency_ns / 1000000.0;
    const double latency_p95_ms = latency_p95_ns / 1000000.0;
    const double latency_p99_ms = latency_p99_ns / 1000000.0;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",throughput," << std::fixed << std::setprecision (3) << throughput << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",bandwidth," << std::fixed << std::setprecision (3) << bandwidth_mb_s
              << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency," << std::fixed << std::setprecision (6) << latency_ms << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p95," << std::fixed << std::setprecision (6) << latency_p95_ms
              << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p99," << std::fixed << std::setprecision (6) << latency_p99_ms
              << std::endl;
}

template <typename MetricsT>
inline void print_server_metrics_for_sizes (const std::string &lib_type,
                                            const std::string &pattern,
                                            const std::string &transport,
                                            const std::vector<size_t> &sizes,
                                            const MetricsT &metrics)
{
    (void) lib_type;
    (void) pattern;
    (void) transport;
    (void) sizes;
    (void) metrics;
}

inline void print_result (const std::string &lib_type,
                          const std::string &pattern,
                          const std::string &transport,
                          size_t size,
                          double throughput,
                          double latency)
{
    print_result (lib_type, pattern, transport, size, throughput, latency, latency, latency);
}

#endif
