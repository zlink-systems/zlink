#ifndef PERF_MULTI_ECHO_POLICY_HPP
#define PERF_MULTI_ECHO_POLICY_HPP

#include "perf_multi_client_helpers.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <vector>

namespace perf_multi_echo_policy
{

// Structural contract:
// - state: fatal, fatal_errno, collect_active, active_run_id,
//          active_msg_size, active_received
// - slot: send_enabled, auto_send_on_recv, send_pending, inflight,
//         run_id, msg_size, phase, next_seq, poller_events, latency
//
// Recv-mode invariant:
// - Pattern files still own poller wait timing and recv drain execution.
// - After POLLIN, patterns must keep their DONTWAIT recv drain loop.

template <typename Slot> inline void echo_reset_slot_latency (Slot *slot)
{
    slot->latency = bench_latency_sampler_t ();
}

inline bool echo_is_blocked_send_errno (int err)
{
    return err == EAGAIN;
}

template <typename State> inline void echo_mark_fatal (State *state, int err)
{
    if (!state)
        return;

    state->fatal.store (true, std::memory_order_release);
    state->fatal_errno.store (err != 0 ? err : EIO, std::memory_order_release);
}

inline double echo_percentile_from_sorted (const std::vector<double> &sorted_samples,
                                           double quantile)
{
    if (sorted_samples.empty ())
        return 0.0;
    if (quantile <= 0.0)
        return sorted_samples.front ();
    if (quantile >= 1.0)
        return sorted_samples.back ();

    const double pos = (static_cast<double> (sorted_samples.size ()) - 1.0) * quantile;
    const size_t lo = static_cast<size_t> (pos);
    const size_t hi = lo + 1 < sorted_samples.size () ? lo + 1 : lo;
    const double frac = pos - static_cast<double> (lo);
    return sorted_samples[lo] + (sorted_samples[hi] - sorted_samples[lo]) * frac;
}

template <typename State>
inline void echo_reset_active_metrics (State *state, uint32_t run_id, size_t msg_size)
{
    if (!state)
        return;

    state->collect_active.store (false, std::memory_order_release);
    state->active_run_id.store (run_id, std::memory_order_release);
    state->active_msg_size.store (msg_size, std::memory_order_release);
    state->active_received.store (0, std::memory_order_release);
}

template <typename Slot>
inline void echo_configure_phase_slot (
  Slot *slot, uint32_t run_id, size_t msg_size, perf_multi_metric::phase_t phase, bool send_enabled)
{
    slot->run_id = run_id;
    slot->msg_size = msg_size;
    slot->phase = phase;
    slot->next_seq = 1;
    slot->send_pending = false;
    slot->inflight = false;
    slot->send_enabled = send_enabled;
    slot->auto_send_on_recv = send_enabled;
    slot->poller_events = 0;
}

template <typename Slot> inline void echo_stop_phase_slot (Slot *slot)
{
    slot->send_enabled = false;
    slot->send_pending = false;
    slot->auto_send_on_recv = false;
}

template <typename Slot>
inline void echo_append_slot_latency (const Slot *slot,
                                      unsigned long long *latency_count_out,
                                      double *latency_sum_ns_out,
                                      std::vector<double> *latency_samples_out)
{
    if (latency_count_out)
        *latency_count_out += slot->latency.count ();
    if (latency_sum_ns_out)
        *latency_sum_ns_out += slot->latency.sum_ns ();
    if (latency_samples_out)
        slot->latency.append_samples (latency_samples_out);
}

inline bool echo_finalize_latency_stats (unsigned long long latency_count,
                                         double latency_sum_ns,
                                         std::vector<double> &latency_samples,
                                         bench_latency_stats_t *latency_out)
{
    if (!latency_out)
        return latency_count > 0;

    if (latency_count == 0) {
        *latency_out = bench_latency_stats_t ();
        return false;
    }

    latency_out->mean_ns = latency_sum_ns / static_cast<double> (latency_count);
    if (latency_samples.empty ()) {
        latency_out->p95_ns = latency_out->mean_ns;
        latency_out->p99_ns = latency_out->mean_ns;
        return true;
    }

    std::sort (latency_samples.begin (), latency_samples.end ());
    latency_out->p95_ns = echo_percentile_from_sorted (latency_samples, 0.95);
    latency_out->p99_ns = echo_percentile_from_sorted (latency_samples, 0.99);
    return true;
}

template <typename State,
          typename ResetFn,
          typename ConfigureFn,
          typename SeedFn,
          typename ServiceFn,
          typename StopFn,
          typename ActiveStartFn,
          typename ActiveStopFn>
inline bool echo_run_two_phase_request_flow (State *state,
                                             int timeout_ms,
                                             double active_seconds,
                                             const ResetFn &reset_fn,
                                             const ConfigureFn &configure_fn,
                                             const SeedFn &seed_fn,
                                             const ServiceFn &service_fn,
                                             const StopFn &stop_fn,
                                             const ActiveStartFn &active_start_fn,
                                             const ActiveStopFn &active_stop_fn)
{
    reset_fn ();
    active_start_fn ();
    configure_fn (perf_multi_metric::phase_active, true);
    if (!echo_start_phase_requests (state, timeout_ms, seed_fn,
                                    [&] (State *, int slice_ms, bool *progressed) {
                                        return service_fn (slice_ms, progressed);
                                    }))
        return false;
    if (!echo_wait_phase_duration (state, active_seconds,
                                   [&] (State *, int slice_ms, bool *progressed) {
                                       return service_fn (slice_ms, progressed);
                                   }))
        return false;
    active_stop_fn ();
    return true;
}

inline bool echo_emit_client_results (const char *pattern,
                                      const std::string &lib_name,
                                      const std::string &transport,
                                      size_t msg_size,
                                      int duration_seconds,
                                      bool fatal,
                                      unsigned long long active_received,
                                      unsigned long long latency_count,
                                      const bench_latency_stats_t &latency)
{
    if (!pattern || duration_seconds <= 0 || fatal || active_received == 0 || latency_count == 0) {
        errno = EINVAL;
        return false;
    }

    const double throughput =
      static_cast<double> (active_received) / static_cast<double> (duration_seconds);
    perf_multi_client::print_client_result_lines (pattern, lib_name, transport, msg_size,
                                                  throughput, latency);
    return true;
}

template <typename State, typename SeedFn, typename ServiceFn>
inline bool echo_start_phase_requests (State *state,
                                       int timeout_ms,
                                       const SeedFn &seed_fn,
                                       const ServiceFn &service_fn)
{
    if (!state)
        return false;

    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (std::max (1, timeout_ms));

    while (std::chrono::steady_clock::now () < deadline) {
        bool all_started = false;
        if (!seed_fn (&all_started)) {
            echo_mark_fatal (state, errno);
            return false;
        }

        if (all_started)
            return true;

        const int remaining_ms = perf_multi_client::remaining_poll_timeout_ms (deadline);
        if (remaining_ms <= 0)
            break;

        bool progressed = false;
        if (!service_fn (state, remaining_ms, &progressed))
            return false;
    }

    return false;
}

template <typename State, typename ServiceFn>
inline bool echo_wait_phase_duration (State *state, double seconds, const ServiceFn &service_fn)
{
    if (!state)
        return false;
    if (seconds <= 0.0)
        return true;

    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                            std::chrono::duration<double> (seconds));

    while (!state->fatal.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline) {
        const int remaining_ms = perf_multi_client::remaining_poll_timeout_ms (deadline);
        if (remaining_ms <= 0)
            break;

        bool progressed = false;
        if (!service_fn (state, remaining_ms, &progressed))
            return false;
    }

    return !state->fatal.load (std::memory_order_acquire);
}

} // namespace perf_multi_echo_policy

#endif
