#ifndef ZLINK_CPP_PERF_LATENCY_SAMPLER_HPP
#define ZLINK_CPP_PERF_LATENCY_SAMPLER_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace perf
{

struct latency_sampler_stats_t
{
    latency_sampler_stats_t () : mean_ns (0.0), p95_ns (0.0), p99_ns (0.0) {}

    double mean_ns;
    double p95_ns;
    double p99_ns;
};

class latency_sampler_t
{
  public:
    explicit latency_sampler_t (size_t = 0) :
        _count (0),
        _sum (0.0),
        _sample_cap (resolve_sample_cap ()),
        _samples_seen (0),
        _rng (0xA341316Cu)
    {
        if (_sample_cap > 0)
            _samples.reserve (_sample_cap);
    }

    void add (double latency_ns)
    {
        const double sample = latency_ns >= 0.0 ? latency_ns : 0.0;
        ++_count;
        _sum += sample;
        add_sample (sample);
    }

    void reset ()
    {
        _count = 0;
        _sum = 0.0;
        _samples_seen = 0;
        _rng = 0xA341316Cu;
        _samples.clear ();
    }

    unsigned long long count () const { return _count; }
    double sum_ns () const { return _sum; }

    void append_samples (std::vector<double> *out_) const
    {
        if (!out_ || _samples.empty ())
            return;
        out_->insert (out_->end (), _samples.begin (), _samples.end ());
    }

    latency_sampler_stats_t snapshot ()
    {
        latency_sampler_stats_t out;
        if (_count == 0)
            return out;

        out.mean_ns = _sum / static_cast<double> (_count);
        if (_samples.empty ()) {
            out.p95_ns = out.mean_ns;
            out.p99_ns = out.mean_ns;
            return out;
        }

        std::sort (_samples.begin (), _samples.end ());
        out.p95_ns = percentile (_samples, 0.95);
        out.p99_ns = percentile (_samples, 0.99);
        return out;
    }

  private:
    static double percentile (const std::vector<double> &values, double q)
    {
        if (values.empty ())
            return 0.0;
        if (q <= 0.0)
            return values.front ();
        if (q >= 1.0)
            return values.back ();

        const double pos = (values.size () - 1) * q;
        const size_t lo = static_cast<size_t> (pos);
        const size_t hi = lo + 1 < values.size () ? lo + 1 : lo;
        const double frac = pos - static_cast<double> (lo);
        return values[lo] + (values[hi] - values[lo]) * frac;
    }

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

    unsigned long long _count;
    double _sum;
    size_t _sample_cap;
    unsigned long long _samples_seen;
    uint32_t _rng;
    std::vector<double> _samples;
};

} // namespace perf

#endif
