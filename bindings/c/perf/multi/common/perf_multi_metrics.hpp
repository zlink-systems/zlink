#ifndef PERF_MULTI_METRICS_HPP
#define PERF_MULTI_METRICS_HPP

#include "perf_common_multi.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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
    bench_latency_sampler_t () :
        _count (0),
        _sum_ns (0.0),
        _sample_cap (resolve_sample_cap ()),
        _samples_seen (0),
        _rng (0xA341316Cu)
    {
        if (_sample_cap > 0)
            _samples.reserve (_sample_cap);
    }

    void add (double latency_ns_)
    {
        const double sample = latency_ns_ >= 0.0 ? latency_ns_ : 0.0;
        ++_count;
        _sum_ns += sample;
        add_sample (sample);
    }

    void reset ()
    {
        _count = 0;
        _sum_ns = 0.0;
        _samples_seen = 0;
        _rng = 0xA341316Cu;
        _samples.clear ();
    }

    unsigned long long count () const { return _count; }
    double sum_ns () const { return _sum_ns; }

    void append_samples (std::vector<double> *out_) const
    {
        if (!out_ || _samples.empty ())
            return;
        out_->insert (out_->end (), _samples.begin (), _samples.end ());
    }

    bench_latency_stats_t snapshot ()
    {
        bench_latency_stats_t out;
        if (_count == 0)
            return out;

        out.mean_ns = _sum_ns / static_cast<double> (_count);
        if (_samples.empty ()) {
            out.p95_ns = out.mean_ns;
            out.p99_ns = out.mean_ns;
            return out;
        }

        std::sort (_samples.begin (), _samples.end ());
        out.p95_ns = percentile_from_sorted (_samples, 0.95);
        out.p99_ns = percentile_from_sorted (_samples, 0.99);
        return out;
    }

  private:
    static double percentile_from_sorted (const std::vector<double> &sorted_, double q_)
    {
        if (sorted_.empty ())
            return 0.0;
        if (q_ <= 0.0)
            return sorted_.front ();
        if (q_ >= 1.0)
            return sorted_.back ();

        const double pos = (sorted_.size () - 1) * q_;
        const size_t lo = static_cast<size_t> (pos);
        const size_t hi = lo + 1 < sorted_.size () ? lo + 1 : lo;
        const double frac = pos - static_cast<double> (lo);
        return sorted_[lo] + (sorted_[hi] - sorted_[lo]) * frac;
    }

    unsigned long long _count;
    double _sum_ns;
    size_t _sample_cap;
    unsigned long long _samples_seen;
    uint32_t _rng;
    std::vector<double> _samples;

    static size_t resolve_sample_cap ()
    {
        const char *value = std::getenv ("PERF_MULTI_LATENCY_SAMPLE_CAP");
        if (!value || !*value)
            return 65536;

        char *end = NULL;
        const unsigned long long parsed = std::strtoull (value, &end, 10);
        if (!end || *end != '\0')
            return 65536;
        return static_cast<size_t> (parsed);
    }

    uint32_t next_random ()
    {
        _rng = (_rng * 1664525u) + 1013904223u;
        return _rng;
    }

    void add_sample (double sample_)
    {
        ++_samples_seen;
        if (_sample_cap == 0)
            return;
        if (_samples.size () < _sample_cap) {
            _samples.push_back (sample_);
            return;
        }

        const unsigned long long slot = next_random () % _samples_seen;
        if (slot < _samples.size ())
            _samples[static_cast<size_t> (slot)] = sample_;
    }
};

inline bool is_echo_pattern (const std::string &pattern)
{
    std::string normalized = pattern;
    if (normalized.compare (0, 6, "MULTI_") == 0)
        normalized.erase (0, 6);
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
    if (normalized_pattern.compare (0, 6, "MULTI_") == 0)
        normalized_pattern.erase (0, 6);
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
              << ",latency," << std::fixed << std::setprecision (3) << latency_ms << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p95," << std::fixed << std::setprecision (3) << latency_p95_ms
              << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p99," << std::fixed << std::setprecision (3) << latency_p99_ms
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
