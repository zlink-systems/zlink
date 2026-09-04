#ifndef PERF_MULTI_CLIENT_HELPERS_HPP
#define PERF_MULTI_CLIENT_HELPERS_HPP

#include "perf_common.hpp"
#include "perf_common_multi.hpp"
#include "perf_multi_metric_header.hpp"
#include "../../common/perf_tls_setup.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace perf_multi_client
{

using ::setup_tls_client;

static std::atomic<int> g_debug_one_way_logs (0);
static std::atomic<int> g_debug_header_logs (0);

enum send_status_t
{
    send_ok = 0,
    send_blocked = 1,
    send_error = 2
};

// DONTWAIT FINAL can be retained by Core after the physical send HWM is
// full.  Keep at most one such record per socket: immediate admissions still
// run at saturation speed, while the application-owned pending backlog cannot
// grow toward the socket-wide 65,536 completion reservation limit.
inline size_t send_completion_limit_per_socket ()
{
    return 1;
}

inline int send_completion_drain_timeout_ms ()
{
    return resolve_multi_int_env ("PERF_MULTI_SEND_DRAIN_TIMEOUT_MS", 5000, 1);
}

inline int latency_phase_duration_seconds ()
{
    return 1;
}

inline size_t latency_phase_max_in_flight_per_socket ()
{
    return 1;
}

struct send_completion_slot_t
{
    send_completion_slot_t () :
        socket (NULL), pending (0), completion_id (0), replies (0), send_blocked (false)
    {
    }

    void *socket;
    size_t pending;
    zlink_completion_id_t completion_id;
    size_t replies;
    bool send_blocked;
};

struct send_completion_tracker_t
{
    send_completion_tracker_t () : poller (NULL), base_events (0), slots (), events () {}
    ~send_completion_tracker_t ()
    {
        if (poller)
            zlink_poller_destroy (&poller);
    }

  private:
    send_completion_tracker_t (const send_completion_tracker_t &);
    send_completion_tracker_t &operator= (const send_completion_tracker_t &);

  public:

    void *poller;
    short base_events;
    std::vector<send_completion_slot_t> slots;
    std::vector<zlink_poller_event_t> events;
};

inline short tracker_slot_events (const send_completion_tracker_t &tracker,
                                  const send_completion_slot_t &slot)
{
    short events = static_cast<short> (tracker.base_events | ZLINK_POLLCOMPLETION);
    if (slot.send_blocked)
        events = static_cast<short> (events | ZLINK_POLLOUT);
    return events;
}

inline bool update_tracker_slot_events (send_completion_tracker_t *tracker,
                                        send_completion_slot_t *slot)
{
    return tracker && tracker->poller && slot && slot->socket
           && zlink_poller_modify (tracker->poller, slot->socket,
                                   tracker_slot_events (*tracker, *slot))
                == ZLINK_CONFIG_OK;
}

inline void close_send_completion_tracker (send_completion_tracker_t *tracker)
{
    if (!tracker)
        return;
    if (tracker->poller)
        zlink_poller_destroy (&tracker->poller);
    tracker->slots.clear ();
    tracker->events.clear ();
}

inline bool create_send_completion_tracker (const std::vector<void *> &sockets,
                                            short base_events,
                                            send_completion_tracker_t *tracker)
{
    if (!tracker || sockets.empty ())
        return false;

    close_send_completion_tracker (tracker);
    tracker->poller = zlink_poller_new ();
    if (!tracker->poller)
        return false;
    tracker->base_events = base_events;
    tracker->slots.resize (sockets.size ());
    tracker->events.resize (sockets.size ());

    for (size_t i = 0; i < sockets.size (); ++i) {
        send_completion_slot_t &slot = tracker->slots[i];
        slot.socket = sockets[i];
        if (!slot.socket
            || zlink_poller_add (tracker->poller, slot.socket, &slot,
                                 tracker_slot_events (*tracker, slot))
                 != ZLINK_CONFIG_OK) {
            close_send_completion_tracker (tracker);
            return false;
        }
    }
    return true;
}

inline bool record_send_completion (send_completion_slot_t *slot,
                                    const zlink_completion_t &completion)
{
    if (!slot || completion.kind != ZLINK_COMPLETION_SEND
        || completion.completion_id == 0 || completion.user_context != slot->socket
        || slot->pending != 1 || completion.completion_id != slot->completion_id) {
        errno = EPROTO;
        return false;
    }

    --slot->pending;
    slot->completion_id = 0;
    if (completion.send_result == ZLINK_SEND_ADMITTED)
        return true;

    errno = completion.send_terminal_errno != 0 ? completion.send_terminal_errno : EIO;
    return false;
}

inline bool drain_send_completions (send_completion_slot_t *slot)
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
        const bool valid = record_send_completion (slot, completion);
        zlink_completion_close (&completion);
        if (!valid)
            return false;
    }
}

inline bool tracker_has_pending_completions (const send_completion_tracker_t &tracker)
{
    for (size_t i = 0; i < tracker.slots.size (); ++i) {
        if (tracker.slots[i].pending != 0)
            return true;
    }
    return false;
}

inline bool tracker_has_pending_replies (const send_completion_tracker_t &tracker)
{
    for (size_t i = 0; i < tracker.slots.size (); ++i) {
        if (tracker.slots[i].replies != 0)
            return true;
    }
    return false;
}

inline send_completion_slot_t *tracker_event_slot (const zlink_poller_event_t &event)
{
    send_completion_slot_t *slot = static_cast<send_completion_slot_t *> (event.user_data);
    return slot && slot->socket == event.socket ? slot : NULL;
}

inline bool drain_tracker_completion_events (send_completion_tracker_t *tracker, int event_count)
{
    if (!tracker || event_count < 0)
        return false;
    for (int i = 0; i < event_count; ++i) {
        zlink_poller_event_t &event = tracker->events[static_cast<size_t> (i)];
        send_completion_slot_t *slot = tracker_event_slot (event);
        if (!slot) {
            errno = EPROTO;
            return false;
        }
        if ((event.events & ZLINK_POLLCOMPLETION) != 0
            && !drain_send_completions (slot)) {
            return false;
        }
        if ((event.events & ZLINK_POLLOUT) != 0 && slot->send_blocked) {
            slot->send_blocked = false;
            if (!update_tracker_slot_events (tracker, slot))
                return false;
        }
    }
    return true;
}

inline bool is_supported_transport (const std::string &transport)
{
    if (transport == "tcp" || transport == "tls" || transport == "ws" || transport == "wss")
        return true;
#if !defined(_WIN32)
    if (transport == "ipc")
        return true;
#endif
    if (transport == "inproc")
        return true;
    return false;
}

inline bool parse_endpoint_arg (int argc, char **argv, std::string *endpoint_out)
{
    if (!endpoint_out)
        return false;

    endpoint_out->clear ();
    for (int i = 4; i + 1 < argc; ++i) {
        if (std::strcmp (argv[i], "--endpoint") == 0) {
            *endpoint_out = argv[i + 1];
            return !endpoint_out->empty ();
        }
    }

    return false;
}

inline send_status_t classify_send_result (zlink_submit_result_t rc)
{
    if (rc == ZLINK_SUBMIT_OK)
        return send_ok;
    const int err = zlink_errno ();
    if (err == EAGAIN)
        return send_blocked;
    return send_error;
}

inline send_status_t send_echo_message_flags (void *socket,
                                              const zlink_routing_id_t *target_rid,
                                              std::vector<char> &payload,
                                              size_t payload_size,
                                              bool router_send,
                                              zlink_send_flags_t base_flags,
                                              bool per_socket_payload,
                                              void *completion_context = NULL,
                                              zlink_completion_id_t *completion_id_out = NULL)
{
    if (completion_id_out)
        *completion_id_out = 0;
    if (payload_size > payload.size ())
        return send_error;

    zlink_msg_t part;
    (void) per_socket_payload;
    if (zlink_msg_init_size (&part, payload_size) != 0)
        return send_error;
    if (payload_size > 0) {
        std::memcpy (zlink_msg_data (&part), payload.data (), payload_size);
    }

    const bool multipart = perf_measurement_part_count () != 1u;
    zlink_msg_t empty_part;
    bool empty_part_initialized = false;
    if (multipart) {
        if (zlink_msg_init (&empty_part) != 0) {
            zlink_msg_close (&part);
            return send_error;
        }
        empty_part_initialized = true;
    }

    zlink_submit_result_t rc = ZLINK_SUBMIT_INVALID_ARGUMENT;
    if (router_send && (!target_rid || target_rid->size == 0)) {
        errno = EINVAL;
    } else if (router_send) {
        rc = zlink_send_part_rid (
          socket, target_rid, &part, base_flags,
          multipart ? ZLINK_PART_MORE : ZLINK_PART_FINAL,
          multipart ? NULL : completion_context,
          multipart ? NULL : completion_id_out);
        if (rc == ZLINK_SUBMIT_OK && multipart) {
            rc = zlink_send_part_rid (socket, target_rid, &empty_part, base_flags,
                                      ZLINK_PART_FINAL, completion_context,
                                      completion_id_out);
        }
    } else {
        rc = zlink_send_part (socket, &part, base_flags,
                              multipart ? ZLINK_PART_MORE : ZLINK_PART_FINAL,
                              multipart ? NULL : completion_context,
                              multipart ? NULL : completion_id_out);
        if (rc == ZLINK_SUBMIT_OK && multipart) {
            rc = zlink_send_part (socket, &empty_part, base_flags, ZLINK_PART_FINAL,
                                  completion_context, completion_id_out);
        }
    }

    // Every part submission consumes its input into an empty initialized
    // message, on both success and failure. Close those handles exactly once;
    // the preinitialized FINAL also makes allocation failure impossible after
    // a MORE prefix has already been staged.
    const int send_errno = rc == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    const send_status_t status = classify_send_result (rc);
    zlink_msg_close (&part);
    if (empty_part_initialized)
        zlink_msg_close (&empty_part);
    if (send_errno != 0)
        errno = send_errno;
    return status;
}

inline send_status_t send_echo_message (void *socket,
                                        const zlink_routing_id_t *target_rid,
                                        std::vector<char> &payload,
                                        size_t payload_size,
                                        bool router_send,
                                        bool per_socket_payload,
                                        void *completion_context = NULL,
                                        zlink_completion_id_t *completion_id_out = NULL)
{
    return send_echo_message_flags (socket, target_rid, payload, payload_size, router_send,
                                    ZLINK_DONTWAIT, per_socket_payload, completion_context,
                                    completion_id_out);
}

inline int recv_one_message (
  void *socket, bool router_surface, std::vector<char> &scratch, int flags, size_t capture_bytes)
{
    if (!socket)
        return -1;

    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&part) != 0)
        return -1;

    int rc = -1;
    zlink_reply_token_t reply_token = 0;
    if (router_surface) {
        rc = ::zlink_router_recv_part (socket, &source_rid, &reply_token, &part,
                                       &has_more, static_cast<zlink_recv_flags_t> (flags));
    } else {
        rc = ::zlink_recv_part (socket, &source_rid, &part, &has_more,
                                static_cast<zlink_recv_flags_t> (flags));
    }
    if (rc != 0) {
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err == EAGAIN || err == EINTR)
            return 0;
        return -1;
    }

    const perf_zlink_recv_next_fn recv_next = router_surface
                                                ? ::perf_zlink_recv_next_router
                                                : ::perf_zlink_recv_next_plain;
    if (router_surface
        && ((source_rid && source_rid->size == 0) || reply_token != 0
            || !::perf_zlink_recv_measurement_tail (
              socket, has_more, static_cast<zlink_recv_flags_t> (flags), recv_next))) {
        zlink_msg_close (&part);
        errno = EPROTO;
        return -1;
    }

    if (!router_surface
        && (source_rid || !::perf_zlink_recv_measurement_tail (
          socket, has_more, static_cast<zlink_recv_flags_t> (flags), recv_next))) {
        zlink_msg_close (&part);
        errno = EPROTO;
        return -1;
    }

    if (capture_bytes > 0 && !scratch.empty ()) {
        const size_t copy_size =
          std::min (std::min (capture_bytes, scratch.size ()), zlink_msg_size (&part));
        if (copy_size > 0) {
            std::memcpy (scratch.data (), zlink_msg_data (&part), copy_size);
        }
    }

    zlink_msg_close (&part);
    return 1;
}

inline int
recv_one_message (void *socket, bool router_surface, std::vector<char> &scratch, int flags)
{
    return recv_one_message (socket, router_surface, scratch, flags, scratch.size ());
}

inline bool wait_client_connect_ready_all (std::vector<ready_monitor_t> &monitors, int timeout_ms)
{
    if (monitors.empty ())
        return true;

    const int bounded_timeout = timeout_ms > 0 ? timeout_ms : 0;
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (bounded_timeout);

    for (size_t i = 0; i < monitors.size (); ++i) {
        const auto now = std::chrono::steady_clock::now ();
        if (now >= deadline)
            return false;

        const int remaining_ms = static_cast<int> (
          std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now).count ());
        if (!wait_for_socket_monitor_event (monitors[i], ZLINK_EVENT_CONNECTION_READY,
                                            remaining_ms))
            return false;
    }

    return true;
}

inline void close_client_sockets (std::vector<void *> *sockets)
{
    if (!sockets)
        return;

    for (size_t i = 0; i < sockets->size (); ++i) {
        if ((*sockets)[i]) {
            zlink_close ((*sockets)[i]);
            (*sockets)[i] = NULL;
        }
    }
}

inline void close_client_monitors (std::vector<ready_monitor_t> *monitors)
{
    if (!monitors)
        return;

    for (size_t i = 0; i < monitors->size (); ++i)
        close_ready_monitor ((*monitors)[i]);
}

inline bool create_client_sockets (ctx_guard_t &ctx,
                                   const std::string &transport,
                                   const std::string &endpoint,
                                   const multi_bench_settings_t &settings,
                                   int client_socket_type,
                                   size_t msg_size,
                                   std::vector<void *> *sockets_out,
                                   std::vector<ready_monitor_t> *monitors_out,
                                   bool print_auto_hwm_snapshot = true)
{
    if (!sockets_out)
        return false;

    sockets_out->assign (settings.clients, NULL);
    if (monitors_out)
        monitors_out->assign (settings.clients, ready_monitor_t ());

    for (size_t i = 0; i < sockets_out->size (); ++i) {
        void *sock =
          zlink_socket (ctx.get (), static_cast<zlink_socket_type_t> (client_socket_type));
        if (!sock)
            return false;

        apply_benchmark_socket_options (sock, settings.hwm, transport,
                                        static_cast<zlink_socket_type_t> (client_socket_type),
                                        msg_size, print_auto_hwm_snapshot);

        if (client_socket_type == ZLINK_SOCKET_ROUTER
            || client_socket_type == ZLINK_SOCKET_DEALER) {
            char id_buf[32];
            const int id_len = std::snprintf (id_buf, sizeof (id_buf), "client_%zu", i);
            if (id_len > 0) {
                zlink_set_routing_id (sock, id_buf, static_cast<size_t> (id_len));
            }
        }

        if (client_socket_type == ZLINK_SOCKET_ROUTER) {
            static const char k_server_connect_id[] = "SERVER";
            if (zlink_set_router_option (sock, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                         k_server_connect_id, sizeof (k_server_connect_id) - 1)
                != 0) {
                if (bench_debug_enabled ()) {
                    std::cerr << "set router connect_routing_id failed: "
                              << zlink_strerror (zlink_errno ()) << std::endl;
                }
                zlink_close (sock);
                return false;
            }
        }

        if (client_socket_type == ZLINK_SOCKET_SUB) {
            static const char k_subscribe_all[] = "";
            if (zlink_set_subscription (sock, k_subscribe_all) != ZLINK_CONFIG_OK) {
                if (bench_debug_enabled ()) {
                    std::cerr << "set_subscription failed: " << zlink_strerror (zlink_errno ())
                              << std::endl;
                }
                zlink_close (sock);
                return false;
            }
        }

        if (!setup_tls_client (sock, transport)) {
            zlink_close (sock);
            return false;
        }

        if (monitors_out) {
            const bool monitor_opened = open_configured_socket_monitor (
              sock,
              ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_BIND_FAILED | ZLINK_EVENT_ACCEPT_FAILED
                | ZLINK_EVENT_CLOSE_FAILED | ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL
                | ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL | ZLINK_EVENT_HANDSHAKE_FAILED_AUTH,
              &(*monitors_out)[i]);
            if (!monitor_opened) {
                zlink_close (sock);
                return false;
            }
        }

        if (zlink_connect (sock, endpoint.c_str ()) != ZLINK_CONNECT_OK) {
            std::cerr << "connect failed for " << endpoint << ": "
                      << zlink_strerror (zlink_errno ()) << std::endl;
            if (monitors_out)
                close_ready_monitor ((*monitors_out)[i]);
            zlink_close (sock);
            return false;
        }
        (*sockets_out)[i] = sock;
    }

    return true;
}

inline void refresh_connected_client_auto_hwm (const std::vector<void *> &sockets,
                                               zlink_socket_type_t client_socket_type,
                                               uint64_t hwm_value,
                                               const std::string &transport,
                                               size_t msg_size)
{
    for (size_t i = 0; i < sockets.size (); ++i) {
        if (!sockets[i])
            continue;
        apply_benchmark_hwm (sockets[i], hwm_value);
    }

    if (!sockets.empty () && sockets[0]) {
        perf_print_auto_hwm_snapshot (sockets[0], false, "endpoint", transport, false, msg_size,
                                      client_socket_type);
    }
}

inline bool make_routing_id (const char *text, zlink_routing_id_t *routing_id)
{
    if (!text || !routing_id)
        return false;

    const size_t size = std::strlen (text);
    if (size == 0 || size > sizeof (routing_id->data))
        return false;

    std::memset (routing_id, 0, sizeof (*routing_id));
    std::memcpy (routing_id->data, text, size);
    routing_id->size = static_cast<uint8_t> (size);
    return true;
}

inline int remaining_poll_timeout_ms (const std::chrono::steady_clock::time_point &deadline)
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ();
    if (now >= deadline)
        return 0;

    const long remaining_ms =
      std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now).count ();
    if (remaining_ms <= 0)
        return 0;
    if (remaining_ms > INT_MAX)
        return INT_MAX;
    return static_cast<int> (remaining_ms);
}

inline uint32_t next_metric_run_id ()
{
    static std::atomic<uint32_t> next_id (1);
    uint32_t run_id = next_id.fetch_add (1, std::memory_order_relaxed);
    if (run_id == 0)
        run_id = next_id.fetch_add (1, std::memory_order_relaxed);
    return run_id;
}

inline void normalize_latency_stats (double lat_sum,
                                     long lat_count,
                                     bench_latency_sampler_t *samples,
                                     bench_latency_stats_t *latency_out)
{
    if (!latency_out)
        return;
    if (lat_count <= 0) {
        *latency_out = bench_latency_stats_t ();
        return;
    }

    bench_latency_stats_t stats = samples ? samples->snapshot () : bench_latency_stats_t ();
    if (stats.mean_ns <= 0.0)
        stats.mean_ns = lat_sum / static_cast<double> (lat_count);
    if (stats.p95_ns <= 0.0)
        stats.p95_ns = stats.mean_ns;
    if (stats.p99_ns <= 0.0)
        stats.p99_ns = stats.p95_ns;
    *latency_out = stats;
}

inline bool metric_header_matches (const perf_multi_metric::header_t &header,
                                   uint32_t expected_run_id,
                                   perf_multi_metric::phase_t expected_phase,
                                   size_t expected_msg_size)
{
    if (header.magic != perf_multi_metric::k_magic)
        return false;
    if (header.phase != static_cast<uint8_t> (expected_phase))
        return false;
    if (header.msg_size != static_cast<uint32_t> (expected_msg_size))
        return false;
    if (expected_run_id != 0 && header.run_id != expected_run_id)
        return false;
    return true;
}

inline bool stamp_metric_payload (std::vector<char> &payload,
                                  size_t payload_size,
                                  uint32_t run_id,
                                  perf_multi_metric::phase_t phase,
                                  size_t msg_size,
                                  uint64_t seq)
{
    if (payload_size < perf_multi_metric::header_size () || payload_size > payload.size ()) {
        return false;
    }

    return perf_multi_metric::stamp_payload (payload.data (), payload_size, run_id, phase, msg_size,
                                             seq, perf_multi_metric::now_ns ());
}

inline size_t metric_capture_bytes ()
{
    return perf_multi_metric::header_size () + static_cast<size_t> (64);
}

inline bool decode_metric_header_from_capture (const char *data,
                                               size_t size,
                                               perf_multi_metric::header_t *header_out)
{
    if (!header_out || !data || size < perf_multi_metric::header_size ())
        return false;

    for (size_t offset = 0; (offset + perf_multi_metric::header_size ()) <= size; ++offset) {
        perf_multi_metric::header_t candidate;
        if (!perf_multi_metric::decode_header (data + offset, size - offset, &candidate)) {
            continue;
        }
        if (candidate.magic != perf_multi_metric::k_magic)
            continue;
        *header_out = candidate;
        return true;
    }

    return false;
}

inline bool decode_metric_header_from_capture (const std::vector<char> &scratch,
                                               perf_multi_metric::header_t *header_out)
{
    return decode_metric_header_from_capture (scratch.data (), scratch.size (), header_out);
}

inline int recv_one_message_header (void *socket,
                                    bool router_surface,
                                    std::vector<char> &scratch,
                                    int flags,
                                    size_t capture_bytes,
                                    perf_multi_metric::header_t *header_out,
                                    bool *decoded_out)
{
    if (!socket)
        return -1;

    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t reply_token = 0;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (&part) != 0)
        return -1;
    int rc = -1;
    if (router_surface) {
        rc = ::zlink_router_recv_part (socket, &source_rid, &reply_token, &part,
                                       &has_more, static_cast<zlink_recv_flags_t> (flags));
    } else {
        rc = ::zlink_recv_part (socket, &source_rid, &part, &has_more,
                                static_cast<zlink_recv_flags_t> (flags));
    }
    if (rc != 0) {
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err == EAGAIN || err == EINTR)
            return 0;
        return -1;
    }

    const perf_zlink_recv_next_fn recv_next = router_surface
                                                ? ::perf_zlink_recv_next_router
                                                : ::perf_zlink_recv_next_plain;
    if (router_surface
        && (reply_token != 0 || !::perf_zlink_recv_measurement_tail (
          socket, has_more, static_cast<zlink_recv_flags_t> (flags), recv_next))) {
        zlink_msg_close (&part);
        errno = EPROTO;
        return -1;
    }

    if (!router_surface
        && (source_rid || !::perf_zlink_recv_measurement_tail (
          socket, has_more, static_cast<zlink_recv_flags_t> (flags), recv_next))) {
        zlink_msg_close (&part);
        errno = EPROTO;
        return -1;
    }

    bool decoded = false;
    if (header_out) {
        decoded = perf_multi_metric::decode_payload_header (zlink_msg_data (&part),
                                                            zlink_msg_size (&part), header_out);
        if (!decoded && capture_bytes > 0 && !scratch.empty ()) {
            const size_t write_size =
              std::min (std::min (capture_bytes, scratch.size ()), zlink_msg_size (&part));
            if (write_size > 0) {
                std::memcpy (scratch.data (), zlink_msg_data (&part), write_size);
            }
            if (write_size >= perf_multi_metric::header_size ()) {
                decoded =
                  decode_metric_header_from_capture (scratch.data (), write_size, header_out);
            }
        }
    }

    if (decoded_out)
        *decoded_out = decoded;

    zlink_msg_close (&part);
    return 1;
}

inline bool drain_socket_non_blocking (void *socket,
                                       bool router_surface,
                                       std::vector<char> &scratch,
                                       size_t expected_msg_size,
                                       uint32_t expected_run_id,
                                       perf_multi_metric::phase_t expected_phase,
                                       bool collect_latency,
                                       long *recv_count,
                                       double *lat_sum,
                                       long *lat_count,
                                       bench_latency_sampler_t *lat_samples)
{
    if (!socket)
        return false;

    long local_recv = 0;
    while (true) {
        perf_multi_metric::header_t header;
        bool decoded = false;
        const int rc = recv_one_message_header (socket, router_surface, scratch, ZLINK_DONTWAIT,
                                                scratch.size (), &header, &decoded);
        if (rc < 0)
            return false;
        if (rc == 0)
            break;

        if (!decoded) {
            if (bench_debug_enabled ()
                && g_debug_one_way_logs.fetch_add (1, std::memory_order_acq_rel) < 12) {
                std::cerr << "[perf-multi-one-way] header decode failed" << std::endl;
            }
            continue;
        }

        if (header.magic != perf_multi_metric::k_magic)
            continue;

        if (!metric_header_matches (header, expected_run_id, expected_phase, expected_msg_size)) {
            if (bench_debug_enabled ()
                && g_debug_one_way_logs.fetch_add (1, std::memory_order_acq_rel) < 12) {
                std::cerr << "[perf-multi-one-way] header mismatch run=" << header.run_id
                          << " phase=" << static_cast<unsigned int> (header.phase)
                          << " size=" << header.msg_size << " expected_run=" << expected_run_id
                          << " expected_phase=" << static_cast<unsigned int> (expected_phase)
                          << " expected_size=" << expected_msg_size << std::endl;
            }
            continue;
        }

        ++local_recv;
        if (collect_latency && lat_sum && lat_count) {
            const uint64_t now_ns = perf_multi_metric::now_ns ();
            if (header.sent_ts_ns > 0 && now_ns >= header.sent_ts_ns) {
                const double sample_ns = static_cast<double> (now_ns - header.sent_ts_ns);
                *lat_sum += sample_ns;
                (*lat_count)++;
                if (lat_samples)
                    lat_samples->add (sample_ns);
            }
        }
    }

    if (recv_count)
        *recv_count += local_recv;
    return true;
}

inline bool run_one_way_window_loop (const std::vector<void *> &recv_sockets,
                                     bool router_surface,
                                     const multi_bench_settings_t &settings,
                                     size_t expected_msg_size,
                                     uint32_t expected_run_id,
                                     perf_multi_metric::phase_t expected_phase,
                                     size_t scratch_capacity,
                                     double duration_seconds,
                                     bool collect_latency,
                                     long *recv_total,
                                     double *lat_sum,
                                     long *lat_count,
                                     bench_latency_stats_t *latency_stats)
{
    if (recv_sockets.empty ())
        return false;
    if (duration_seconds <= 0.0) {
        if (recv_total)
            *recv_total = 0;
        if (lat_sum)
            *lat_sum = 0.0;
        if (lat_count)
            *lat_count = 0;
        if (latency_stats)
            *latency_stats = bench_latency_stats_t ();
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                            std::chrono::duration<double> (std::max (0.0, duration_seconds)));

    const size_t scratch_size = std::max<size_t> (scratch_capacity, metric_capture_bytes ());

    bool fatal_error = false;
    long recv_sum = 0;
    double lat_sum_local = 0.0;
    long lat_count_local = 0;
    bench_latency_sampler_t lat_samples;
    std::vector<char> scratch (scratch_size, '\0');
    std::vector<zlink_pollitem_t> poll_items (recv_sockets.size ());
    for (size_t i = 0; i < recv_sockets.size (); ++i) {
        const zlink_pollitem_t item = {
          recv_sockets[i],
          0,
          ZLINK_POLLIN,
          0,
        };
        poll_items[i] = item;
    }

    while (std::chrono::steady_clock::now () < deadline && !fatal_error) {
        for (size_t i = 0; i < poll_items.size (); ++i)
            poll_items[i].revents = 0;

        const int prc =
          perf_socket_poll (&poll_items[0], static_cast<int> (poll_items.size ()), -1);
        if (prc < 0) {
            if (zlink_errno () != EINTR) {
                fatal_error = true;
                break;
            }
            continue;
        } else if (prc > 0) {
            for (size_t i = 0; i < poll_items.size (); ++i) {
                if ((poll_items[i].revents & ZLINK_POLLIN) == 0)
                    continue;

                long recv_now = 0;
                if (!drain_socket_non_blocking (recv_sockets[i], router_surface, scratch,
                                                expected_msg_size, expected_run_id, expected_phase,
                                                collect_latency, &recv_now,
                                                collect_latency ? &lat_sum_local : NULL,
                                                collect_latency ? &lat_count_local : NULL,
                                                collect_latency ? &lat_samples : NULL)) {
                    fatal_error = true;
                    break;
                }
                recv_sum += recv_now;
            }
        }

        (void) settings;
    }

    if (recv_total)
        *recv_total = recv_sum;
    if (lat_sum)
        *lat_sum = lat_sum_local;
    if (lat_count)
        *lat_count = lat_count_local;
    if (latency_stats) {
        if (!collect_latency)
            *latency_stats = bench_latency_stats_t ();
        else
            normalize_latency_stats (lat_sum_local, lat_count_local, &lat_samples, latency_stats);
    }

    return !fatal_error;
}

inline bool run_one_way_duration (const std::vector<void *> &recv_sockets,
                                  const multi_bench_settings_t &settings,
                                  size_t msg_size,
                                  uint32_t run_id,
                                  size_t scratch_capacity,
                                  double *throughput_out,
                                  bench_latency_stats_t *latency_out)
{
    if (!throughput_out || !latency_out)
        return false;

    *throughput_out = 0.0;
    *latency_out = bench_latency_stats_t ();
    if (recv_sockets.empty ())
        return false;

    long recv_count = 0;
    double lat_sum = 0.0;
    long lat_count = 0;
    bench_latency_stats_t active_latency;

    if (!run_one_way_window_loop (recv_sockets, false, settings, msg_size, run_id,
                                  perf_multi_metric::phase_active, scratch_capacity,
                                  static_cast<double> (std::max (1, settings.duration_seconds)),
                                  true, &recv_count, &lat_sum, &lat_count, &active_latency)) {
        return false;
    }

    if (recv_count <= 0 || lat_count <= 0) {
        if (bench_debug_enabled ()) {
            std::cerr << "[perf-multi-one-way] active metrics invalid recv=" << recv_count
                      << " lat_count=" << lat_count << " run_id=" << run_id
                      << " msg_size=" << msg_size << std::endl;
        }
        return false;
    }

    *throughput_out = static_cast<double> (recv_count)
                      / static_cast<double> (std::max (1, settings.duration_seconds));
    if (active_latency.mean_ns <= 0.0)
        normalize_latency_stats (lat_sum, lat_count, NULL, &active_latency);
    *latency_out = active_latency;

    return true;
}

inline bool run_echo_window_round_robin (const std::vector<void *> &sockets,
                                         const multi_bench_settings_t &settings,
                                         std::vector<char> &payload,
                                         size_t payload_size,
                                         size_t expected_msg_size,
                                         const std::string &server_id,
                                         bool client_router_send,
                                         uint32_t run_id,
                                         perf_multi_metric::phase_t phase,
                                         size_t scratch_capacity,
                                         double duration_seconds,
                                         bool allow_send,
                                         bool collect_latency,
                                         bool per_socket_payload,
                                         long *recv_total,
                                         double *lat_sum,
                                         long *lat_count,
                                         bench_latency_stats_t *latency_stats,
                                         std::vector<double> *latency_samples_out = NULL,
                                         size_t maximum_latency_sample_cap =
                                           std::numeric_limits<size_t>::max (),
                                         size_t max_replies_in_flight_per_socket = 0)
{
    (void) settings;
    if (sockets.empty ())
        return false;
    if (duration_seconds <= 0.0) {
        if (recv_total)
            *recv_total = 0;
        if (lat_sum)
            *lat_sum = 0.0;
        if (lat_count)
            *lat_count = 0;
        if (latency_stats)
            *latency_stats = bench_latency_stats_t ();
        return true;
    }

    bool fatal_error = false;
    long local_recv = 0;
    double lat_sum_local = 0.0;
    long lat_count_local = 0;
    bench_latency_sampler_t lat_samples (maximum_latency_sample_cap);
    size_t rr = 0;
    uint64_t sequence = 1;

    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                            std::chrono::duration<double> (std::max (0.0, duration_seconds)));

    const size_t scratch_size =
      std::max<size_t> (scratch_capacity, perf_multi_metric::header_size ());

    zlink_routing_id_t target_rid;
    const zlink_routing_id_t *target_rid_ptr = NULL;
    if (client_router_send) {
        // Keep routing-id construction outside the echo hot path. ROUTER_ROUTER
        // measures routed send/recv cost; rebuilding this stable target id on
        // every message hides small-message regressions in benchmark overhead.
        if (!make_routing_id (server_id.c_str (), &target_rid))
            return false;
        target_rid_ptr = &target_rid;
    }

    std::vector<char> scratch (scratch_size, '\0');
    std::vector<std::vector<char>> socket_payloads;
    if (allow_send && per_socket_payload)
        socket_payloads.assign (sockets.size (), payload);

    send_completion_tracker_t tracker;
    if (!create_send_completion_tracker (sockets, ZLINK_POLLIN, &tracker))
        return false;

    const auto drain_socket = [&] (send_completion_slot_t *slot, bool count_metrics) -> bool {
        if (!slot || !slot->socket)
            return false;
        while (true) {
            perf_multi_metric::header_t header;
            std::memset (&header, 0, sizeof (header));
            bool decoded = false;
            const int recv_rc = recv_one_message_header (
              slot->socket, client_router_send, scratch, ZLINK_DONTWAIT, scratch.size (), &header,
              &decoded);
            if (recv_rc < 0)
                return false;
            if (recv_rc == 0)
                return true;
            if (!decoded || !metric_header_matches (header, run_id, phase, expected_msg_size)) {
                if (bench_debug_enabled ()
                    && g_debug_header_logs.fetch_add (1, std::memory_order_acq_rel) < 16) {
                    std::cerr << "[perf-multi-echo] header mismatch phase="
                              << static_cast<unsigned int> (phase) << " run_id=" << run_id
                              << " decoded=" << decoded << " header_run_id=" << header.run_id
                              << " expected_size=" << expected_msg_size
                              << " header_size=" << header.msg_size << " header_phase="
                              << static_cast<unsigned int> (header.phase) << std::endl;
                }
                continue;
            }
            if (slot->replies == 0) {
                errno = EPROTO;
                return false;
            }
            --slot->replies;

            const bool inside_active_deadline = std::chrono::steady_clock::now () < deadline;
            if (!count_metrics || (!collect_latency && !inside_active_deadline))
                continue;

            ++local_recv;
            if (collect_latency) {
                const uint64_t now_ns = perf_multi_metric::now_ns ();
                if (header.sent_ts_ns > 0 && now_ns >= header.sent_ts_ns) {
                    const double sample_ns =
                      static_cast<double> (now_ns - header.sent_ts_ns) * 0.5;
                    lat_sum_local += sample_ns;
                    ++lat_count_local;
                    lat_samples.add (sample_ns);
                }
            }
        }
    };

    const auto service_events = [&] (int event_count, bool count_metrics) -> bool {
        if (!drain_tracker_completion_events (&tracker, event_count))
            return false;
        for (int i = 0; i < event_count; ++i) {
            zlink_poller_event_t &event = tracker.events[static_cast<size_t> (i)];
            if ((event.events & ZLINK_POLLIN) == 0)
                continue;
            send_completion_slot_t *slot = tracker_event_slot (event);
            if (!slot || !drain_socket (slot, count_metrics))
                return false;
        }
        return true;
    };

    while (std::chrono::steady_clock::now () < deadline && !fatal_error) {
        bool submitted = false;
        if (allow_send) {
            const size_t send_start_rr = rr;
            for (size_t attempts = 0; attempts < sockets.size (); ++attempts) {
                const size_t idx = (send_start_rr + attempts) % sockets.size ();
                send_completion_slot_t &slot = tracker.slots[idx];
                if (slot.send_blocked
                    || slot.pending >= send_completion_limit_per_socket ()
                    || (max_replies_in_flight_per_socket > 0
                        && slot.replies >= max_replies_in_flight_per_socket)) {
                    continue;
                }

                std::vector<char> &send_payload =
                  per_socket_payload ? socket_payloads[idx] : payload;
                if (!stamp_metric_payload (send_payload, payload_size, run_id, phase,
                                           expected_msg_size, sequence)) {
                    fatal_error = true;
                    break;
                }

                zlink_completion_id_t completion_id = 0;
                const send_status_t send_rc = send_echo_message (
                  sockets[idx], target_rid_ptr, send_payload, payload_size, client_router_send,
                  per_socket_payload, slot.socket, &completion_id);
                if (send_rc == send_ok) {
                    if (completion_id != 0) {
                        ++slot.pending;
                        slot.completion_id = completion_id;
                    }
                    ++slot.replies;
                    ++sequence;
                    submitted = true;
                    continue;
                }
                if (send_rc == send_blocked) {
                    slot.send_blocked = true;
                    if (!update_tracker_slot_events (&tracker, &slot)) {
                        fatal_error = true;
                        break;
                    }
                    if (bench_debug_enabled ()
                        && g_debug_one_way_logs.fetch_add (1, std::memory_order_acq_rel) < 12) {
                        std::cerr << "[perf-multi-echo] send blocked phase="
                                  << static_cast<unsigned int> (phase) << " idx=" << idx
                                  << " errno=" << zlink_errno () << std::endl;
                    }
                    continue;
                }

                if (bench_debug_enabled ()) {
                    std::cerr << "[perf-multi-echo] send error phase="
                              << static_cast<unsigned int> (phase) << " idx=" << idx
                              << " errno=" << zlink_errno () << std::endl;
                }
                fatal_error = true;
                break;
            }
        }

        if (fatal_error)
            break;
        rr = (rr + 1) % sockets.size ();

        // The echo requester owns the active deadline.  Bound this wait by
        // the remaining interval so a quiet peer cannot leave the active
        // phase blocked past its configured duration.
        const int poll_timeout = submitted ? 0 : remaining_poll_timeout_ms (deadline);
        if (poll_timeout <= 0 && !submitted)
            break;
        const int poll_rc = zlink_poller_wait (
          tracker.poller, tracker.events.empty () ? NULL : &tracker.events[0],
          static_cast<int> (tracker.events.size ()), poll_timeout, NULL);
        if (poll_rc < 0) {
            if (bench_debug_enabled ()) {
                std::cerr << "[perf-multi-echo] poll error phase="
                          << static_cast<unsigned int> (phase) << " errno=" << zlink_errno ()
                          << std::endl;
            }
            if (zlink_errno () == EINTR)
                continue;
            fatal_error = true;
            break;
        }
        if (poll_rc == 0)
            continue;
        if (!service_events (poll_rc, true))
            fatal_error = true;
    }

    // Stop timestamping new records at the phase boundary, then pull every
    // completion and echo already owned by Core. This makes the next latency
    // phase start with empty physical and application-side backlogs.
    const auto drain_deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (send_completion_drain_timeout_ms ());
    while (!fatal_error
           && (tracker_has_pending_completions (tracker)
               || tracker_has_pending_replies (tracker))) {
        const int wait_ms = std::min (50, remaining_poll_timeout_ms (drain_deadline));
        if (wait_ms <= 0)
            break;
        const int poll_rc = zlink_poller_wait (
          tracker.poller, tracker.events.empty () ? NULL : &tracker.events[0],
          static_cast<int> (tracker.events.size ()), wait_ms, NULL);
        if (poll_rc < 0) {
            if (zlink_errno () == EINTR)
                continue;
            fatal_error = true;
            break;
        }
        if (poll_rc > 0 && !service_events (poll_rc, collect_latency)) {
            fatal_error = true;
            break;
        }
    }

    if (!fatal_error
        && (tracker_has_pending_completions (tracker)
            || tracker_has_pending_replies (tracker))) {
        if (bench_debug_enabled ()) {
            std::cerr << "[perf-multi-echo] drain timeout phase="
                      << static_cast<unsigned int> (phase) << " size=" << expected_msg_size
                      << std::endl;
        }
        errno = ETIMEDOUT;
        fatal_error = true;
    }

    if (recv_total)
        *recv_total = local_recv;
    if (lat_sum)
        *lat_sum = lat_sum_local;
    if (lat_count)
        *lat_count = lat_count_local;
    if (latency_stats) {
        if (!collect_latency)
            *latency_stats = bench_latency_stats_t ();
        else
            normalize_latency_stats (lat_sum_local, lat_count_local, &lat_samples, latency_stats);
    }
    if (latency_samples_out) {
        latency_samples_out->clear ();
        if (collect_latency)
            lat_samples.append_samples (latency_samples_out);
    }

    return !fatal_error;
}

inline bool run_echo_duration (const std::vector<void *> &sockets,
                               const multi_bench_settings_t &settings,
                               std::vector<char> &payload,
                               size_t payload_size,
                               size_t msg_size,
                               size_t scratch_capacity,
                               const std::string &server_id,
                               bool client_router_send,
                               bool per_socket_payload,
                               uint32_t run_id,
                               double *throughput_out,
                               bench_latency_stats_t *latency_out)
{
    if (!throughput_out || !latency_out)
        return false;

    *throughput_out = 0.0;
    *latency_out = bench_latency_stats_t ();
    if (sockets.empty ())
        return false;

    long recv_count = 0;
    double lat_sum = 0.0;
    long lat_count = 0;
    bench_latency_stats_t ignored_latency;

    if (!run_echo_window_round_robin (
          sockets, settings, payload, payload_size, msg_size, server_id, client_router_send, run_id,
          perf_multi_metric::phase_active, scratch_capacity,
          static_cast<double> (std::max (1, settings.duration_seconds)), true, false,
          per_socket_payload, &recv_count, &lat_sum, &lat_count, &ignored_latency)) {
        return false;
    }

    if (recv_count <= 0) {
        if (bench_debug_enabled ()) {
            std::cerr << "[perf-multi-echo] active metrics invalid recv=" << recv_count
                      << " lat_count=" << lat_count << " run_id=" << run_id
                      << " msg_size=" << msg_size << std::endl;
        }
        return false;
    }

    *throughput_out = static_cast<double> (recv_count)
                      / static_cast<double> (std::max (1, settings.duration_seconds));

    long latency_recv_count = 0;
    lat_sum = 0.0;
    lat_count = 0;
    if (!run_echo_window_round_robin (
          sockets, settings, payload, payload_size, msg_size, server_id, client_router_send, run_id,
          perf_multi_metric::phase_latency, scratch_capacity,
          static_cast<double> (latency_phase_duration_seconds ()), true, true,
          per_socket_payload, &latency_recv_count, &lat_sum, &lat_count, latency_out, NULL,
          std::numeric_limits<size_t>::max (), latency_phase_max_in_flight_per_socket ())) {
        return false;
    }
    if (latency_recv_count <= 0 || lat_count <= 0 || latency_out->mean_ns <= 0.0) {
        if (bench_debug_enabled ()) {
            std::cerr << "[perf-multi-echo] latency metrics invalid recv="
                      << latency_recv_count << " lat_count=" << lat_count << " run_id=" << run_id
                      << " msg_size=" << msg_size << std::endl;
        }
        return false;
    }

    return true;
}

inline void print_client_result_lines (const char *pattern,
                                       const std::string &lib_name,
                                       const std::string &transport,
                                       size_t msg_size,
                                       double throughput,
                                       const bench_latency_stats_t &latency)
{
    print_result (lib_name, pattern, transport, msg_size, throughput, latency.mean_ns,
                  latency.p95_ns, latency.p99_ns);
}

inline void print_echo_client_result_lines (const char *pattern,
                                            const std::string &lib_name,
                                            const std::string &transport,
                                            size_t msg_size,
                                            double throughput,
                                            const bench_latency_stats_t &latency)
{
    print_client_result_lines (pattern, lib_name, transport, msg_size, throughput, latency);
}

inline std::vector<size_t> resolve_case_msg_sizes (size_t fallback_size)
{
    std::vector<size_t> msg_sizes = resolve_bench_msg_sizes (fallback_size);
    if (msg_sizes.empty ())
        msg_sizes.push_back (fallback_size > 0 ? fallback_size : 64);
    return msg_sizes;
}

inline size_t resolve_case_max_msg_size (size_t fallback_size, const std::vector<size_t> &msg_sizes)
{
    size_t max_msg_size = fallback_size > 0 ? fallback_size : 64;
    for (size_t i = 0; i < msg_sizes.size (); ++i) {
        if (msg_sizes[i] > max_msg_size)
            max_msg_size = msg_sizes[i];
    }
    return max_msg_size;
}

} // namespace perf_multi_client

#endif
