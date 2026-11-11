#ifndef PERF_LATENCY_HISTOGRAM_HPP
#define PERF_LATENCY_HISTOGRAM_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Fixed log-linear histogram for nanosecond latency values. Seven low bits
// divide each power-of-two interval into 128 equal-width buckets.
class perf_latency_histogram_t
{
  public:
    static const size_t sub_bucket_bits = 7;
    static const size_t sub_bucket_count = 1u << sub_bucket_bits;
    static const size_t bucket_count = 1u + 65u * sub_bucket_count;

    perf_latency_histogram_t () { reset (); }

    void reset ()
    {
        std::memset (_buckets, 0, sizeof (_buckets));
        _count = 0;
        _sum_ns = 0.0;
        _min_ns = 0.0;
        _max_ns = 0.0;
    }

    void add (double latency_ns)
    {
        const double sample = latency_ns >= 0.0 ? latency_ns : 0.0;
        const size_t index = bucket_index (sample);
        ++_buckets[index];
        ++_count;
        _sum_ns += sample;
        if (_count == 1 || sample < _min_ns)
            _min_ns = sample;
        if (_count == 1 || sample > _max_ns)
            _max_ns = sample;
    }

    void merge (const perf_latency_histogram_t &other)
    {
        for (size_t i = 0; i < bucket_count; ++i)
            _buckets[i] += other._buckets[i];
        if (other._count != 0) {
            if (_count == 0 || other._min_ns < _min_ns)
                _min_ns = other._min_ns;
            if (_count == 0 || other._max_ns > _max_ns)
                _max_ns = other._max_ns;
        }
        _count += other._count;
        _sum_ns += other._sum_ns;
    }

    void merge_buckets (const uint64_t *buckets,
                        uint64_t count,
                        double sum_ns,
                        double min_ns,
                        double max_ns)
    {
        for (size_t i = 0; i < bucket_count; ++i)
            _buckets[i] += buckets[i];
        if (count != 0) {
            if (_count == 0 || min_ns < _min_ns)
                _min_ns = min_ns;
            if (_count == 0 || max_ns > _max_ns)
                _max_ns = max_ns;
        }
        _count += count;
        _sum_ns += sum_ns;
    }

    uint64_t count () const { return _count; }
    double sum_ns () const { return _sum_ns; }
    double min_ns () const { return _min_ns; }
    double max_ns () const { return _max_ns; }
    const uint64_t *buckets () const { return _buckets; }

    double percentile (double quantile) const
    {
        if (_count == 0)
            return 0.0;
        const uint64_t target = static_cast<uint64_t> (
          std::ceil (quantile * static_cast<double> (_count)));
        uint64_t cumulative = 0;
        for (size_t i = 0; i < bucket_count; ++i) {
            cumulative += _buckets[i];
            if (cumulative >= (target == 0 ? 1 : target))
                return bucket_representative (i);
        }
        return _max_ns;
    }

  private:
    uint64_t _buckets[bucket_count];
    uint64_t _count;
    double _sum_ns;
    double _min_ns;
    double _max_ns;

    static size_t bucket_index (double value)
    {
        if (value == 0)
            return 0;
        int exponent = 0;
        const double normalized = std::frexp (value, &exponent);
        const int bucket_exponent = exponent;
        const size_t exponent_index = bucket_exponent < 0
                                        ? 0
                                        : static_cast<size_t> (bucket_exponent);
        if (exponent_index >= 65u)
            return bucket_count - 1u;
        const unsigned sub = static_cast<unsigned> (
          (normalized * 2.0 - 1.0) * sub_bucket_count);
        return 1u + exponent_index * sub_bucket_count
               + (sub < sub_bucket_count ? sub : sub_bucket_count - 1u);
    }

    static double bucket_representative (size_t index)
    {
        if (index == 0)
            return 0.0;
        const size_t positive_index = index - 1u;
        const size_t exponent = positive_index / sub_bucket_count;
        const size_t sub = positive_index % sub_bucket_count;
        const double base = std::ldexp (1.0, static_cast<int> (exponent) - 1);
        return base * (1.0 + static_cast<double> (sub + 1) / sub_bucket_count);
    }
};

#endif
