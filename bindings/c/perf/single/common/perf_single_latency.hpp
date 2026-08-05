#ifndef PERF_SINGLE_LATENCY_HPP
#define PERF_SINGLE_LATENCY_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

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
    latency_stats_builder_t () : latency_stats_builder_t (default_sample_cap ()) {}

    explicit latency_stats_builder_t (size_t sample_cap_) :
        _count (0),
        _sum_ns (0.0),
        _sample_cap (sample_cap_),
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
        if (_sample_cap == 0)
            return;
        if (_samples.size () < _sample_cap) {
            _samples.push_back (sample);
            return;
        }
        const unsigned long long slot = next_random () % _count;
        if (slot < _samples.size ())
            _samples[static_cast<size_t> (slot)] = sample;
    }

    unsigned long long count () const { return _count; }

    latency_stats_t snapshot ()
    {
        latency_stats_t out;
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
    uint32_t _rng;
    std::vector<double> _samples;

    uint32_t next_random ()
    {
        _rng = (_rng * 1664525u) + 1013904223u;
        return _rng;
    }

    static size_t default_sample_cap ()
    {
        const char *value = std::getenv ("PERF_SINGLE_LATENCY_SAMPLE_CAP");
        if (!value || !*value)
            return 1000000;

        char *end = NULL;
        const unsigned long long parsed = std::strtoull (value, &end, 10);
        if (!end || *end != '\0')
            return 1000000;
        return static_cast<size_t> (parsed);
    }
};

#endif
