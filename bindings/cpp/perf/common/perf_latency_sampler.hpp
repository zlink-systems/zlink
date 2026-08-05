#ifndef ZLINK_CPP_PERF_LATENCY_SAMPLER_HPP
#define ZLINK_CPP_PERF_LATENCY_SAMPLER_HPP

#include <algorithm>
#include <cstddef>
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
    explicit latency_sampler_t (size_t reserve_hint = 200000) : _count (0), _sum (0.0)
    {
        if (reserve_hint > 0)
            _samples.reserve (reserve_hint);
    }

    void add (double latency_ns)
    {
        const double sample = latency_ns >= 0.0 ? latency_ns : 0.0;
        ++_count;
        _sum += sample;
        _samples.push_back (sample);
    }

    void merge_from (const latency_sampler_t &other)
    {
        if (other._count == 0)
            return;

        _sum += other._sum;
        _count += other._count;
        _samples.insert (_samples.end (), other._samples.begin (), other._samples.end ());
    }

    unsigned long long count () const { return _count; }

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
        if (out.p95_ns < out.mean_ns)
            out.p95_ns = out.mean_ns;
        if (out.p99_ns < out.p95_ns)
            out.p99_ns = out.p95_ns;
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

    unsigned long long _count;
    double _sum;
    std::vector<double> _samples;
};

} // namespace perf

#endif
