#ifndef PERF_MULTI_MEASUREMENT_HPP
#define PERF_MULTI_MEASUREMENT_HPP

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace perf_multi_measurement
{
static const uint64_t settle_ns = 500000000ULL;

// Throughput remains the saturated receive window. Latency uses the same
// received records/active second at 80%; independent senders share that rate
// in proportion to their in-window admissions.
// After bounded drain, schedule at epoch + k/rate; discard the first 0.5s
// by actual send timestamp, then collect latency_duration_seconds of samples.
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
                              double paced_received_rate)
{
    std::ostringstream line;
    line << std::setprecision (12) << "BENCH_DIAG," << pattern << ',' << msg_size
         << ",active_sender_rate," << (active_seconds > 0 ? active_sends / active_seconds : 0)
         << ",target_sender_rate," << schedule.rate << ",latency_sends," << schedule.accepted_count
         << ",late_sends_gt_1ms," << schedule.late_count << ",received_rate,"
         << (active_seconds > 0 ? active_received / active_seconds : 0) << ",paced_received_rate,"
         << paced_received_rate;
    std::cout << line.str () << std::endl;
}
}
#endif
