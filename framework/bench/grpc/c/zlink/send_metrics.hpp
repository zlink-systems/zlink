#ifndef ZLINK_C_BENCH_SEND_METRICS_HPP
#define ZLINK_C_BENCH_SEND_METRICS_HPP

#include "../common/bench_common.hpp"
#include <atomic>
#include <optional>
#include <stdexcept>

namespace zlink_c_bench
{
// Control requests use the command socket outside the active submission loop.
inline constexpr const char *k_send_stats_start = "bench-stats-start ";
inline constexpr const char *k_send_stats_snapshot = "bench-stats-snapshot";

// Receivers reserve independent sample slots. Control runs before submission and
// after drain, so start/snapshot never overlap record calls.
class send_metrics_t
{
  public:
    void start (uint64_t deadline, phase_t phase = phase_active)
    {
        _deadline = deadline;
        _phase = phase;
        _received = _errors = _active_received = _latency_sum_ns = 0;
        _latency_ns.resize (k_latency_sample_limit);
        _boundary.reset ();
    }

    void close (uint64_t now)
    {
        if (_deadline == 0 || now < _deadline || _boundary)
            return;
        result_t boundary;
        boundary.completed = _active_received.load ();
        boundary.mean_us = boundary.completed == 0 ? 0.0
                                                   : static_cast<double> (_latency_sum_ns.load ())
                                                       / boundary.completed / 1000.0;
        latency_sampler_t samples (k_latency_sample_limit);
        const size_t count = std::min<uint64_t> (boundary.completed, k_latency_sample_limit);
        for (size_t i = 0; i < count; ++i)
            samples.add_us (_latency_ns[i] / 1000.0);
        const auto percentiles = samples.percentiles ();
        boundary.p95_us = percentiles.first;
        boundary.p99_us = percentiles.second;
        _boundary = boundary;
    }

    void record (const void *body, size_t size, uint64_t now)
    {
        decoded_header_t header{};
        if (!decode_payload (body, size, &header) || header.payload_size != size
            || (header.phase != phase_warmup && header.phase != phase_active)) {
            ++_errors;
            return;
        }
        if (header.phase != _phase)
            return;
        ++_received;
        if (_deadline != 0 && now < _deadline) {
            const uint64_t latency = now >= header.sent_ns ? now - header.sent_ns : 0;
            const uint64_t index = _active_received.fetch_add (1);
            _latency_sum_ns.fetch_add (latency);
            if (index < _latency_ns.size ())
                _latency_ns[index] = latency;
        }
    }

    result_t snapshot (uint64_t now)
    {
        close (now);
        if (!_boundary)
            throw std::runtime_error ("send stats requested before active close");
        result_t result = *_boundary;
        result.server_received = _received.load ();
        result.server_errors = _errors.load ();
        return result;
    }

    std::optional<std::string> control (const std::string &body, uint64_t now)
    {
        if (body.rfind (k_send_stats_start, 0) == 0) {
            std::istringstream settings (body.substr (std::strlen (k_send_stats_start)));
            uint64_t deadline;
            unsigned phase;
            if (!(settings >> deadline >> phase) || phase > phase_active)
                throw std::invalid_argument ("invalid send phase settings");
            start (deadline, static_cast<phase_t> (phase));
            return std::string ();
        }
        if (body != k_send_stats_snapshot)
            return std::nullopt;
        const auto result = snapshot (now);
        std::ostringstream text;
        text.precision (17);
        text << result.completed << ' ' << result.server_received.value () << ' '
             << result.server_errors << ' ' << result.mean_us << ' ' << result.p95_us << ' '
             << result.p99_us;
        return text.str ();
    }

    static void read_snapshot (const std::string &body, result_t &result)
    {
        std::istringstream stats (body);
        uint64_t received;
        if (!(stats >> result.completed >> received >> result.server_errors >> result.mean_us
              >> result.p95_us >> result.p99_us))
            throw std::runtime_error ("invalid send stats reply");
        result.server_received = received;
        result.has_drain = true;
        result.abandoned = result.submitted > received ? result.submitted - received : 0;
        result.drain_bound_hit = result.abandoned != 0;
    }

    uint64_t deadline () const { return _deadline; }

  private:
    phase_t _phase = phase_active;
    uint64_t _deadline = 0;
    std::atomic<uint64_t> _received{0};
    std::atomic<uint64_t> _errors{0};
    std::atomic<uint64_t> _active_received{0};
    std::atomic<uint64_t> _latency_sum_ns{0};
    std::vector<uint64_t> _latency_ns;
    std::optional<result_t> _boundary;
};
}
#endif
