#ifndef PERF_SINGLE_PHASE_HPP
#define PERF_SINGLE_PHASE_HPP

#include "perf_single_metric_header.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>

// PERF_POLICY.md § 1.1: sent_ts_ns and the receive instant are read from the
// same monotonic time source, so the difference is a valid one-way latency.
// The receive instant is passed in explicitly because PERF_SINGLE_TEST_POLICY
// § 2.1 uses that very instant to decide active-window membership; latency and
// throughput must therefore be derived from one timestamp per message.
inline double single_latency_ns_at (const perf_single_metric::header_t &header_,
                                    uint64_t recv_ts_ns_,
                                    double factor_ = 1.0)
{
    if (header_.sent_ts_ns <= 0 || recv_ts_ns_ < static_cast<uint64_t> (header_.sent_ts_ns)) {
        return 0.0;
    }
    return static_cast<double> (recv_ts_ns_ - static_cast<uint64_t> (header_.sent_ts_ns)) * factor_;
}

template <typename StateT>
inline bool single_header_matches_run (const StateT &state_,
                                       const perf_single_metric::header_t &header_)
{
    return perf_single_metric::is_expected (header_, state_.run_id,
                                            perf_single_metric::phase_active, state_.msg_size);
}

inline bool single_perf_validate_recv_mode_for_pattern (const char *pattern)
{
    if (!pattern || !*pattern)
        return false;

    const char *mode = std::getenv ("PERF_RECV_MODE");
    if (!mode || !*mode)
        return true;

    std::string normalized (mode);
    std::transform (normalized.begin (), normalized.end (), normalized.begin (), ::tolower);
    if (normalized == "recv")
        return true;

    std::cerr << "policy violation: single perf supports recv only" << " pattern=" << pattern
              << std::endl;
    return false;
}

#endif
