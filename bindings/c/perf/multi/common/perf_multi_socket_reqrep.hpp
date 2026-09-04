#ifndef PERF_MULTI_SOCKET_REQREP_HPP
#define PERF_MULTI_SOCKET_REQREP_HPP

#include "perf_common.hpp"
#include "perf_common_multi.hpp"
#include "perf_multi_client_helpers.hpp"
#include "perf_multi_metric_header.hpp"

#include "../../common/perf_tls_setup.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace perf_multi_socket_reqrep
{

using ::setup_tls_server;

struct endpoint_config_t
{
    endpoint_config_t () :
        pattern_name (NULL),
        token (NULL),
        client_socket_type (ZLINK_SOCKET_DEALER),
        server_socket_type (ZLINK_SOCKET_ROUTER),
        client_router_request (false),
        server_has_routing_id (false),
        server_routing_id ("SERVER")
    {
    }

    const char *pattern_name;
    const char *token;
    zlink_socket_type_t client_socket_type;
    zlink_socket_type_t server_socket_type;
    bool client_router_request;
    bool server_has_routing_id;
    const char *server_routing_id;
};

struct client_state_t;

struct client_slot_t
{
    client_slot_t () :
        owner (NULL),
        socket (NULL),
        index (0),
        next_seq (1),
        outstanding (0),
        wait_token (0),
        retained_request (false),
        retry_ready (false),
        routed_request (false),
        target_rid (),
        payload ()
    {
    }

    client_state_t *owner;
    void *socket;
    size_t index;
    uint64_t next_seq;
    size_t outstanding;
    zlink_completion_id_t wait_token;
    bool retained_request;
    bool retry_ready;
    bool routed_request;
    zlink_routing_id_t target_rid;
    std::vector<char> payload;
};

struct client_state_t
{
    explicit client_state_t (
      size_t maximum_latency_sample_cap = std::numeric_limits<size_t>::max ()) :
        poller (NULL),
        active_run_id (0),
        active_msg_size (0),
        active_deadline_ns (0),
        active_reply_count (0),
        capture_latency (false),
        fatal (false),
        slots (),
        events (),
        latency (maximum_latency_sample_cap)
    {
    }

    void *poller;
    uint32_t active_run_id;
    size_t active_msg_size;
    uint64_t active_deadline_ns;
    unsigned long long active_reply_count;
    bool capture_latency;
    bool fatal;
    // Every socket is registered on one POLLCOMPLETION poller. The benchmark
    // thread therefore owns submission, reply callbacks, and metrics.
    std::vector<client_slot_t> slots;
    std::vector<zlink_poller_event_t> events;
    bench_latency_sampler_t latency;
};

inline bool init_routing_id_text (const char *text, zlink_routing_id_t *rid_out)
{
    if (!text || !*text || !rid_out)
        return false;
    const size_t len = std::strlen (text);
    if (len == 0 || len > sizeof (rid_out->data))
        return false;
    std::memset (rid_out, 0, sizeof (*rid_out));
    std::memcpy (rid_out->data, text, len);
    rid_out->size = static_cast<uint8_t> (len);
    return true;
}

inline int poll_timeout_until (const std::chrono::steady_clock::time_point &deadline,
                               int max_wait_ms)
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ();
    if (now >= deadline)
        return 0;
    const long remaining_ms =
      std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now).count ();
    if (remaining_ms <= 0)
        return 1;
    return static_cast<int> (std::min<long> (remaining_ms, std::max (1, max_wait_ms)));
}

#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
inline bool is_transient_submit (zlink_submit_result_t rc, int err)
{
    return rc == ZLINK_SUBMIT_BACKPRESSURED || err == EAGAIN || err == EWOULDBLOCK
           || err == EINTR || err == ETIMEDOUT;
}
#endif

inline bool request_rid_matches (const client_slot_t &slot,
                                 const zlink_routing_id_t &peer_rid)
{
    return slot.routed_request
             ? perf_multi_client::routing_ids_equal (slot.target_rid, peer_rid)
             : peer_rid.size == 0;
}

inline void clear_retained_request (client_slot_t *slot)
{
    if (!slot)
        return;
    slot->wait_token = 0;
    slot->retained_request = false;
    slot->retry_ready = false;
    slot->routed_request = false;
    std::memset (&slot->target_rid, 0, sizeof (slot->target_rid));
}

inline void record_request_completion (client_slot_t *slot,
                                       zlink_request_result_t result,
                                       zlink_msg_t *parts,
                                       size_t part_count)
{
    if (!slot || !slot->owner)
        return;

    client_state_t *state = slot->owner;
    if (slot->outstanding > 0)
        --slot->outstanding;

    if (result != ZLINK_REQUEST_OK || !parts
        || part_count != perf_measurement_part_count ())
        return;

    if (part_count == 2 && zlink_msg_size (&parts[1]) != 0)
        return;

    perf_multi_metric::header_t header;
    if (!perf_multi_metric::decode_payload_header (zlink_msg_data (&parts[0]),
                                                   zlink_msg_size (&parts[0]), &header)) {
        return;
    }

    if (!perf_multi_metric::is_expected (header, state->active_run_id,
                                         perf_multi_metric::phase_active,
                                         state->active_msg_size)) {
        return;
    }

    const uint64_t now_ns = perf_multi_metric::now_ns ();
    if (now_ns >= state->active_deadline_ns || header.sent_ts_ns <= 0
        || now_ns < static_cast<uint64_t> (header.sent_ts_ns)) {
        return;
    }

    const double sample_ns =
      static_cast<double> (now_ns - static_cast<uint64_t> (header.sent_ts_ns)) * 0.5;
    ++state->active_reply_count;
    if (state->capture_latency)
        state->latency.add (sample_ns);
}

#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
inline void on_request_reply (zlink_request_result_t result,
                              zlink_msg_t *parts,
                              size_t part_count,
                              void *userdata)
{
    record_request_completion (static_cast<client_slot_t *> (userdata), result, parts,
                               part_count);
}
#endif

inline bool submit_request (const endpoint_config_t &config,
                            client_slot_t *slot,
                            const zlink_routing_id_t *target_rid,
                            uint32_t run_id,
                            size_t msg_size,
                            uint32_t timeout_ms,
                            bool *blocked_out)
{
    if (blocked_out)
        *blocked_out = false;
    if (!slot || !slot->socket) {
        errno = EINVAL;
        return false;
    }

    const bool retrying = slot->retained_request;
    if (retrying && (!slot->retry_ready || slot->wait_token != 0)) {
        if (blocked_out)
            *blocked_out = true;
        return true;
    }

    const size_t payload_size = std::max<size_t> (msg_size, perf_multi_metric::header_size ());
    if (!retrying) {
        if (slot->payload.size () != payload_size)
            slot->payload.assign (payload_size, 'c');

        if (!perf_multi_metric::stamp_payload (slot->payload.data (), payload_size, run_id,
                                               perf_multi_metric::phase_active, msg_size,
                                               slot->next_seq,
                                               perf_multi_metric::now_ns ())) {
            return false;
        }
    } else if (slot->payload.size () != payload_size) {
        errno = EPROTO;
        return false;
    }

    zlink_msg_t part;
    if (zlink_msg_init_size (&part, payload_size) != 0)
        return false;
    if (payload_size > 0)
        std::memcpy (zlink_msg_data (&part), slot->payload.data (), payload_size);

    zlink_submit_result_t rc = ZLINK_SUBMIT_INVALID_ARGUMENT;
#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
    ++slot->outstanding;
    if (config.client_router_request) {
        if (!target_rid || target_rid->size == 0) {
            zlink_msg_close (&part);
            --slot->outstanding;
            errno = EINVAL;
            return false;
        }
        rc = perf_zlink_router_request_measurement_part (
          slot->socket, target_rid, &part, ZLINK_SEND_FLAGS_DONTWAIT, timeout_ms,
          on_request_reply, slot);
    } else {
        rc = perf_zlink_dealer_request_measurement_part (
          slot->socket, &part, ZLINK_SEND_FLAGS_DONTWAIT, timeout_ms,
          on_request_reply, slot);
    }

    if (rc == ZLINK_SUBMIT_OK) {
        ++slot->next_seq;
        return true;
    }

    const int err = zlink_errno ();
    zlink_msg_close (&part);
    --slot->outstanding;
#else
    zlink_completion_id_t completion_id = 0;
    slot->retry_ready = false;
    if (config.client_router_request) {
        if (!target_rid || target_rid->size == 0) {
            zlink_msg_close (&part);
            errno = EINVAL;
            return false;
        }
        rc = perf_zlink_router_request_measurement_part (
          slot->socket, target_rid, &part, ZLINK_SEND_FLAGS_DONTWAIT, timeout_ms,
          slot, &completion_id);
    } else {
        rc = perf_zlink_dealer_request_measurement_part (
          slot->socket, &part, ZLINK_SEND_FLAGS_DONTWAIT, timeout_ms,
          slot, &completion_id);
    }

    const int err = rc == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    zlink_msg_close (&part);
    if (rc == ZLINK_SUBMIT_OK && completion_id != 0) {
        ++slot->outstanding;
        ++slot->next_seq;
        clear_retained_request (slot);
        return true;
    }

    if (rc == ZLINK_SUBMIT_OK) {
        errno = EPROTO;
        return false;
    }
    if (rc == ZLINK_SUBMIT_BACKPRESSURED
        && (err == EAGAIN || err == EWOULDBLOCK) && completion_id != 0) {
        slot->retained_request = true;
        slot->retry_ready = false;
        slot->wait_token = completion_id;
        slot->routed_request = config.client_router_request;
        if (config.client_router_request)
            slot->target_rid = *target_rid;
        else
            std::memset (&slot->target_rid, 0, sizeof (slot->target_rid));
        if (blocked_out)
            *blocked_out = true;
        errno = err;
        return true;
    }
    if (rc == ZLINK_SUBMIT_BACKPRESSURED)
        errno = EPROTO;
    else
        errno = err != 0 ? err : EIO;
    return false;
#endif
#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
    if (is_transient_submit (rc, err)) {
        if (blocked_out)
            *blocked_out = true;
        return true;
    }
    return false;
#endif
}

#if !defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
inline bool drain_socket_completions (client_state_t *state,
                                      void *socket,
                                      client_slot_t *socket_slot)
{
    if (!state || !socket || !socket_slot || socket_slot->owner != state
        || socket_slot->socket != socket) {
        errno = EPROTO;
        return false;
    }
    for (;;) {
        zlink_completion_t completion;
        std::memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t rc = zlink_completion_recv (
          socket, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA)
            return true;
        if (rc != ZLINK_RECV_OK)
            return false;
        if (completion.kind == ZLINK_COMPLETION_WRITABLE) {
            const bool owned = socket_slot->retained_request
                               && socket_slot->wait_token != 0
                               && completion.completion_id
                                    == socket_slot->wait_token;
            if (!owned) {
                // Setup SENDs can leave an anonymous WRITABLE on this shared
                // queue. REQUEST owns and dispatches only its matching token.
                zlink_completion_close (&completion);
                continue;
            }
            const bool valid = completion.user_context == socket_slot
                               && request_rid_matches (*socket_slot,
                                                       completion.peer_rid)
                               && completion.send_result == ZLINK_SEND_ADMITTED
                               && completion.send_terminal_errno == 0;
            if (valid) {
                socket_slot->wait_token = 0;
                socket_slot->retry_ready = true;
            } else {
                const int terminal_errno = completion.send_terminal_errno;
                clear_retained_request (socket_slot);
                errno = terminal_errno != 0 ? terminal_errno : EPROTO;
            }
            zlink_completion_close (&completion);
            if (!valid)
                return false;
            continue;
        }
        const bool valid = completion.kind == ZLINK_COMPLETION_REQUEST
                           && completion.completion_id != 0
                           && completion.user_context == socket_slot
                           && socket_slot->outstanding != 0;
        if (valid) {
            record_request_completion (socket_slot, completion.request_result,
                                       completion.reply_parts,
                                       completion.reply_part_count);
        }
        zlink_completion_close (&completion);
        if (!valid)
            return false;
    }
}
#endif

inline bool drain_ready_completions (client_state_t *state, int event_count)
{
    if (!state || event_count < 0)
        return false;
#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
    (void) event_count;
    return true;
#else
    for (int i = 0; i < event_count; ++i) {
        const zlink_poller_event_t &event = state->events[static_cast<size_t> (i)];
        client_slot_t *const slot = static_cast<client_slot_t *> (event.user_data);
        if ((event.events & ZLINK_POLLCOMPLETION) != 0
            && !drain_socket_completions (state, event.socket, slot))
            return false;
    }
    return true;
#endif
}

inline bool any_waiting_reply (const client_state_t &state)
{
    for (size_t i = 0; i < state.slots.size (); ++i) {
        if (state.slots[i].outstanding != 0)
            return true;
    }
    return false;
}

inline bool any_retained_request (const client_state_t &state)
{
    for (size_t i = 0; i < state.slots.size (); ++i) {
        if (state.slots[i].retained_request)
            return true;
    }
    return false;
}

inline bool retry_ready_requests (const endpoint_config_t &config,
                                  client_state_t *state,
                                  const zlink_routing_id_t *target_rid,
                                  uint32_t run_id,
                                  size_t msg_size,
                                  uint32_t request_timeout_ms)
{
    for (size_t i = 0; i < state->slots.size (); ++i) {
        client_slot_t &slot = state->slots[i];
        if (!slot.retained_request || !slot.retry_ready)
            continue;
        bool blocked = false;
        if (!submit_request (config, &slot, target_rid, run_id, msg_size,
                             request_timeout_ms, &blocked))
            return false;
    }
    return true;
}

inline bool drain_pending_replies (const endpoint_config_t &config,
                                   client_state_t *state,
                                   const zlink_routing_id_t *target_rid,
                                   uint32_t run_id,
                                   size_t msg_size,
                                   uint32_t request_timeout_ms)
{
    if (!state || !state->poller) {
        errno = EINVAL;
        return false;
    }

    const int drain_ms = std::max<int> (
      perf_multi_client::send_retry_drain_timeout_ms (),
      static_cast<int> (request_timeout_ms) * 4);
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (drain_ms);
    while ((any_waiting_reply (*state) || any_retained_request (*state))
           && std::chrono::steady_clock::now () < deadline) {
        const int wait_ms = poll_timeout_until (deadline, 50);
        const int event_count =
          zlink_poller_wait (state->poller, state->events.empty () ? NULL : &state->events[0],
                             static_cast<int> (state->events.size ()), wait_ms, NULL);
        if (event_count < 0) {
            if (zlink_errno () == EINTR)
                continue;
            return false;
        }
        if (!drain_ready_completions (state, event_count))
            return false;
        if (!retry_ready_requests (config, state, target_rid, run_id, msg_size,
                                   request_timeout_ms))
            return false;
    }
    return !any_waiting_reply (*state) && !any_retained_request (*state);
}

inline size_t request_submit_progress_quantum (size_t msg_size,
                                                size_t client_count,
                                                bool websocket_transport)
{
    // Bound submission work between completion-poller turns by bytes. This
    // controls only when completions are progressed: it neither caps
    // outstanding requests nor waits for a reply before submitting more.
    if (client_count == 0)
        return 0;
    if (!websocket_transport)
        return client_count;
    const size_t progress_quantum_bytes = 32 * 1024;
    const size_t payload_size =
      std::max<size_t> (msg_size, perf_multi_metric::header_size ());
    return std::min (client_count,
                     std::max<size_t> (1, progress_quantum_bytes / payload_size));
}

inline bool run_measurement_window (const endpoint_config_t &config,
                                    client_state_t *state,
                                    const multi_bench_settings_t &settings,
                                    bool websocket_transport,
                                    uint32_t run_id,
                                    size_t msg_size,
                                    int duration_seconds,
                                    unsigned long long *reply_count_out,
                                    bench_latency_stats_t *latency_out)
{
    if (!state || !state->poller || state->slots.empty () || !reply_count_out
        || !latency_out) {
        errno = EINVAL;
        return false;
    }

    zlink_routing_id_t target_rid;
    const zlink_routing_id_t *target_rid_ptr = NULL;
    if (config.client_router_request) {
        if (!init_routing_id_text (config.server_routing_id, &target_rid))
            return false;
        target_rid_ptr = &target_rid;
    }

    state->active_run_id = run_id;
    state->active_msg_size = msg_size;
    state->active_deadline_ns =
      perf_multi_metric::now_ns ()
      + static_cast<uint64_t> (std::max (1, duration_seconds)) * 1000000000ULL;
    state->active_reply_count = 0;
    state->capture_latency = true;
    state->latency.reset ();
    for (size_t i = 0; i < state->slots.size (); ++i)
        state->slots[i].outstanding = 0;
    state->fatal = false;
    for (size_t i = 0; i < state->slots.size (); ++i)
        state->slots[i].next_seq = 1;

    size_t submit_cursor = 0;
    const size_t submit_count =
      request_submit_progress_quantum (msg_size, state->slots.size (),
                                        websocket_transport);

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::seconds (std::max (1, duration_seconds));
    const uint32_t request_timeout_ms =
      static_cast<uint32_t> (bench_timeout_ms_from_env ("PERF_MULTI_REQREP_TIMEOUT_MS", 200));

    while (std::chrono::steady_clock::now () < deadline && !state->fatal) {
        bool submitted = false;
        for (size_t offset = 0; offset < submit_count; ++offset) {
            const size_t i = (submit_cursor + offset) % state->slots.size ();
            client_slot_t &slot = state->slots[i];
            bool blocked = false;
            if (!submit_request (config, &slot, target_rid_ptr, run_id, msg_size,
                                 request_timeout_ms, &blocked)) {
                state->fatal = true;
                break;
            }
            if (!blocked)
                submitted = true;
        }
        submit_cursor = (submit_cursor + submit_count) % state->slots.size ();
        if (state->fatal)
            break;
        const int wait_ms = submitted ? 0 : poll_timeout_until (deadline, 50);
        const int event_count =
          zlink_poller_wait (state->poller, state->events.empty () ? NULL : &state->events[0],
                             static_cast<int> (state->events.size ()), wait_ms, NULL);
        if (event_count < 0) {
            if (zlink_errno () == EINTR)
                continue;
            return false;
        }
        if (!drain_ready_completions (state, event_count))
            return false;
    }

    if (state->fatal) {
        std::cerr << "[perf-multi-socket-reqrep] fatal during window size=" << msg_size
                  << " errno=" << errno << std::endl;
        return false;
    }
    if (!drain_pending_replies (config, state, target_rid_ptr, run_id, msg_size,
                                request_timeout_ms)) {
        size_t waiting_slots = 0;
        unsigned long long waiting_requests = 0;
        for (size_t i = 0; i < state->slots.size (); ++i) {
            if (state->slots[i].outstanding > 0) {
                ++waiting_slots;
                waiting_requests += state->slots[i].outstanding;
            }
        }
        std::cerr << "[perf-multi-socket-reqrep] drain timeout size=" << msg_size
                  << " waiting_slots=" << waiting_slots << "/" << state->slots.size ()
                  << " waiting_requests=" << waiting_requests
                  << " retained_requests=" << any_retained_request (*state)
                  << " request_timeout_ms=" << request_timeout_ms
                  << " replies=" << state->active_reply_count << std::endl;
        return false;
    }

    *reply_count_out = state->active_reply_count;
    *latency_out = state->latency.snapshot ();
    if (state->latency.count () == 0) {
        std::cerr << "[perf-multi-socket-reqrep] no latency samples size=" << msg_size
                  << " replies=" << state->active_reply_count << std::endl;
        return false;
    }
    return true;
}

inline bool run_active_window (const endpoint_config_t &config,
                               client_state_t *state,
                               const multi_bench_settings_t &settings,
                               bool websocket_transport,
                               uint32_t run_id,
                               size_t msg_size,
                               unsigned long long *reply_count_out,
                               bench_latency_stats_t *latency_out)
{
    return run_measurement_window (
      config, state, settings, websocket_transport, run_id, msg_size,
      settings.duration_seconds,
      reply_count_out, latency_out);
}

inline bool setup_client_state (const endpoint_config_t &config,
                                ctx_guard_t &ctx,
                                const std::string &transport,
                                const std::string &endpoint,
                                const multi_bench_settings_t &settings,
                                size_t max_msg_size,
                                client_state_t *state)
{
    if (!state)
        return false;

    std::vector<void *> sockets;
    std::vector<ready_monitor_t> monitors;
    if (!perf_multi_client::create_client_sockets (
          ctx, transport, endpoint, settings, config.client_socket_type, max_msg_size, &sockets,
          &monitors, false)) {
        perf_multi_client::close_client_monitors (&monitors);
        perf_multi_client::close_client_sockets (&sockets);
        return false;
    }

    if (!perf_multi_client::wait_client_connect_ready_all (
          monitors, settings.connect_ready_timeout_ms)) {
        perf_multi_client::close_client_monitors (&monitors);
        perf_multi_client::close_client_sockets (&sockets);
        return false;
    }
    perf_multi_client::close_client_monitors (&monitors);
    perf_multi_client::refresh_connected_client_auto_hwm (
      sockets, config.client_socket_type, settings.hwm, transport, max_msg_size);

    state->slots.resize (sockets.size ());
    for (size_t i = 0; i < sockets.size (); ++i) {
        state->slots[i].owner = state;
        state->slots[i].socket = sockets[i];
        state->slots[i].index = i;
    }
    sockets.clear ();

    state->poller = zlink_poller_new ();
    if (!state->poller)
        return false;
    for (size_t i = 0; i < state->slots.size (); ++i) {
        if (zlink_poller_add (state->poller, state->slots[i].socket, &state->slots[i],
                              ZLINK_POLLCOMPLETION)
            != 0) {
            return false;
        }
    }
    state->events.resize (state->slots.size ());
    return true;
}

inline void close_client_state (client_state_t *state)
{
    if (!state)
        return;
    if (state->poller)
        zlink_poller_destroy (&state->poller);
    for (size_t i = 0; i < state->slots.size (); ++i) {
        if (state->slots[i].socket) {
            zlink_close (state->slots[i].socket);
            state->slots[i].socket = NULL;
        }
    }
}

inline bool wait_for_runner_stop_after_done ()
{
    std::string line;
    while (std::getline (std::cin, line)) {
        if (!line.empty () && line[line.size () - 1] == '\r')
            line.erase (line.size () - 1);
        if (line == "STOP" || line == "QUIT")
            return true;
    }
    return false;
}

inline int run_client_benchmark (const endpoint_config_t &config,
                                 const std::string &lib_name,
                                 const std::string &transport,
                                 const std::string &endpoint,
                                 size_t fallback_size)
{
    set_perf_multi_pattern_env (config.pattern_name);

    if (!perf_multi_client::is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << "," << config.pattern_name << "," << transport
                  << std::endl;
        return 0;
    }
    if (!transport_available (transport)) {
        std::cerr << "transport unavailable: " << transport << std::endl;
        return 1;
    }

    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    const std::vector<size_t> msg_sizes = perf_multi_client::resolve_case_msg_sizes (fallback_size);
    const size_t max_msg_size =
      perf_multi_client::resolve_case_max_msg_size (fallback_size, msg_sizes);

    ctx_guard_t ctx;
    if (!ctx.valid ())
        return 1;

    client_state_t state;
    if (!setup_client_state (config, ctx, transport, endpoint, settings, max_msg_size, &state)) {
        close_client_state (&state);
        return 1;
    }

    const bool websocket_transport = transport == "ws" || transport == "wss";

    for (size_t si = 0; si < msg_sizes.size (); ++si) {
        const size_t msg_size = msg_sizes[si];
        const uint32_t run_id = perf_multi_client::next_metric_run_id ();
        unsigned long long reply_count = 0;
        bench_latency_stats_t latency;
        if (!run_active_window (config, &state, settings, websocket_transport,
                                run_id, msg_size, &reply_count, &latency)) {
            close_client_state (&state);
            return 1;
        }
        if (reply_count == 0) {
            if (bench_debug_enabled ()) {
                std::cerr << "[perf-multi-socket-reqrep] no active replies pattern="
                          << config.pattern_name << " size=" << msg_size << std::endl;
            }
            close_client_state (&state);
            return 1;
        }
        const double throughput = static_cast<double> (reply_count)
                                  / static_cast<double> (std::max (1, settings.duration_seconds));
        perf_multi_client::print_echo_client_result_lines (config.pattern_name, lib_name,
                                                           transport, msg_size, throughput,
                                                           latency);
        std::cout << "CLIENT_DONE," << msg_size << std::endl;
    }
    if (!wait_for_runner_stop_after_done ()) {
        close_client_state (&state);
        return 1;
    }

    close_client_state (&state);
    return 0;
}

inline bool submit_router_reply_with_retry (void *server,
                                            const zlink_routing_id_t *source_rid,
                                            uint64_t reply_token,
                                            zlink_msg_t *part)
{
    if (!server || !source_rid || reply_token == 0 || !part)
        return false;

    if (perf_measurement_part_count () == 2u) {
#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
        const zlink_submit_result_t payload_rc = zlink_router_reply_part (
#else
        const zlink_submit_result_t payload_rc = zlink_reply_part (
#endif
          server, source_rid, reply_token, part, ZLINK_PART_MORE);
        if (payload_rc != ZLINK_SUBMIT_OK)
            return false;
        while (!perf_stop_requested ().load (std::memory_order_acquire)) {
            zlink_msg_t empty_part;
            if (zlink_msg_init (&empty_part) != 0)
                return false;
#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
            const zlink_submit_result_t final_rc = zlink_router_reply_part (
#else
            const zlink_submit_result_t final_rc = zlink_reply_part (
#endif
              server, source_rid, reply_token, &empty_part, ZLINK_PART_FINAL);
            if (final_rc == ZLINK_SUBMIT_OK)
                return true;
            if (final_rc != ZLINK_SUBMIT_BACKPRESSURED)
                return false;
            zlink_pollitem_t item = {server, 0, ZLINK_POLLOUT, 0};
            if (perf_socket_poll (&item, 1, perf_aux_poll_wait_ms ()) < 0
                && zlink_errno () != EINTR && zlink_errno () != EAGAIN)
                return false;
        }
        return false;
    }

    // Reply submission consumes the supplied part on backpressure. Keep a
    // shared-storage copy so the retry preserves the received metric payload.
    zlink_msg_t retry_template;
    const bool retry_template_initialized = zlink_msg_init (&retry_template) == 0;
    if (!retry_template_initialized
        || zlink_msg_copy (&retry_template, part) != ZLINK_CONFIG_OK) {
        if (retry_template_initialized)
            zlink_msg_close (&retry_template);
        zlink_msg_close (part);
        return false;
    }

#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
    zlink_submit_result_t reply_rc =
      zlink_router_reply_part (server, source_rid, reply_token, part, ZLINK_PART_FINAL);
#else
    zlink_submit_result_t reply_rc =
      zlink_reply_part (server, source_rid, reply_token, part, ZLINK_PART_FINAL);
#endif
    while (reply_rc == ZLINK_SUBMIT_BACKPRESSURED
           && !perf_stop_requested ().load (std::memory_order_acquire)) {
        zlink_pollitem_t item = {server, 0, ZLINK_POLLOUT, 0};
        const int poll_rc = perf_socket_poll (&item, 1, perf_aux_poll_wait_ms ());
        if (poll_rc < 0) {
            if (zlink_errno () == EINTR || zlink_errno () == EAGAIN)
                continue;
            break;
        }
        if (poll_rc == 0 || (item.revents & ZLINK_POLLOUT) == 0)
            continue;

        zlink_msg_t retry;
        const bool retry_initialized = zlink_msg_init (&retry) == 0;
        if (!retry_initialized
            || zlink_msg_copy (&retry, &retry_template) != ZLINK_CONFIG_OK) {
            if (retry_initialized)
                zlink_msg_close (&retry);
            reply_rc = ZLINK_SUBMIT_TERMINATED;
            break;
        }
#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
        reply_rc = zlink_router_reply_part (
#else
        reply_rc = zlink_reply_part (
#endif
          server, source_rid, reply_token, &retry, ZLINK_PART_FINAL);
    }

    zlink_msg_close (&retry_template);
    return reply_rc == ZLINK_SUBMIT_OK;
}

enum server_recv_step_t
{
    server_recv_step_replied = 0,
    server_recv_step_drained = 1,
    server_recv_step_error = 2
};

inline server_recv_step_t reply_one_request (void *server,
                                             void *ctx,
                                             uint64_t hwm_value,
                                             const std::string &transport,
                                             zlink_socket_type_t socket_type,
                                             size_t *active_msg_size)
{
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t reply_token = 0;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&part) != 0)
        return server_recv_step_error;

    const int rc =
      zlink_router_recv_part (server, &source_rid, &reply_token, &part,
                              &has_more, static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
    if (rc != 0) {
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR)
            return server_recv_step_drained;
        return server_recv_step_error;
    }

    if (!source_rid || source_rid->size == 0 || reply_token == 0
        || !perf_zlink_recv_measurement_tail (
          server, has_more, ZLINK_RECV_FLAGS_DONTWAIT, perf_zlink_recv_next_router)) {
        zlink_msg_close (&part);
        errno = EPROTO;
        return server_recv_step_error;
    }

    const size_t msg_size = zlink_msg_size (&part);
    if (active_msg_size && msg_size > 0 && *active_msg_size != msg_size) {
        apply_benchmark_hwm (server, hwm_value);
        *active_msg_size = msg_size;
        perf_print_auto_hwm_snapshot (server, false, "server", transport, true, msg_size,
                                      socket_type);
    }

    if (submit_router_reply_with_retry (server, source_rid, reply_token, &part))
        return server_recv_step_replied;

    const int reply_err = zlink_errno ();
    // Once the runner has requested teardown, an in-flight reply may stop on
    // ENOTCONN or leave no errno when a backpressure retry observes STOP.
    // Neither outcome is a measurement failure after CLIENT_DONE.
    if (perf_stop_requested ().load (std::memory_order_acquire)) {
        return server_recv_step_drained;
    }
    if (bench_debug_enabled ()) {
        std::cerr << "[perf-multi-socket-reqrep] reply failed err=" << reply_err << std::endl;
    }
    return server_recv_step_error;
}

inline bool run_server_loop (void *server,
                             void *ctx,
                             uint64_t hwm_value,
                             const std::string &transport,
                             zlink_socket_type_t socket_type)
{
    size_t active_msg_size = 0;
    while (!perf_stop_requested ().load (std::memory_order_acquire)) {
        zlink_pollitem_t item = {server, 0, ZLINK_POLLIN, 0};
        // The stdin watcher sets perf_stop_requested(), but a forever poll
        // would not observe that flag after the last request is complete.
        // Use the common auxiliary wait so STOP can finish the server cleanly.
        const int poll_rc = perf_socket_poll (&item, 1, perf_aux_poll_wait_ms ());
        if (poll_rc < 0) {
            if (zlink_errno () == EINTR)
                continue;
            return false;
        }
        if (perf_stop_requested ().load (std::memory_order_acquire))
            break;
        if ((item.revents & ZLINK_POLLIN) == 0)
            continue;
        for (;;) {
            const server_recv_step_t step =
              reply_one_request (server, ctx, hwm_value, transport, socket_type, &active_msg_size);
            if (step == server_recv_step_error)
                return false;
            if (step == server_recv_step_drained)
                break;
        }
    }
    return true;
}

inline int run_server_benchmark (const endpoint_config_t &config,
                                 const std::string &lib_name,
                                 const std::string &transport,
                                 size_t initial_msg_size)
{
    set_perf_multi_pattern_env (config.pattern_name);

    if (!perf_multi_client::is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << "," << config.pattern_name << "," << transport
                  << std::endl;
        return 0;
    }
    if (!transport_available (transport)) {
        std::cerr << "transport unavailable: " << transport << std::endl;
        return 1;
    }

    ctx_guard_t ctx;
    if (!ctx.valid ())
        return 1;

    void *server = zlink_socket (ctx.get (), config.server_socket_type);
    if (!server)
        return 1;

    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    if (initial_msg_size == 0) {
        zlink_close (server);
        return 1;
    }
    const int linger_ms = 0;
    const int send_timeout_ms =
      bench_timeout_ms_from_env ("PERF_MULTI_SNDTIMEO_MS", bench_timeout_ms_from_env (
                                                            "PERF_SNDTIMEO_MS", 200));
    set_sockopt_int (server, ZLINK_OPT_LINGER, linger_ms, "ZLINK_OPT_LINGER");
    set_sockopt_int (server, ZLINK_OPT_SNDTIMEO, send_timeout_ms, "ZLINK_OPT_SNDTIMEO");
    apply_benchmark_hwm (server, settings.hwm);
    if (config.server_has_routing_id && config.server_routing_id) {
        zlink_set_routing_id (server, config.server_routing_id,
                              std::strlen (config.server_routing_id));
    }
    if (!setup_tls_server (server, transport)) {
        zlink_close (server);
        return 1;
    }

    const std::string endpoint = bind_server_endpoint (
      server, transport, lib_name + std::string ("_") + config.token + "_server");
    if (endpoint.empty ()) {
        zlink_close (server);
        return 1;
    }

    perf_stop_requested ().store (false, std::memory_order_release);
    install_perf_signal_handlers ();
    std::thread stdin_watcher ([] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            if (line == "STOP" || line == "QUIT") {
                perf_stop_requested ().store (true, std::memory_order_release);
                return;
            }
        }
        perf_stop_requested ().store (true, std::memory_order_release);
    });
    stdin_watcher.detach ();

    std::cout << "READY," << endpoint << std::endl;
    const bool ok =
      run_server_loop (server, ctx.get (), settings.hwm, transport, config.server_socket_type);
    zlink_close (server);
    return ok ? 0 : 1;
}

} // namespace perf_multi_socket_reqrep

#endif
