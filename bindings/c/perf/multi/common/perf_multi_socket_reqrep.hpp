#ifndef PERF_MULTI_SOCKET_REQREP_HPP
#define PERF_MULTI_SOCKET_REQREP_HPP

#include "perf_common.hpp"
#include "perf_common_multi.hpp"
#include "perf_multi_client_helpers.hpp"
#include "perf_multi_metric_header.hpp"

#include "../../common/perf_tls_setup.hpp"

#include <algorithm>
#include <atomic>
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
        waiting_reply (false),
        payload ()
    {
    }

    client_state_t *owner;
    void *socket;
    size_t index;
    uint64_t next_seq;
    bool waiting_reply;
    std::vector<char> payload;
};

struct client_state_t
{
    client_state_t () :
        poller (NULL),
        active_run_id (0),
        active_msg_size (0),
        active_deadline_ns (0),
        active_reply_count (0),
        fatal (false),
        slots (),
        events (),
        latency ()
    {
    }

    void *poller;
    uint32_t active_run_id;
    size_t active_msg_size;
    uint64_t active_deadline_ns;
    unsigned long long active_reply_count;
    bool fatal;
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

inline bool is_transient_submit (zlink_submit_result_t rc, int err)
{
    return rc == ZLINK_SUBMIT_BACKPRESSURED || err == EAGAIN || err == EWOULDBLOCK
           || err == EINTR || err == ETIMEDOUT;
}

inline void on_request_reply (zlink_request_result_t result,
                              zlink_msg_t *parts,
                              size_t part_count,
                              void *userdata)
{
    client_slot_t *slot = static_cast<client_slot_t *> (userdata);
    if (!slot || !slot->owner)
        return;

    client_state_t *state = slot->owner;
    slot->waiting_reply = false;

    if (result != ZLINK_REQUEST_OK || !parts || part_count == 0)
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
    state->latency.add (sample_ns);
}

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

    const size_t payload_size = std::max<size_t> (msg_size, perf_multi_metric::header_size ());
    if (slot->payload.size () != payload_size)
        slot->payload.assign (payload_size, 'c');

    if (!perf_multi_metric::stamp_payload (slot->payload.data (), payload_size, run_id,
                                           perf_multi_metric::phase_active, msg_size,
                                           slot->next_seq, perf_multi_metric::now_ns ())) {
        return false;
    }

    zlink_msg_t part;
    if (zlink_msg_init_size (&part, payload_size) != 0)
        return false;
    if (payload_size > 0)
        std::memcpy (zlink_msg_data (&part), slot->payload.data (), payload_size);

    slot->waiting_reply = true;
    zlink_submit_result_t rc = ZLINK_SUBMIT_INVALID_ARGUMENT;
    if (config.client_router_request) {
        if (!target_rid || target_rid->size == 0) {
            zlink_msg_close (&part);
            slot->waiting_reply = false;
            errno = EINVAL;
            return false;
        }
        rc = zlink_router_request_part (slot->socket, target_rid, &part, ZLINK_DONTWAIT,
                                        ZLINK_PART_FINAL, timeout_ms, on_request_reply, slot);
    } else {
        rc = zlink_dealer_request_part (slot->socket, &part, ZLINK_DONTWAIT, ZLINK_PART_FINAL,
                                        timeout_ms, on_request_reply, slot);
    }

    if (rc == ZLINK_SUBMIT_OK) {
        ++slot->next_seq;
        return true;
    }

    const int err = zlink_errno ();
    zlink_msg_close (&part);
    slot->waiting_reply = false;
    if (is_transient_submit (rc, err)) {
        if (blocked_out)
            *blocked_out = true;
        return true;
    }
    return false;
}

inline bool any_waiting_reply (const client_state_t &state)
{
    for (size_t i = 0; i < state.slots.size (); ++i) {
        if (state.slots[i].waiting_reply)
            return true;
    }
    return false;
}

inline bool drain_pending_replies (client_state_t *state, uint32_t request_timeout_ms)
{
    if (!state || !state->poller) {
        errno = EINVAL;
        return false;
    }

    const int drain_ms = std::max<int> (1000, static_cast<int> (request_timeout_ms) * 4);
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (drain_ms);
    while (any_waiting_reply (*state) && std::chrono::steady_clock::now () < deadline) {
        const int wait_ms = poll_timeout_until (deadline, 50);
        const int event_count =
          zlink_poller_wait (state->poller, state->events.empty () ? NULL : &state->events[0],
                             static_cast<int> (state->events.size ()), wait_ms, NULL);
        if (event_count < 0) {
            if (zlink_errno () == EINTR)
                continue;
            return false;
        }
    }
    return true;
}

inline bool run_active_window (const endpoint_config_t &config,
                               client_state_t *state,
                               const multi_bench_settings_t &settings,
                               uint32_t run_id,
                               size_t msg_size,
                               unsigned long long *reply_count_out,
                               bench_latency_stats_t *latency_out)
{
    if (!state || !state->poller || !reply_count_out || !latency_out) {
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
      + static_cast<uint64_t> (std::max (1, settings.duration_seconds)) * 1000000000ULL;
    state->active_reply_count = 0;
    state->fatal = false;
    state->latency.reset ();
    for (size_t i = 0; i < state->slots.size (); ++i) {
        state->slots[i].waiting_reply = false;
        state->slots[i].next_seq = 1;
    }

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::seconds (std::max (1, settings.duration_seconds));
    const uint32_t request_timeout_ms =
      static_cast<uint32_t> (bench_timeout_ms_from_env ("PERF_MULTI_REQREP_TIMEOUT_MS", 200));

    while (std::chrono::steady_clock::now () < deadline && !state->fatal) {
        bool progress = false;
        for (size_t i = 0; i < state->slots.size (); ++i) {
            client_slot_t &slot = state->slots[i];
            if (slot.waiting_reply)
                continue;

            bool blocked = false;
            if (!submit_request (config, &slot, target_rid_ptr, run_id, msg_size,
                                 request_timeout_ms, &blocked)) {
                state->fatal = true;
                break;
            }
            if (!blocked)
                progress = true;
        }
        if (progress)
            continue;

        const int wait_ms = poll_timeout_until (deadline, 50);
        if (wait_ms <= 0)
            break;
        const int event_count =
          zlink_poller_wait (state->poller, state->events.empty () ? NULL : &state->events[0],
                             static_cast<int> (state->events.size ()), wait_ms, NULL);
        if (event_count < 0) {
            if (zlink_errno () == EINTR)
                continue;
            return false;
        }
    }

    if (state->fatal)
        return false;
    if (!drain_pending_replies (state, request_timeout_ms))
        return false;

    *reply_count_out = state->active_reply_count;
    *latency_out = state->latency.snapshot ();
    return true;
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
      ctx.get (), sockets, config.client_socket_type, settings.hwm, transport, max_msg_size);

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

    for (size_t si = 0; si < msg_sizes.size (); ++si) {
        const size_t msg_size = msg_sizes[si];
        const uint32_t run_id = perf_multi_client::next_metric_run_id ();
        unsigned long long reply_count = 0;
        bench_latency_stats_t latency;
        if (!run_active_window (config, &state, settings, run_id, msg_size, &reply_count,
                                &latency)) {
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
    }

    close_client_state (&state);
    return 0;
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
    uint64_t request_seq = 0;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&part) != 0)
        return server_recv_step_error;

    const int rc =
      zlink_router_recv_part (server, &source_rid, &request_seq, &part,
                              &has_more, static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
    if (rc != 0) {
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR)
            return server_recv_step_drained;
        return server_recv_step_error;
    }

    if (!source_rid || source_rid->size == 0 || request_seq == 0 || has_more != ZLINK_PART_FINAL) {
        zlink_msg_close (&part);
        errno = EPROTO;
        return server_recv_step_error;
    }

    const size_t msg_size = zlink_msg_size (&part);
    if (active_msg_size && msg_size > 0 && *active_msg_size != msg_size) {
        if (!apply_benchmark_context_auto_hwm_msg_unit (ctx, msg_size)) {
            zlink_msg_close (&part);
            return server_recv_step_error;
        }
        apply_benchmark_hwm (server, hwm_value);
        *active_msg_size = msg_size;
        perf_print_auto_hwm_snapshot (server, false, "server", transport, true, msg_size,
                                      socket_type);
    }

    const zlink_submit_result_t reply_rc =
      zlink_router_reply_part (server, source_rid, request_seq, &part, ZLINK_PART_FINAL);
    if (reply_rc == ZLINK_SUBMIT_OK)
        return server_recv_step_replied;

    if (bench_debug_enabled ()) {
        std::cerr << "[perf-multi-socket-reqrep] reply failed rc=" << reply_rc
                  << " err=" << zlink_errno () << std::endl;
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
        const int poll_rc = perf_socket_poll (&item, 1, -1);
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
                                 const std::string &transport)
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
