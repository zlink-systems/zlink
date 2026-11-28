#ifndef PERF_MULTI_MEASUREMENT_HPP
#define PERF_MULTI_MEASUREMENT_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include "perf_multi_metric_header.hpp"

namespace perf_multi_measurement
{
static const uint64_t settle_ns = 500000000ULL;
static const uint64_t catch_up_window_ns = 500000000ULL;
// Paced poll waits: floor to ms and cap at max_wait_ms. Under recheck_below_ms the wait is 0,
// so the sender rechecks right away instead of oversleeping the next send.
static const int recheck_below_ms = 1;
static const int max_wait_ms = 50;
static const uint64_t ns_per_ms = 1000000ULL;

static const size_t max_burst_per_sender = 64;

// Keep the current burst across poll turns until it fills or the sender yields.
class sender_rotation_t
{
  public:
    explicit sender_rotation_t (size_t sender_count) : _sender_count (sender_count) {}

    size_t index () const { return _index; }

    void advance ()
    {
        if (++_index == _sender_count)
            _index = 0;
        _accepted = 0;
    }

    bool accepted ()
    {
        if (++_accepted < max_burst_per_sender)
            return false;
        advance ();
        return true;
    }

  private:
    const size_t _sender_count;
    size_t _index = 0;
    size_t _accepted = 0;
};

inline bool is_paced_receive_phase (uint8_t phase)
{
    return phase == perf_multi_metric::phase_warmup || phase == perf_multi_metric::phase_latency;
}

class catch_up_tracker_t
{
  public:
    static constexpr uint64_t receive_quantum = 1;

    explicit catch_up_tracker_t (double target_rate) : _target_rate (target_rate)
    {
    }

    bool observe (uint64_t now_ns, const std::vector<uint64_t> &received, uint64_t scheduled)
    {
        if (_caught_up || _target_rate <= 0.0 || received.empty ())
            return _caught_up;

        _min_received = *std::min_element (received.begin (), received.end ());
        _in_flight = scheduled > _min_received ? scheduled - _min_received : 0;
        if (!_window_received.empty () && _in_flight <= _window_start_in_flight) {
            const uint64_t elapsed_ns = now_ns - _window_start_ns;
            if (elapsed_ns < catch_up_window_ns)
                return false;

            // Subscriber order is fixed. The minimum cumulative count may
            // belong to a different subscriber at each end of the window.
            uint64_t min_received_delta = received[0] - _window_received[0];
            for (size_t i = 1; i < received.size (); ++i)
                min_received_delta =
                  std::min (min_received_delta, received[i] - _window_received[i]);
            // The target rate is compared as counts on the same schedule: the
            // slowest subscriber received what was scheduled in the window. A
            // subscriber observed just after one arrival and just before the
            // next is one message short without falling behind, so one message
            // of observation quantization is allowed.
            if (min_received_delta + receive_quantum >= scheduled - _window_scheduled) {
                _caught_up = true;
                return true;
            }
        }

        _window_start_ns = now_ns;
        _window_start_in_flight = _in_flight;
        _window_scheduled = scheduled;
        _window_received = received;
        return false;
    }

    bool caught_up () const { return _caught_up; }
    uint64_t min_received () const { return _min_received; }
    uint64_t in_flight () const { return _in_flight; }
    uint64_t in_flight_at_catch_up () const { return _caught_up ? _in_flight : 0; }
    // Any observation above the starting in-flight begins a new window, so
    // the start value is also the upper bound of the window.
    uint64_t window_start_in_flight () const { return _window_start_in_flight; }

  private:
    const double _target_rate;
    std::vector<uint64_t> _window_received;
    uint64_t _window_start_ns = 0;
    uint64_t _window_start_in_flight = 0;
    uint64_t _window_scheduled = 0;
    uint64_t _min_received = 0;
    uint64_t _in_flight = 0;
    bool _caught_up = false;
};

// Throughput remains the saturated receive window. Latency uses the same
// received records/active second at 80%; independent senders share that rate
// in proportion to their in-window admissions.
// After bounded drain, schedule at epoch + k/rate. Non-PUBSUB patterns use the
// fixed settling window below; PUBSUB uses catch_up_tracker_t before sampling.
inline double target_rate (uint64_t active_received,
                           double active_seconds,
                           uint64_t sender_accepted = 1,
                           uint64_t total_accepted = 1)
{
    return active_seconds > 0.0 && total_accepted > 0
             ? active_received / active_seconds * 0.8
                 * (static_cast<double> (sender_accepted) / total_accepted)
             : 0.0;
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
               + (rate > 0.0 ? static_cast<uint64_t> (static_cast<long double> (accepted_count)
                                                      * 1000000000.0L / rate)
                             : 0);
    }
    bool due (uint64_t now_ns) const { return rate > 0.0 && now_ns >= next_ns (); }
    // Wait until the next send or end_ns, whichever comes first. 0 means recheck now: the
    // deadline has passed or less than recheck_below_ms remains.
    int wait_ms (uint64_t now_ns, uint64_t end_ns) const
    {
        const uint64_t until = end_ns < next_ns () ? end_ns : next_ns ();
        if (now_ns >= until)
            return 0;
        const uint64_t ms = (until - now_ns) / ns_per_ms;
        return ms < static_cast<uint64_t> (recheck_below_ms) ? 0
               : ms > static_cast<uint64_t> (max_wait_ms)    ? max_wait_ms
                                                             : static_cast<int> (ms);
    }
    void accepted (uint64_t now_ns)
    {
        if (now_ns > next_ns () && now_ns - next_ns () > 1000000ULL)
            ++late_count;
        ++accepted_count;
    }

    uint64_t accepted_count;
    uint64_t late_count;
    double rate;
    uint64_t epoch_ns;
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
