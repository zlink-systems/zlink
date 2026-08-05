#ifndef PERF_SINGLE_PHASE_HPP
#define PERF_SINGLE_PHASE_HPP

#include "perf_single_metric_header.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>

inline double single_latency_ns (const perf_single_metric::header_t &header_, double factor_ = 1.0)
{
    const uint64_t now_ns = perf_single_metric::now_ns ();
    if (header_.sent_ts_ns <= 0 || now_ns < static_cast<uint64_t> (header_.sent_ts_ns)) {
        return 0.0;
    }
    return static_cast<double> (now_ns - static_cast<uint64_t> (header_.sent_ts_ns)) * factor_;
}

template <typename StateT>
inline bool single_header_matches_run (const StateT &state_,
                                       const perf_single_metric::header_t &header_)
{
    return perf_single_metric::is_expected (header_, state_.run_id,
                                            perf_single_metric::phase_active, state_.msg_size);
}

template <typename StateT>
inline bool single_record_active_header (StateT *state_,
                                         const perf_single_metric::header_t &header_,
                                         double latency_factor_ = 1.0)
{
    if (!state_ || !single_header_matches_run (*state_, header_))
        return false;

    state_->active_received.fetch_add (1, std::memory_order_relaxed);
    state_->latency.add (single_latency_ns (header_, latency_factor_));
    return true;
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
