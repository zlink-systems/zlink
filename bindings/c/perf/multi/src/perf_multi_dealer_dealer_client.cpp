#include "../common/perf_common.hpp"
#include "../common/perf_common_multi.hpp"
#include "../common/perf_multi_metric_header.hpp"
#include "../common/perf_multi_client_helpers.hpp"
#include "../common/perf_multi_handshake.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{

static const char *k_pattern = "MULTI_DEALER_DEALER";
static const int k_client_socket_type = ZLINK_SOCKET_DEALER;

static std::atomic<bool> g_stop_requested (false);

enum send_status_t
{
    send_status_ok = 0,
    send_status_blocked = 1,
    send_status_fatal = 2
};

struct dd_send_slot_t
{
    dd_send_slot_t () :
        socket (NULL), pending_completions (0), completion_id (0), send_blocked (false)
    {
    }

    void *socket;
    size_t pending_completions;
    zlink_completion_id_t completion_id;
    bool send_blocked;
};

struct dd_send_state_t
{
    dd_send_state_t () : poller (NULL), slots (), events () {}

    void *poller;
    std::vector<dd_send_slot_t> slots;
    std::vector<zlink_poller_event_t> events;
};

using perf_multi_client::close_client_monitors;
using perf_multi_client::close_client_sockets;
using perf_multi_client::is_supported_transport;
using perf_multi_client::parse_endpoint_arg;
using perf_multi_client::refresh_connected_client_auto_hwm;
using perf_multi_client::resolve_case_msg_sizes;
using perf_multi_client::wait_client_connect_ready_all;

inline void on_signal (int)
{
    g_stop_requested.store (true, std::memory_order_release);
}

inline void install_signal_handlers ()
{
    std::signal (SIGINT, on_signal);
#if defined(SIGTERM)
    std::signal (SIGTERM, on_signal);
#endif
}

inline bool create_client_sockets (ctx_guard_t &ctx,
                                   const std::string &transport,
                                   const std::string &endpoint,
                                   const multi_bench_settings_t &settings,
                                   size_t msg_size,
                                   std::vector<void *> *sockets_out,
                                   std::vector<ready_monitor_t> *monitors_out)
{
    return perf_multi_client::create_client_sockets (ctx, transport, endpoint, settings,
                                                     k_client_socket_type, msg_size, sockets_out,
                                                     monitors_out, false);
}

inline short dd_slot_events (const dd_send_slot_t &slot)
{
    short events = static_cast<short> (ZLINK_POLLIN | ZLINK_POLLCOMPLETION);
    if (slot.send_blocked)
        events = static_cast<short> (events | ZLINK_POLLOUT);
    return events;
}

inline bool update_dd_slot_events (dd_send_state_t *state, dd_send_slot_t *slot)
{
    return state && state->poller && slot && slot->socket
           && zlink_poller_modify (state->poller, slot->socket, dd_slot_events (*slot))
                == ZLINK_CONFIG_OK;
}

inline void close_dd_send_state (dd_send_state_t *state)
{
    if (!state)
        return;
    if (state->poller)
        zlink_poller_destroy (&state->poller);
    state->slots.clear ();
    state->events.clear ();
}

inline bool init_dd_send_state (const std::vector<void *> &sockets, dd_send_state_t *state)
{
    if (!state || sockets.empty ())
        return false;

    close_dd_send_state (state);
    state->slots.resize (sockets.size ());
    state->events.resize (sockets.size ());
    state->poller = zlink_poller_new ();
    if (!state->poller)
        return false;

    for (size_t i = 0; i < sockets.size (); ++i) {
        state->slots[i].socket = sockets[i];
        if (!state->slots[i].socket
            || zlink_poller_add (state->poller, state->slots[i].socket, &state->slots[i],
                                 dd_slot_events (state->slots[i]))
                 != ZLINK_CONFIG_OK) {
            close_dd_send_state (state);
            return false;
        }
    }
    return true;
}

inline send_status_t send_one_message (dd_send_slot_t *slot,
                                       size_t payload_size,
                                       uint32_t run_id,
                                       perf_multi_metric::phase_t phase,
                                       size_t msg_size,
                                       uint64_t seq)
{
    if (!slot || !slot->socket || payload_size == 0 || slot->pending_completions != 0)
        return send_status_fatal;

    zlink_msg_t part;
    if (zlink_msg_init_size (&part, payload_size) != 0)
        return send_status_fatal;

    if (!perf_multi_metric::stamp_payload (static_cast<char *> (zlink_msg_data (&part)),
                                           payload_size, run_id, phase, msg_size, seq,
                                           perf_multi_metric::now_ns ())) {
        zlink_msg_close (&part);
        return send_status_fatal;
    }

    zlink_msg_t tail;
    const bool multipart = perf_measurement_part_count () != 1u;
    if (multipart && zlink_msg_init (&tail) != 0) {
        zlink_msg_close (&part);
        return send_status_fatal;
    }

    zlink_completion_id_t completion_id = 0;
    zlink_submit_result_t rc = ZLINK_SUBMIT_INVALID_ARGUMENT;
    if (!multipart) {
        rc = zlink_send_part (slot->socket, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                              ZLINK_PART_FINAL, slot->socket, &completion_id);
    } else {
        rc = zlink_send_part (slot->socket, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                              ZLINK_PART_MORE, NULL, NULL);
        if (rc == ZLINK_SUBMIT_OK) {
            rc = zlink_send_part (slot->socket, &tail, ZLINK_SEND_FLAGS_DONTWAIT,
                                  ZLINK_PART_FINAL, slot->socket, &completion_id);
        }
    }

    const int err = zlink_errno ();
    zlink_msg_close (&part);
    if (multipart)
        zlink_msg_close (&tail);

    if (rc == ZLINK_SUBMIT_OK) {
        if (completion_id != 0) {
            ++slot->pending_completions;
            slot->completion_id = completion_id;
        }
        return send_status_ok;
    }

    if (rc == ZLINK_SUBMIT_BACKPRESSURED || err == EAGAIN || err == EWOULDBLOCK)
        return send_status_blocked;

    return send_status_fatal;
}

inline bool drain_dd_send_completions (dd_send_slot_t *slot)
{
    if (!slot || !slot->socket)
        return false;

    for (;;) {
        zlink_completion_t completion;
        std::memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t rc = zlink_completion_recv (
          slot->socket, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA)
            return true;
        if (rc != ZLINK_RECV_OK)
            return false;

        const bool valid = completion.kind == ZLINK_COMPLETION_SEND
                           && completion.completion_id != 0
                           && completion.user_context == slot->socket
                           && slot->pending_completions == 1
                           && completion.completion_id == slot->completion_id;
        if (valid) {
            --slot->pending_completions;
            slot->completion_id = 0;
        }
        const bool admitted = valid && completion.send_result == ZLINK_SEND_ADMITTED;
        const int terminal_errno = completion.send_terminal_errno;
        zlink_completion_close (&completion);
        if (!admitted) {
            errno = terminal_errno != 0 ? terminal_errno : EPROTO;
            return false;
        }
    }
}

inline bool service_dd_events (dd_send_state_t *state, int timeout_ms, int *event_count_out = NULL)
{
    if (event_count_out)
        *event_count_out = 0;
    if (!state || !state->poller)
        return false;

    const int event_count = zlink_poller_wait (
      state->poller, state->events.empty () ? NULL : &state->events[0],
      static_cast<int> (state->events.size ()), timeout_ms, NULL);
    if (event_count < 0) {
        if (zlink_errno () == EINTR || zlink_errno () == EAGAIN)
            return true;
        return false;
    }
    if (event_count_out)
        *event_count_out = event_count;

    for (int i = 0; i < event_count; ++i) {
        const zlink_poller_event_t &event = state->events[static_cast<size_t> (i)];
        dd_send_slot_t *slot = static_cast<dd_send_slot_t *> (event.user_data);
        if (!slot || slot->socket != event.socket) {
            errno = EPROTO;
            return false;
        }
        if ((event.events & ZLINK_POLLCOMPLETION) != 0
            && !drain_dd_send_completions (slot)) {
            return false;
        }
        if ((event.events & ZLINK_POLLOUT) != 0 && slot->send_blocked) {
            slot->send_blocked = false;
            if (!update_dd_slot_events (state, slot))
                return false;
        }
    }
    return true;
}

inline bool dd_has_pending_completion (const dd_send_state_t &state)
{
    for (size_t i = 0; i < state.slots.size (); ++i) {
        if (state.slots[i].pending_completions != 0)
            return true;
    }
    return false;
}

inline int dd_wait_ms (const std::chrono::steady_clock::time_point &deadline, int maximum_ms)
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ();
    if (now >= deadline)
        return 0;
    const long long remaining =
      std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now).count ();
    return static_cast<int> (std::max<long long> (1, std::min<long long> (maximum_ms, remaining)));
}

inline bool drain_dd_pending_completions (dd_send_state_t *state, int timeout_ms)
{
    if (!state)
        return false;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (std::max (1, timeout_ms));
    while (dd_has_pending_completion (*state) && std::chrono::steady_clock::now () < deadline) {
        if (!service_dd_events (state, dd_wait_ms (deadline, 50)))
            return false;
    }
    if (dd_has_pending_completion (*state)) {
        errno = ETIMEDOUT;
        return false;
    }
    return true;
}

inline bool send_stop_token (void *socket)
{
    return send_stop_token_bounded (socket, [] (void *target) {
        const size_t token_size = std::strlen (k_stop_token);
        zlink_msg_t part;
        if (zlink_msg_init_size (&part, token_size) != 0)
            return perf_stop_submit_fatal;
        std::memcpy (zlink_msg_data (&part), k_stop_token, token_size);

        const zlink_submit_result_t rc =
          ::perf_zlink_send_parts (target, &part, 1, ZLINK_SEND_FLAGS_NONE);
        const int err = rc == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
        zlink_msg_close (&part);
        if (rc == ZLINK_SUBMIT_OK)
            return perf_stop_submit_ok;

        if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT)
            return perf_stop_submit_retry;
        return perf_stop_submit_fatal;
    });
}

inline bool run_active_send_window (dd_send_state_t *state,
                                    size_t payload_size,
                                    uint32_t run_id,
                                    size_t msg_size,
                                    double duration_seconds,
                                    uint64_t *seq)
{
    if (!state || !state->poller || state->slots.empty () || !seq || duration_seconds <= 0.0)
        return false;

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
        std::chrono::duration<double> (duration_seconds));

    if (bench_transition_debug_enabled ()) {
        std::cerr << "[multi-dealer-dealer-client] active send begin msg_size=" << msg_size
                  << std::endl;
    }

    size_t rr = 0;
    while (!g_stop_requested.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline) {
        bool submitted = false;
        const size_t start = rr;
        for (size_t attempts = 0; attempts < state->slots.size (); ++attempts) {
            dd_send_slot_t &slot = state->slots[(start + attempts) % state->slots.size ()];
            // Core's physical queue remains the saturation boundary. Keep only
            // one additional Core-owned pending record per socket.
            if (slot.pending_completions != 0 || slot.send_blocked)
                continue;

            const send_status_t send_rc = send_one_message (
              &slot, payload_size, run_id, perf_multi_metric::phase_active, msg_size, *seq);
            if (send_rc == send_status_ok) {
                ++(*seq);
                submitted = true;
                continue;
            }
            if (send_rc == send_status_blocked) {
                slot.send_blocked = true;
                if (!update_dd_slot_events (state, &slot))
                    return false;
                continue;
            }
            return false;
        }
        rr = (start + 1) % state->slots.size ();

        const int timeout_ms = submitted ? 0 : dd_wait_ms (deadline, 50);
        if (timeout_ms == 0 && !submitted)
            break;
        if (!service_dd_events (state, timeout_ms))
            return false;
    }

    if (g_stop_requested.load (std::memory_order_acquire))
        return false;
    if (!drain_dd_pending_completions (state, 5000))
        return false;

    for (size_t i = 0; i < state->slots.size (); ++i) {
        if (state->slots[i].send_blocked) {
            state->slots[i].send_blocked = false;
            if (!update_dd_slot_events (state, &state->slots[i]))
                return false;
        }
    }

    if (bench_transition_debug_enabled ()) {
        std::cerr << "[multi-dealer-dealer-client] active send end msg_size=" << msg_size
                  << std::endl;
    }
    return true;
}

inline bool wait_for_phase_from_stdin (size_t msg_size, const char *prefix)
{
    if (!prefix || !*prefix) {
        errno = EINVAL;
        return false;
    }
    std::string line;
    while (std::getline (std::cin, line)) {
        if (line == "STOP" || line == "QUIT") {
            errno = ECANCELED;
            return false;
        }
        size_t phase_size = 0;
        if (perf_multi_handshake::parse_size_command_line (
              line, prefix, &phase_size)
            && phase_size == msg_size) {
            return true;
        }
    }
    errno = ECANCELED;
    return false;
}

enum latency_ack_status_t
{
    latency_ack_ok = 0,
    latency_ack_empty = 1,
    latency_ack_fatal = 2
};

inline latency_ack_status_t receive_latency_ack (void *socket,
                                                 size_t msg_size,
                                                 uint32_t run_id,
                                                 uint64_t expected_seq)
{
    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&part) != 0)
        return latency_ack_fatal;
    const int rc = zlink_recv_part (socket, &source_rid, &part, &has_more,
                                    ZLINK_RECV_FLAGS_DONTWAIT);
    if (rc != 0) {
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err == EAGAIN || err == EINTR)
            return latency_ack_empty;
        return latency_ack_fatal;
    }

    perf_multi_metric::header_t header;
    std::memset (&header, 0, sizeof (header));
    const bool valid = !source_rid && has_more == ZLINK_PART_FINAL
                       && perf_multi_metric::decode_payload_header (
                         zlink_msg_data (&part), zlink_msg_size (&part), &header)
                       && header.magic == perf_multi_metric::k_magic
                       && header.run_id == run_id
                       && header.phase
                            == static_cast<uint8_t> (perf_multi_metric::phase_latency)
                       && header.msg_size == msg_size && header.seq == expected_seq;
    zlink_msg_close (&part);
    if (!valid) {
        errno = EPROTO;
        return latency_ack_fatal;
    }
    return latency_ack_ok;
}

inline bool run_latency_send_window (dd_send_state_t *state,
                                     size_t payload_size,
                                     uint32_t run_id,
                                     size_t msg_size,
                                     uint64_t *seq)
{
    if (!state || !state->poller || state->slots.empty () || !seq)
        return false;

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    size_t rr = 0;
    bool in_flight = false;
    uint64_t in_flight_seq = 0;
    unsigned long long ack_count = 0;

    while (!g_stop_requested.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline) {
        bool submitted = false;
        if (!in_flight && !dd_has_pending_completion (*state)) {
            dd_send_slot_t &slot = state->slots[rr];
            if (!slot.send_blocked) {
                const uint64_t candidate_seq = *seq;
                const send_status_t send_rc =
                  send_one_message (&slot, payload_size, run_id,
                                    perf_multi_metric::phase_latency, msg_size, candidate_seq);
                if (send_rc == send_status_ok) {
                    ++(*seq);
                    in_flight = true;
                    in_flight_seq = candidate_seq;
                    submitted = true;
                    rr = (rr + 1) % state->slots.size ();
                } else if (send_rc == send_status_blocked) {
                    slot.send_blocked = true;
                    if (!update_dd_slot_events (state, &slot))
                        return false;
                    rr = (rr + 1) % state->slots.size ();
                } else {
                    return false;
                }
            } else {
                rr = (rr + 1) % state->slots.size ();
            }
        }

        const int timeout_ms = submitted ? 0 : dd_wait_ms (deadline, 50);
        if (timeout_ms == 0 && !submitted)
            break;
        int event_count = 0;
        if (!service_dd_events (state, timeout_ms, &event_count))
            return false;
        for (int i = 0; i < event_count; ++i) {
            const zlink_poller_event_t &event = state->events[static_cast<size_t> (i)];
            if ((event.events & ZLINK_POLLIN) == 0)
                continue;
            for (;;) {
                const latency_ack_status_t ack_rc =
                  receive_latency_ack (event.socket, msg_size, run_id, in_flight_seq);
                if (ack_rc == latency_ack_empty)
                    break;
                if (ack_rc == latency_ack_fatal || !in_flight)
                    return false;
                // Publish the delivery acknowledgement only after this socket's
                // ready burst has reached NO_DATA, matching the single-runner
                // in-flight-1 sampling boundary.
                in_flight = false;
                ++ack_count;
            }
        }
    }

    const std::chrono::steady_clock::time_point drain_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while ((in_flight || dd_has_pending_completion (*state))
           && std::chrono::steady_clock::now () < drain_deadline) {
        int event_count = 0;
        if (!service_dd_events (state, dd_wait_ms (drain_deadline, 50), &event_count))
            return false;
        for (int i = 0; i < event_count; ++i) {
            const zlink_poller_event_t &event = state->events[static_cast<size_t> (i)];
            if ((event.events & ZLINK_POLLIN) == 0)
                continue;
            for (;;) {
                const latency_ack_status_t ack_rc =
                  receive_latency_ack (event.socket, msg_size, run_id, in_flight_seq);
                if (ack_rc == latency_ack_empty)
                    break;
                if (ack_rc == latency_ack_fatal || !in_flight)
                    return false;
                in_flight = false;
                ++ack_count;
            }
        }
    }

    if (g_stop_requested.load (std::memory_order_acquire) || in_flight
        || dd_has_pending_completion (*state) || ack_count == 0) {
        errno = ETIMEDOUT;
        return false;
    }
    return true;
}

inline bool send_phase_stop_tokens (const std::vector<void *> &sockets)
{
    for (size_t i = 0; i < sockets.size (); ++i) {
        if (!send_stop_token (sockets[i]))
            return false;
    }
    return true;
}

inline bool run_single_size_case (const std::vector<void *> &sockets,
                                  const multi_bench_settings_t &settings,
                                  size_t msg_size,
                                  uint32_t run_id)
{
    const size_t payload_size = std::max<size_t> (msg_size, perf_multi_metric::header_size ());
    const double active_s = static_cast<double> (std::max (1, settings.duration_seconds));

    dd_send_state_t state;
    if (!init_dd_send_state (sockets, &state))
        return false;

    uint64_t seq = 1;
    bool ok = run_active_send_window (&state, payload_size, run_id, msg_size, active_s, &seq)
              && send_phase_stop_tokens (sockets)
              && wait_for_phase_from_stdin (msg_size, "PHASE_LATENCY,")
              && run_latency_send_window (&state, payload_size, run_id, msg_size, &seq)
              && send_phase_stop_tokens (sockets)
              && wait_for_phase_from_stdin (msg_size, "PHASE_DONE,");

    close_dd_send_state (&state);
    return ok;
}

inline int run_client_benchmark (const std::string &lib_name,
                                 const std::string &transport,
                                 const std::string &endpoint,
                                 size_t fallback_size)
{
    set_perf_multi_pattern_env (k_pattern);

    if (!is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << "," << k_pattern << "," << transport
                  << std::endl;
        return 0;
    }

    if (!transport_available (transport)) {
        std::cerr << "transport unavailable: " << transport << std::endl;
        return 1;
    }

    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    const std::vector<size_t> msg_sizes = resolve_case_msg_sizes (fallback_size);
    size_t max_msg_size = fallback_size > 0 ? fallback_size : 64;
    for (size_t i = 0; i < msg_sizes.size (); ++i) {
        if (msg_sizes[i] > max_msg_size)
            max_msg_size = msg_sizes[i];
    }

    ctx_guard_t ctx;
    if (!ctx.valid ())
        return 1;

    std::vector<void *> sockets;
    std::vector<ready_monitor_t> monitors;
    if (!create_client_sockets (ctx, transport, endpoint, settings, max_msg_size, &sockets,
                                &monitors)) {
        if (bench_debug_enabled ())
            std::cerr << "[multi-dealer-dealer-client] create sockets failed" << std::endl;
        close_client_monitors (&monitors);
        close_client_sockets (&sockets);
        return 1;
    }

    if (!wait_client_connect_ready_all (monitors, settings.connect_ready_timeout_ms)) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-client] connect ready timeout" << std::endl;
        }
        close_client_monitors (&monitors);
        close_client_sockets (&sockets);
        return 1;
    }
    close_client_monitors (&monitors);

    g_stop_requested.store (false, std::memory_order_release);
    install_signal_handlers ();

    for (size_t si = 0; si < msg_sizes.size (); ++si) {
        if (g_stop_requested.load (std::memory_order_acquire)) {
            close_client_monitors (&monitors);
            close_client_sockets (&sockets);
            return 1;
        }

        refresh_connected_client_auto_hwm (sockets,
                                           static_cast<zlink_socket_type_t> (k_client_socket_type),
                                           settings.hwm, transport, msg_sizes[si]);

        const uint32_t run_id = static_cast<uint32_t> (si + 1);
        std::cout << "CLIENT_READY," << msg_sizes[si] << std::endl;
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-client] ready size=" << msg_sizes[si] << std::endl;
        }
        if (!perf_multi_handshake::wait_for_start_from_stdin (msg_sizes[si])) {
            if (bench_transition_debug_enabled ()) {
                std::cerr << "[multi-dealer-dealer-client] start gate failed size=" << msg_sizes[si]
                          << std::endl;
            }
            close_client_monitors (&monitors);
            close_client_sockets (&sockets);
            return 1;
        }
        if (!run_single_size_case (sockets, settings, msg_sizes[si], run_id)) {
            if (bench_transition_debug_enabled ()) {
                std::cerr << "[multi-dealer-dealer-client] size case failed size=" << msg_sizes[si]
                          << std::endl;
            }
            close_client_monitors (&monitors);
            close_client_sockets (&sockets);
            return 1;
        }
        if (bench_transition_debug_enabled ()) {
            std::cerr << "[multi-dealer-dealer-client] done size=" << msg_sizes[si] << std::endl;
        }
        std::cout << "CLIENT_DONE," << msg_sizes[si] << std::endl;
    }

    close_client_monitors (&monitors);
    close_client_sockets (&sockets);
    return 0;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc < 4)
        return 1;
    if (!multi_perf_validate_recv_mode_for_pattern (k_pattern))
        return 1;

    const std::string lib_name = argv[1];
    const std::string transport = argv[2];
    const size_t fallback_size = static_cast<size_t> (std::strtoull (argv[3], NULL, 10));

    std::string endpoint;
    if (!parse_endpoint_arg (argc, argv, &endpoint)) {
        std::cerr << "missing --endpoint" << std::endl;
        return 1;
    }

    return run_client_benchmark (lib_name, transport, endpoint, fallback_size);
}
