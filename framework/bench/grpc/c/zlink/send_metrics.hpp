#ifndef ZLINK_C_BENCH_SEND_METRICS_HPP
#define ZLINK_C_BENCH_SEND_METRICS_HPP

#include "../common/bench_common.hpp"
#include <optional>
#include <stdexcept>

namespace zlink_c_bench
{
// Control requests use the command socket outside the active submission loop.
inline constexpr const char *k_send_stats_start = "bench-stats-start ";
inline constexpr const char *k_send_stats_snapshot = "bench-stats-snapshot";
inline constexpr size_t k_send_latency_samples = 200000;

// The target receive thread owns both the live counters and the active snapshot.
class send_metrics_t
{
  public:
    void start (uint64_t deadline)
    {
        _deadline = deadline;
        _received = _errors = 0;
        _latency = latency_sampler_t (k_send_latency_samples);
        _boundary.reset ();
    }

    void close (uint64_t now)
    {
        if (_deadline == 0 || now < _deadline || _boundary)
            return;
        result_t boundary;
        boundary.completed = _received;
        boundary.mean_us = _latency.mean_us ();
        const auto percentiles = _latency.percentiles ();
        boundary.p95_us = percentiles.first;
        boundary.p99_us = percentiles.second;
        _boundary = boundary;
    }

    void record (const void *body, size_t size, uint64_t now)
    {
        close (now);
        decoded_header_t header{};
        if (!decode_payload (body, size, &header) || header.payload_size != size
            || (header.phase != phase_warmup && header.phase != phase_active)) {
            ++_errors;
            return;
        }
        if (header.phase != phase_active)
            return;
        ++_received;
        if (!_boundary)
            _latency.add_us (now >= header.sent_ns ? (now - header.sent_ns) / 1000.0 : 0);
    }

    result_t snapshot (uint64_t now)
    {
        close (now);
        if (!_boundary)
            throw std::runtime_error ("send stats requested before active close");
        result_t result = *_boundary;
        result.server_received = _received;
        result.server_errors = _errors;
        return result;
    }

    uint64_t deadline () const { return _deadline; }

  private:
    uint64_t _deadline = 0;
    uint64_t _received = 0;
    uint64_t _errors = 0;
    latency_sampler_t _latency{0};
    std::optional<result_t> _boundary;
};
}
#endif
