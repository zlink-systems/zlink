#ifndef PERF_MULTI_MEASUREMENT_HPP
#define PERF_MULTI_MEASUREMENT_HPP

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <sstream>
#include "perf_metric_header.hpp"

namespace perf_multi_measurement
{
static const uint64_t settle_ns = 500000000ULL;
static constexpr std::chrono::nanoseconds settle_duration (settle_ns);
static const uint64_t catch_up_window_ns = 500000000ULL;
static const uint64_t recheck_below_ms = 1;
static const uint64_t max_wait_ms = 50;
static const uint64_t ns_per_ms = 1000000ULL;

inline bool is_paced_receive_phase (uint8_t phase)
{
    return phase == perf_metric::phase_warmup || phase == perf_metric::phase_latency;
}

class catch_up_tracker_t
{
  public:
    explicit catch_up_tracker_t (double target_rate) :
        _target_rate (target_rate),
        _drain_start_ns (0),
        _in_flight_at_catch_up (0),
        _draining (false),
        _caught_up (false)
    {
    }

    bool observe (uint64_t now_ns, uint64_t received, uint64_t sent)
    {
        if (_caught_up || _target_rate <= 0.0)
            return _caught_up;
        const uint64_t backlog = sent > received ? sent - received : 0;
        const double backlog_bound = _target_rate * 0.001 > 1.0 ? _target_rate * 0.001 : 1.0;
        if (backlog > backlog_bound) {
            _draining = false;
            _drain_start_ns = 0;
            return false;
        }
        if (!_draining) {
            _draining = true;
            _drain_start_ns = now_ns;
            return false;
        }
        if (now_ns - _drain_start_ns >= catch_up_window_ns) {
            _caught_up = true;
            _in_flight_at_catch_up = backlog;
        }
        return _caught_up;
    }

    bool caught_up () const { return _caught_up; }
    uint64_t in_flight_at_catch_up () const { return _in_flight_at_catch_up; }

  private:
    double _target_rate;
    uint64_t _drain_start_ns;
    uint64_t _in_flight_at_catch_up;
    bool _draining;
    bool _caught_up;
};

// Throughput remains the saturated receive window. Latency uses the same
// received records/active second at 80%; independent senders share that rate
// in proportion to their in-window admissions.
// After bounded drain, schedule at epoch + k/rate. Non-PUBSUB patterns use the
// fixed settling window below; PUBSUB uses catch_up_tracker_t before sampling.
inline double target_rate (uint64_t active_received, double active_seconds)
{
    return active_seconds > 0.0 ? active_received / active_seconds * 0.8 : 0.0;
}

inline uint64_t scheduled_count (double target_rate, uint64_t epoch_ns, uint64_t now_ns)
{
    return now_ns > epoch_ns ? static_cast<uint64_t> (target_rate * ((now_ns - epoch_ns) / 1e9))
                             : 0;
}

inline bool latency_sample (uint64_t sent_ns, uint64_t epoch_ns, double duration_seconds)
{
    return sent_ns >= epoch_ns + settle_ns
           && sent_ns
                < epoch_ns + settle_ns + static_cast<uint64_t> (duration_seconds * 1000000000.0);
}

struct schedule_t
{
    schedule_t (double rate_, uint64_t epoch_ns_) :
        accepted_count (0), late_count (0), rate (rate_), epoch_ns (epoch_ns_)
    {
    }

    uint64_t next_ns () const
    {
        return epoch_ns
               + (rate > 0.0 ? static_cast<uint64_t> (static_cast<long double> (_submitted_count)
                                                      * 1000000000.0L / rate)
                             : 0);
    }

    bool due (uint64_t now_ns) const { return rate > 0.0 && now_ns >= next_ns (); }

    // Paced poll wait (PERF_MULTI_TEST_POLICY.md 5.1, C schedule_t::wait_ms): floor to ms,
    // cap at max_wait_ms, and 0 (recheck now) when less than recheck_below_ms remains so the
    // runner never sleeps past the next send.
    static std::chrono::milliseconds wait_ms (uint64_t now_ns, uint64_t end_ns)
    {
        if (now_ns >= end_ns)
            return std::chrono::milliseconds::zero ();
        const uint64_t ms = (end_ns - now_ns) / ns_per_ms;
        if (ms < recheck_below_ms)
            return std::chrono::milliseconds::zero ();
        return std::chrono::milliseconds (std::min (ms, max_wait_ms));
    }

    // The binding may retain a submission and admit it after another socket.
    // Keep its original due time for the latency diagnostic.
    uint64_t submitted ()
    {
        const uint64_t due_ns = next_ns ();
        ++_submitted_count;
        return due_ns;
    }

    void accepted (uint64_t now_ns, uint64_t due_ns)
    {
        if (now_ns > due_ns && now_ns - due_ns > 1000000ULL)
            ++late_count;
        ++accepted_count;
    }

    void accepted (uint64_t now_ns) { accepted (now_ns, submitted ()); }

    uint64_t accepted_count;
    uint64_t late_count;
    double rate;
    uint64_t epoch_ns;

  private:
    uint64_t _submitted_count = 0;
};

inline void print_diagnostic (const char *pattern,
                              size_t msg_size,
                              uint64_t active_sends,
                              double active_seconds,
                              const schedule_t &schedule,
                              uint64_t active_received,
                              double paced_received_rate,
                              bool caught_up = false,
                              double catch_up_seconds = -1.0)
{
    std::ostringstream line;
    line << std::setprecision (12) << "BENCH_DIAG," << pattern << ',' << msg_size
         << ",active_sender_rate," << (active_seconds > 0 ? active_sends / active_seconds : 0)
         << ",target_sender_rate," << schedule.rate << ",latency_sends," << schedule.accepted_count
         << ",late_sends_gt_1ms," << schedule.late_count << ",received_rate,"
         << (active_seconds > 0 ? active_received / active_seconds : 0) << ",paced_received_rate,"
         << paced_received_rate << ",caught_up," << (caught_up ? "yes" : "no")
         << ",catch_up_seconds," << catch_up_seconds;
    std::cout << line.str () << std::endl;
}
}
#endif
