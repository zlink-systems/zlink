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
        socket (NULL), wait_token (0), retained (false), retry_ready (false),
        pollout_suppressed (false), retained_payload ()
    {
    }

    void *socket;
    zlink_completion_id_t wait_token;
    bool retained;
    bool retry_ready;
    // DEALER POLLOUT is an aggregate hint. After one NO_DATA pull it is
    // dropped from the interest set so a writable peer cannot spin the loop;
    // the token's WRITABLE record still wakes POLLCOMPLETION.
    bool pollout_suppressed;
    std::vector<char> retained_payload;
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
    if (slot.wait_token != 0 && !slot.pollout_suppressed)
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

inline void clear_retained_message (dd_send_slot_t *slot)
{
    if (!slot)
        return;
    slot->wait_token = 0;
    slot->retained = false;
    slot->retry_ready = false;
    slot->pollout_suppressed = false;
    slot->retained_payload.clear ();
}

inline send_status_t submit_retained_message (dd_send_slot_t *slot)
{
    if (!slot || !slot->socket || !slot->retained || slot->wait_token != 0
        || slot->retained_payload.empty ()) {
        errno = EBUSY;
        return send_status_fatal;
    }

    zlink_msg_t part;
    if (zlink_msg_init_size (&part, slot->retained_payload.size ()) != 0)
        return send_status_fatal;
    std::memcpy (zlink_msg_data (&part), slot->retained_payload.data (),
                 slot->retained_payload.size ());

    zlink_msg_t tail;
    const bool multipart = perf_measurement_part_count () != 1u;
    if (multipart && zlink_msg_init (&tail) != 0) {
        zlink_msg_close (&part);
        return send_status_fatal;
    }

    zlink_completion_id_t wait_token = 0;
    zlink_submit_result_t rc = ZLINK_SUBMIT_INVALID_ARGUMENT;
    if (!multipart) {
        rc = zlink_send_part (slot->socket, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                              ZLINK_PART_FINAL, slot->socket, &wait_token);
    } else {
        rc = zlink_send_part (slot->socket, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                              ZLINK_PART_MORE, NULL, NULL);
        if (rc == ZLINK_SUBMIT_OK) {
            rc = zlink_send_part (slot->socket, &tail, ZLINK_SEND_FLAGS_DONTWAIT,
                                  ZLINK_PART_FINAL, slot->socket, &wait_token);
        }
    }

    const int err = rc == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    zlink_msg_close (&part);
    if (multipart)
        zlink_msg_close (&tail);

    if (rc == ZLINK_SUBMIT_OK && wait_token == 0) {
        clear_retained_message (slot);
        return send_status_ok;
    }

    if (rc == ZLINK_SUBMIT_BACKPRESSURED
        && (err == EAGAIN || err == EWOULDBLOCK) && wait_token != 0) {
        slot->wait_token = wait_token;
        slot->retry_ready = false;
        slot->pollout_suppressed = false;
        errno = err;
        return send_status_blocked;
    }

    errno = rc == ZLINK_SUBMIT_OK || rc == ZLINK_SUBMIT_BACKPRESSURED
              ? EPROTO
              : (err != 0 ? err : EIO);
    return send_status_fatal;
}

inline send_status_t send_one_message (dd_send_slot_t *slot,
                                       size_t payload_size,
                                       uint32_t run_id,
                                       perf_multi_metric::phase_t phase,
                                       size_t msg_size,
                                       uint64_t seq)
{
    if (!slot || !slot->socket || payload_size == 0 || slot->retained) {
        errno = EBUSY;
        return send_status_fatal;
    }

    slot->retained_payload.resize (payload_size);
    if (!perf_multi_metric::stamp_payload (
          slot->retained_payload.data (), payload_size, run_id, phase, msg_size,
          seq, perf_multi_metric::now_ns ())) {
        slot->retained_payload.clear ();
        return send_status_fatal;
    }
    slot->retained = true;
    return submit_retained_message (slot);
}

inline bool record_dd_writable (dd_send_slot_t *slot,
                                const zlink_completion_t &completion)
{
    if (!slot || completion.kind != ZLINK_COMPLETION_WRITABLE
        || completion.completion_id == 0
        || completion.user_context != slot->socket
        || completion.peer_rid.size != 0 || !slot->retained
        || slot->wait_token == 0
        || completion.completion_id != slot->wait_token) {
        errno = EPROTO;
        return false;
    }

    slot->wait_token = 0;
    slot->pollout_suppressed = false;
    if (completion.send_result == ZLINK_SEND_ADMITTED
        && completion.send_terminal_errno == 0) {
        slot->retry_ready = true;
        return true;
    }

    const int terminal_errno = completion.send_terminal_errno;
    clear_retained_message (slot);
    errno = terminal_errno != 0 ? terminal_errno : EIO;
    return false;
}

inline bool drain_dd_writable (dd_send_state_t *state, dd_send_slot_t *slot)
{
    if (!state || !slot || !slot->socket)
        return false;

    for (;;) {
        zlink_completion_t completion;
        std::memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t rc = zlink_completion_recv (
          slot->socket, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            if (slot->wait_token != 0)
                slot->pollout_suppressed = true;
            break;
        }
        if (rc != ZLINK_RECV_OK)
            return false;

        const bool valid = record_dd_writable (slot, completion);
        const int completion_errno = valid ? 0 : errno;
        zlink_completion_close (&completion);
        if (!valid) {
            errno = completion_errno;
            return false;
        }
    }

    send_status_t retry_status = send_status_ok;
    if (slot->retry_ready)
        retry_status = submit_retained_message (slot);
    if (retry_status == send_status_fatal)
        return false;
    if (!update_dd_slot_events (state, slot))
        return false;
    if (retry_status == send_status_blocked)
        errno = EAGAIN;
    return true;
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
        if ((event.events & (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION)) != 0
            && !drain_dd_writable (state, slot)) {
            return false;
        }
    }
    return true;
}

inline bool dd_has_retained_send (const dd_send_state_t &state)
{
    for (size_t i = 0; i < state.slots.size (); ++i) {
        if (state.slots[i].retained)
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

inline bool drain_dd_retained_sends (dd_send_state_t *state, int timeout_ms)
{
    if (!state)
        return false;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (std::max (1, timeout_ms));
    while (dd_has_retained_send (*state) && std::chrono::steady_clock::now () < deadline) {
        if (!service_dd_events (state, dd_wait_ms (deadline, 50)))
            return false;
    }
    if (dd_has_retained_send (*state)) {
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
            if (slot.retained)
                continue;
            while (!g_stop_requested.load (std::memory_order_acquire)
                   && std::chrono::steady_clock::now () < deadline
                   && !slot.retained) {
                const send_status_t send_rc = send_one_message (
                  &slot, payload_size, run_id, perf_multi_metric::phase_active,
                  msg_size, *seq);
                if (send_rc == send_status_ok) {
                    ++(*seq);
                    submitted = true;
                    continue;
                }
                if (send_rc == send_status_blocked) {
                    // Keep the rejected bytes and sequence until the exact
                    // WRITABLE token permits their resubmission.
                    ++(*seq);
                    if (!update_dd_slot_events (state, &slot))
                        return false;
                    errno = EAGAIN;
                    break;
                }
                return false;
            }
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
    if (!drain_dd_retained_sends (state, 5000))
        return false;

    if (bench_transition_debug_enabled ()) {
        std::cerr << "[multi-dealer-dealer-client] active send end msg_size=" << msg_size
                  << std::endl;
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
    const bool ok =
      run_active_send_window (&state, payload_size, run_id, msg_size, active_s, &seq)
      && send_phase_stop_tokens (sockets);

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
