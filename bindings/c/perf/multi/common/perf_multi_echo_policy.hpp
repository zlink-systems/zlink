#ifndef PERF_MULTI_ECHO_POLICY_HPP
#define PERF_MULTI_ECHO_POLICY_HPP

#include "perf_multi_client_helpers.hpp"

#include <cerrno>
#include <chrono>

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
