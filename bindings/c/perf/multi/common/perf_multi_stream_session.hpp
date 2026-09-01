#ifndef PERF_MULTI_STREAM_SESSION_HPP
#define PERF_MULTI_STREAM_SESSION_HPP

#include "perf_common.hpp"
#include "../../common/streamclient/perf_stream_common.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <iomanip>
#include <mutex>
#include <string>
#include <unordered_map>

namespace perf_multi_stream
{

struct queued_message_t
{
    queued_message_t ()
    {
        std::memset (&routing_id, 0, sizeof (routing_id));
        if (zlink_msg_init (&msg) != 0)
            std::abort ();
    }

    ~queued_message_t () { (void) zlink_msg_close (&msg); }

    queued_message_t (queued_message_t &&other) noexcept
    {
        std::memset (&routing_id, 0, sizeof (routing_id));
        if (zlink_msg_init (&msg) != 0)
            std::abort ();
        routing_id = other.routing_id;
        if (zlink_msg_move (&msg, &other.msg) != 0)
            std::abort ();
    }

    queued_message_t &operator= (queued_message_t &&other) noexcept
    {
        if (this == &other)
            return *this;
        routing_id = other.routing_id;
        (void) zlink_msg_close (&msg);
        if (zlink_msg_init (&msg) != 0)
            std::abort ();
        if (zlink_msg_move (&msg, &other.msg) != 0)
            std::abort ();
        return *this;
    }

    bool assign (const zlink_routing_id_t *rid, zlink_msg_t *msg_part)
    {
        if (!rid || !msg_part)
            return false;
        routing_id = *rid;
        (void) zlink_msg_close (&msg);
        if (zlink_msg_init (&msg) != 0)
            return false;
        return zlink_msg_move (&msg, msg_part) == 0;
    }

    zlink_routing_id_t routing_id;
    zlink_msg_t msg;

  private:
    queued_message_t (const queued_message_t &);
    queued_message_t &operator= (const queued_message_t &);
};

typedef std::deque<queued_message_t> pending_route_messages_t;
typedef uint32_t pending_route_key_t;
typedef std::unordered_map<pending_route_key_t, pending_route_messages_t> pending_route_map_t;

enum send_result_t
{
    send_result_sent = 0,
    send_result_pending = 1,
    send_result_failed = 2
};

struct session_t
{
    session_t () :
        ctx (NULL),
        send_socket (NULL),
        recv_count (0),
        send_count (0),
        pending_count (0),
        transport (),
        pending_by_route (),
        pending_routes (),
        pending_cv ()
    {
    }

    void *ctx;
    void *send_socket;
    std::atomic<unsigned long long> recv_count;
    std::atomic<unsigned long long> send_count;
    std::atomic<unsigned long long> pending_count;
    std::string transport;
    std::mutex pending_mutex;
    pending_route_map_t pending_by_route;
    std::deque<pending_route_key_t> pending_routes;
    std::condition_variable pending_cv;
};

inline pending_route_key_t pending_route_key (const zlink_routing_id_t &rid)
{
    return perf_stream_common::perf_stream_load_u32_be (rid.data);
}

inline bool is_stop_payload (const unsigned char *data, size_t size, const char *stop_token)
{
    return data && stop_token && *stop_token && size == std::strlen (stop_token)
           && std::memcmp (data, stop_token, size) == 0;
}

inline void reset_session (session_t *session,
                           void *ctx,
                           void *send_socket,
                           const std::string &transport)
{
    if (!session)
        return;
    session->ctx = ctx;
    session->send_socket = send_socket;
    session->recv_count.store (0, std::memory_order_release);
    session->send_count.store (0, std::memory_order_release);
    session->pending_count.store (0, std::memory_order_release);
    session->transport = transport;
    std::lock_guard<std::mutex> lock (session->pending_mutex);
    session->pending_by_route.clear ();
    session->pending_routes.clear ();
    session->pending_cv.notify_all ();
}

inline void clear_session (session_t *session)
{
    if (!session)
        return;
    session->send_socket = NULL;
    std::lock_guard<std::mutex> lock (session->pending_mutex);
    session->pending_by_route.clear ();
    session->pending_routes.clear ();
    session->pending_cv.notify_all ();
}

inline size_t pending_size (session_t *session)
{
    if (!session)
        return 0;
    return static_cast<size_t> (session->pending_count.load (std::memory_order_relaxed));
}

inline send_result_t try_send (session_t *session, queued_message_t &queued)
{
    if (!session)
        return send_result_failed;
    void *send_socket = session->send_socket;
    if (!send_socket)
        return send_result_failed;

    // run_server_event_loop() is the sole pending-queue consumer. Direct
    // callback replies use the pipe-safe fast path in try_send_packet_now().
    const int rc =
      perf_zlink_send_rid_parts (send_socket, &queued.routing_id, &queued.msg, 1, ZLINK_DONTWAIT);
    if (rc == 0)
        return send_result_sent;

    const int err = zlink_errno ();
    if (err == EAGAIN)
        return send_result_pending;
    if (bench_debug_enabled ()) {
        std::cerr << "[multi-stream-server] send failed err=" << err << std::endl;
    }
    return send_result_failed;
}

inline bool enqueue (session_t *session, const zlink_routing_id_t *rid, zlink_msg_t *msg_part)
{
    if (!session || !rid || rid->size != sizeof (uint32_t) || !msg_part)
        return false;

    const pending_route_key_t route_key = pending_route_key (*rid);
    queued_message_t queued;
    if (!queued.assign (rid, msg_part))
        return false;

    {
        std::lock_guard<std::mutex> lock (session->pending_mutex);
        pending_route_map_t::iterator route = session->pending_by_route.find (route_key);
        if (route == session->pending_by_route.end ()) {
            route = session->pending_by_route
                      .insert (std::make_pair (route_key, pending_route_messages_t ()))
                      .first;
            session->pending_routes.push_back (route_key);
        }
        route->second.push_back (std::move (queued));
        session->pending_count.fetch_add (1, std::memory_order_relaxed);
    }
    session->pending_cv.notify_one ();
    return true;
}

inline bool
enqueue_packet_message (session_t *session, const zlink_routing_id_t *rid, zlink_msg_t *packet)
{
    return enqueue (session, rid, packet);
}

inline bool build_packet_frame (zlink_msg_t *packet_out,
                                const zlink_msg_t *header_part,
                                const zlink_msg_t *body_part)
{
    if (!packet_out || !header_part || !body_part)
        return false;

    const size_t header_size = zlink_msg_size (const_cast<zlink_msg_t *> (header_part));
    const size_t body_size = zlink_msg_size (const_cast<zlink_msg_t *> (body_part));
    const size_t total_size = 6 + header_size + body_size;
    if (zlink_msg_init_size (packet_out, total_size) != 0)
        return false;

    unsigned char *dst = static_cast<unsigned char *> (zlink_msg_data (packet_out));
    dst[0] = static_cast<unsigned char> ((header_size >> 8) & 0xFF);
    dst[1] = static_cast<unsigned char> (header_size & 0xFF);
    perf_stream_common::perf_stream_store_u32_be (dst + 2, static_cast<uint32_t> (body_size));
    if (header_size > 0)
        std::memcpy (dst + 6, zlink_msg_data (const_cast<zlink_msg_t *> (header_part)),
                     header_size);
    if (body_size > 0)
        std::memcpy (dst + 6 + header_size, zlink_msg_data (const_cast<zlink_msg_t *> (body_part)),
                     body_size);
    return true;
}

inline send_result_t try_send_packet_now (void *stream_socket,
                                          session_t *session,
                                          const zlink_routing_id_t *rid,
                                          zlink_msg_t *packet)
{
    if (!stream_socket || !session || !rid || !packet)
        return send_result_failed;

    // Hot path: this is called from the STREAM packet callback. When the
    // routing id matches the current callback, the C API routes directly to the
    // owning pipe and uses pipe-level synchronization; do not reintroduce a
    // process-wide send mutex here.
    const int rc = perf_zlink_send_rid_parts (stream_socket, rid, packet, 1, ZLINK_DONTWAIT);
    if (rc == 0)
        return send_result_sent;

    const int err = zlink_errno ();
    if (err == EAGAIN)
        return send_result_pending;
    if (bench_debug_enabled ()) {
        std::cerr << "[multi-stream-server] try_send_packet_now failed err=" << err
                  << " rid_size=" << static_cast<unsigned> (rid->size)
                  << " packet_size=" << zlink_msg_size (packet) << " rid_hex=";
        for (unsigned int i = 0; i < static_cast<unsigned int> (rid->size); ++i) {
            std::cerr << std::hex << std::setw (2) << std::setfill ('0')
                      << static_cast<unsigned> (rid->data[i]);
        }
        std::cerr << std::dec << std::setfill (' ') << std::endl;
    }
    return send_result_failed;
}

inline bool handle_packet_message (session_t *session,
                                   void *stream_socket,
                                   const zlink_routing_id_t *rid,
                                   zlink_msg_t *header_part,
                                   zlink_msg_t *body_part,
                                   const char *stop_token)
{
    if (!session || !rid || !header_part || !body_part || !session->send_socket)
        return false;

    const unsigned char *body_payload =
      static_cast<const unsigned char *> (zlink_msg_data (body_part));
    const size_t body_payload_size = zlink_msg_size (body_part);
    if (is_stop_payload (body_payload, body_payload_size, stop_token)) {
        perf_stop_requested ().store (true, std::memory_order_release);
        return true;
    }

    session->recv_count.fetch_add (1, std::memory_order_relaxed);
    zlink_msg_t packet;
    if (!build_packet_frame (&packet, header_part, body_part))
        return false;

    const send_result_t send_rc = try_send_packet_now (stream_socket, session, rid, &packet);
    if (send_rc == send_result_sent) {
        session->send_count.fetch_add (1, std::memory_order_relaxed);
        (void) zlink_msg_close (&packet);
        return true;
    }
    if (send_rc != send_result_pending) {
        if (bench_debug_enabled ()) {
            std::cerr << "[multi-stream-server] immediate send failed err=" << zlink_errno ()
                      << std::endl;
        }
        (void) zlink_msg_close (&packet);
        return false;
    }

    const bool queued = enqueue_packet_message (session, rid, &packet);
    (void) zlink_msg_close (&packet);
    return queued;
}

template <typename SendAttempt>
inline void drain_pending_with (session_t *session, SendAttempt send_attempt)
{
    if (!session)
        return;

    while (true) {
        size_t route_budget = 0;
        {
            std::lock_guard<std::mutex> lock (session->pending_mutex);
            route_budget = session->pending_routes.size ();
            if (route_budget == 0)
                return;
        }

        bool route_blocked = false;
        for (size_t route_index = 0; route_index < route_budget; ++route_index) {
            queued_message_t queued;
            pending_route_key_t route_key = 0;
            {
                std::lock_guard<std::mutex> lock (session->pending_mutex);
                if (session->pending_routes.empty ())
                    break;

                route_key = session->pending_routes.front ();
                session->pending_routes.pop_front ();
                pending_route_map_t::iterator route =
                  session->pending_by_route.find (route_key);
                if (route == session->pending_by_route.end () || route->second.empty ())
                    continue;

                queued = std::move (route->second.front ());
                route->second.pop_front ();
            }

            const send_result_t rc = send_attempt (session, queued);
            {
                std::lock_guard<std::mutex> lock (session->pending_mutex);
                pending_route_map_t::iterator route =
                  session->pending_by_route.find (route_key);
                if (route == session->pending_by_route.end ())
                    continue;

                if (rc == send_result_pending)
                    route->second.push_front (std::move (queued));

                if (!route->second.empty ())
                    session->pending_routes.push_back (route_key);
                else
                    session->pending_by_route.erase (route);
            }

            if (rc == send_result_sent) {
                session->send_count.fetch_add (1, std::memory_order_relaxed);
                const unsigned long long pending_before =
                  session->pending_count.load (std::memory_order_relaxed);
                if (pending_before > 0)
                    session->pending_count.fetch_sub (1, std::memory_order_relaxed);
                continue;
            }
            if (rc == send_result_pending) {
                route_blocked = true;
                continue;
            }
            perf_stop_requested ().store (true, std::memory_order_release);
            return;
        }

        // One pass gives every active routing id one send opportunity. If a
        // pipe remains blocked, wait for the next POLLOUT edge instead of
        // retrying that pipe while other callback threads are still active.
        if (route_blocked)
            return;
    }
}

inline void drain_pending (session_t *session)
{
    drain_pending_with (session, [] (session_t *current, queued_message_t &queued) {
        return try_send (current, queued);
    });
}

struct packet_handler_context_t
{
    packet_handler_context_t () : session (NULL), stop_token (NULL) {}

    session_t *session;
    const char *stop_token;
};

inline void stream_packet_handler_callback (void *stream_,
                                            const zlink_routing_id_t *rid,
                                            zlink_msg_t *header_part,
                                            zlink_msg_t *body_part,
                                            void *userdata)
{
    packet_handler_context_t *ctx = static_cast<packet_handler_context_t *> (userdata);
    if (!stream_ || !ctx || !ctx->session || !rid || !header_part || !body_part) {
        if (header_part)
            (void) zlink_msg_close (header_part);
        if (body_part)
            (void) zlink_msg_close (body_part);
        return;
    }

    if (!handle_packet_message (ctx->session, stream_, rid, header_part, body_part,
                                ctx->stop_token)) {
        perf_stop_requested ().store (true, std::memory_order_release);
        ctx->session->pending_cv.notify_all ();
    }

    (void) zlink_msg_close (header_part);
    (void) zlink_msg_close (body_part);
}

typedef void (*loop_tick_fn_t) (void *);

inline int run_server_event_loop (session_t *session,
                                  void *server_socket,
                                  const char *stop_token,
                                  loop_tick_fn_t loop_tick,
                                  void *loop_tick_ctx)
{
    if (!session || !server_socket || !stop_token || !*stop_token) {
        errno = EINVAL;
        return 1;
    }

    int rc = 0;
    while (!perf_stop_requested ().load (std::memory_order_acquire) && rc == 0) {
        if (loop_tick)
            loop_tick (loop_tick_ctx);

        if (pending_size (session) > 0)
            drain_pending (session);

        if (pending_size (session) == 0) {
            std::unique_lock<std::mutex> lock (session->pending_mutex);
            session->pending_cv.wait_for (
              lock, std::chrono::milliseconds (perf_aux_poll_wait_ms ()), [&] {
                  return !session->pending_routes.empty ()
                         || perf_stop_requested ().load (std::memory_order_acquire);
              });
            continue;
        }

        zlink_pollitem_t item;
        std::memset (&item, 0, sizeof (item));
        item.socket = server_socket;
        item.fd = 0;
        item.events = ZLINK_POLLOUT;
        item.revents = 0;

        const int poll_rc = perf_socket_poll (&item, 1, perf_aux_poll_wait_ms ());
        if (poll_rc < 0) {
            if (zlink_errno () == EINTR || zlink_errno () == EAGAIN)
                continue;
            if (bench_debug_enabled ()) {
                std::cerr << "[multi-stream-server] poll failed err=" << zlink_errno ()
                          << std::endl;
            }
            rc = 1;
            break;
        }

        if ((item.revents & ZLINK_POLLOUT) != 0)
            drain_pending (session);
    }

    return rc;
}

} // namespace perf_multi_stream

#endif
