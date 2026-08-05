/* SPDX-License-Identifier: MPL-2.0 */

#ifndef TEST_MULTI_STREAM_SESSION_SUPPORT_HPP
#define TEST_MULTI_STREAM_SESSION_SUPPORT_HPP

#include <zlink.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>

namespace test_stream_common
{

inline void store_u32_be (unsigned char *p, uint32_t v)
{
    p[0] = static_cast<unsigned char> ((v >> 24) & 0xFF);
    p[1] = static_cast<unsigned char> ((v >> 16) & 0xFF);
    p[2] = static_cast<unsigned char> ((v >> 8) & 0xFF);
    p[3] = static_cast<unsigned char> (v & 0xFF);
}

} // namespace test_stream_common

namespace test_multi_stream
{

inline std::atomic<bool> &stop_requested ()
{
    static std::atomic<bool> flag (false);
    return flag;
}

inline bool debug_enabled ()
{
    static const bool enabled = std::getenv ("PERF_DEBUG") != NULL;
    return enabled;
}

inline int aux_poll_wait_ms ()
{
    return 10;
}

inline int socket_poll (zlink_pollitem_t *items_, int count_, long timeout_ms_)
{
    return zlink_poll (items_, count_, timeout_ms_, NULL);
}

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

    bool assign (const zlink_routing_id_t *rid_, zlink_msg_t *msg_part_)
    {
        if (!rid_ || !msg_part_)
            return false;
        routing_id = *rid_;
        (void) zlink_msg_close (&msg);
        if (zlink_msg_init (&msg) != 0)
            return false;
        return zlink_msg_move (&msg, msg_part_) == 0;
    }

    zlink_routing_id_t routing_id;
    zlink_msg_t msg;

  private:
    queued_message_t (const queued_message_t &);
    queued_message_t &operator= (const queued_message_t &);
};

enum send_result_t
{
    send_result_sent = 0,
    send_result_pending = 1,
    send_result_failed = 2
};

struct session_t
{
    session_t () :
        send_socket (NULL), recv_count (0), send_count (0), pending_count (0), pending_queue ()
    {
    }

    void *send_socket;
    std::atomic<unsigned long long> recv_count;
    std::atomic<unsigned long long> send_count;
    std::atomic<unsigned long long> pending_count;
    std::mutex pending_mutex;
    std::deque<queued_message_t> pending_queue;
};

inline bool is_stop_payload (const unsigned char *data_, size_t size_, const char *stop_token_)
{
    return data_ && stop_token_ && *stop_token_ && size_ == std::strlen (stop_token_)
           && std::memcmp (data_, stop_token_, size_) == 0;
}

inline void reset_session (session_t *session_, void *send_socket_)
{
    if (!session_)
        return;
    session_->send_socket = send_socket_;
    session_->recv_count.store (0, std::memory_order_release);
    session_->send_count.store (0, std::memory_order_release);
    session_->pending_count.store (0, std::memory_order_release);
    std::lock_guard<std::mutex> lock (session_->pending_mutex);
    session_->pending_queue.clear ();
}

inline void clear_session (session_t *session_)
{
    if (!session_)
        return;
    session_->send_socket = NULL;
    std::lock_guard<std::mutex> lock (session_->pending_mutex);
    session_->pending_queue.clear ();
}

inline size_t pending_size (session_t *session_)
{
    if (!session_)
        return 0;
    std::lock_guard<std::mutex> lock (session_->pending_mutex);
    return session_->pending_queue.size ();
}

inline send_result_t try_send (queued_message_t &queued_, void *send_socket_)
{
    if (!send_socket_)
        return send_result_failed;

    const int rc = zlink_send_part_rid (send_socket_, &queued_.routing_id, &queued_.msg,
                                        ZLINK_DONTWAIT, ZLINK_PART_FINAL);
    if (rc == 0)
        return send_result_sent;

    const int err = zlink_errno ();
    if (err == EAGAIN)
        return send_result_pending;
    if (debug_enabled ()) {
        std::cerr << "[multi-stream-server] send failed err=" << err << std::endl;
    }
    return send_result_failed;
}

inline bool enqueue (session_t *session_, const zlink_routing_id_t *rid_, zlink_msg_t *msg_part_)
{
    if (!session_ || !rid_ || !msg_part_)
        return false;

    queued_message_t queued;
    if (!queued.assign (rid_, msg_part_))
        return false;

    session_->pending_count.fetch_add (1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock (session_->pending_mutex);
    session_->pending_queue.push_back (std::move (queued));
    return true;
}

inline bool build_packet_frame (zlink_msg_t *packet_out_,
                                const zlink_msg_t *header_part_,
                                const zlink_msg_t *body_part_)
{
    if (!packet_out_ || !header_part_ || !body_part_)
        return false;

    const size_t header_size = zlink_msg_size (const_cast<zlink_msg_t *> (header_part_));
    const size_t body_size = zlink_msg_size (const_cast<zlink_msg_t *> (body_part_));
    const size_t total_size = 6 + header_size + body_size;
    if (zlink_msg_init_size (packet_out_, total_size) != 0)
        return false;

    unsigned char *dst = static_cast<unsigned char *> (zlink_msg_data (packet_out_));
    dst[0] = static_cast<unsigned char> ((header_size >> 8) & 0xFF);
    dst[1] = static_cast<unsigned char> (header_size & 0xFF);
    test_stream_common::store_u32_be (dst + 2, static_cast<uint32_t> (body_size));
    if (header_size > 0) {
        std::memcpy (dst + 6, zlink_msg_data (const_cast<zlink_msg_t *> (header_part_)),
                     header_size);
    }
    if (body_size > 0) {
        std::memcpy (dst + 6 + header_size, zlink_msg_data (const_cast<zlink_msg_t *> (body_part_)),
                     body_size);
    }
    return true;
}

inline send_result_t
try_send_packet_now (void *stream_socket_, const zlink_routing_id_t *rid_, zlink_msg_t *packet_)
{
    if (!stream_socket_ || !rid_ || !packet_)
        return send_result_failed;

    const int rc =
      zlink_send_part_rid (stream_socket_, rid_, packet_, ZLINK_DONTWAIT, ZLINK_PART_FINAL);
    if (rc == 0)
        return send_result_sent;

    const int err = zlink_errno ();
    if (err == EAGAIN)
        return send_result_pending;
    if (debug_enabled ()) {
        std::cerr << "[multi-stream-server] try_send_packet_now failed err=" << err
                  << " rid_size=" << static_cast<unsigned> (rid_->size)
                  << " packet_size=" << zlink_msg_size (packet_) << " rid_hex=";
        for (unsigned int i = 0; i < static_cast<unsigned int> (rid_->size); ++i) {
            std::cerr << std::hex << std::setw (2) << std::setfill ('0')
                      << static_cast<unsigned> (rid_->data[i]);
        }
        std::cerr << std::dec << std::setfill (' ') << std::endl;
    }
    return send_result_failed;
}

inline bool handle_packet_message (session_t *session_,
                                   void *stream_socket_,
                                   const zlink_routing_id_t *rid_,
                                   zlink_msg_t *header_part_,
                                   zlink_msg_t *body_part_,
                                   const char *stop_token_)
{
    if (!session_ || !rid_ || !header_part_ || !body_part_ || !session_->send_socket) {
        return false;
    }

    const unsigned char *body_payload =
      static_cast<const unsigned char *> (zlink_msg_data (body_part_));
    const size_t body_payload_size = zlink_msg_size (body_part_);
    if (is_stop_payload (body_payload, body_payload_size, stop_token_)) {
        stop_requested ().store (true, std::memory_order_release);
        return true;
    }

    session_->recv_count.fetch_add (1, std::memory_order_relaxed);
    zlink_msg_t packet;
    if (!build_packet_frame (&packet, header_part_, body_part_))
        return false;

    const send_result_t send_rc = try_send_packet_now (stream_socket_, rid_, &packet);
    if (send_rc == send_result_sent) {
        session_->send_count.fetch_add (1, std::memory_order_relaxed);
        (void) zlink_msg_close (&packet);
        return true;
    }
    if (send_rc != send_result_pending) {
        if (debug_enabled ()) {
            std::cerr << "[multi-stream-server] immediate send failed err=" << zlink_errno ()
                      << std::endl;
        }
        (void) zlink_msg_close (&packet);
        return false;
    }

    const bool queued = enqueue (session_, rid_, &packet);
    (void) zlink_msg_close (&packet);
    return queued;
}

inline void drain_pending (session_t *session_)
{
    if (!session_)
        return;

    while (true) {
        queued_message_t queued;
        {
            std::lock_guard<std::mutex> lock (session_->pending_mutex);
            if (session_->pending_queue.empty ())
                return;
            queued = std::move (session_->pending_queue.front ());
            session_->pending_queue.pop_front ();
        }

        const send_result_t rc = try_send (queued, session_->send_socket);
        if (rc == send_result_sent) {
            session_->send_count.fetch_add (1, std::memory_order_relaxed);
            const unsigned long long pending_before =
              session_->pending_count.load (std::memory_order_relaxed);
            if (pending_before > 0) {
                session_->pending_count.fetch_sub (1, std::memory_order_relaxed);
            }
            continue;
        }
        if (rc == send_result_pending) {
            std::lock_guard<std::mutex> lock (session_->pending_mutex);
            session_->pending_queue.push_front (std::move (queued));
            return;
        }
        stop_requested ().store (true, std::memory_order_release);
        return;
    }
}

struct packet_handler_context_t
{
    packet_handler_context_t () : session (NULL), stop_token (NULL) {}

    session_t *session;
    const char *stop_token;
};

inline void stream_packet_handler_callback (void *stream_,
                                            const zlink_routing_id_t *rid_,
                                            zlink_msg_t *header_part_,
                                            zlink_msg_t *body_part_,
                                            void *userdata_)
{
    packet_handler_context_t *ctx = static_cast<packet_handler_context_t *> (userdata_);
    if (!stream_ || !ctx || !ctx->session || !rid_ || !header_part_ || !body_part_) {
        if (header_part_)
            (void) zlink_msg_close (header_part_);
        if (body_part_)
            (void) zlink_msg_close (body_part_);
        return;
    }

    if (!handle_packet_message (ctx->session, stream_, rid_, header_part_, body_part_,
                                ctx->stop_token)) {
        stop_requested ().store (true, std::memory_order_release);
    }

    (void) zlink_msg_close (header_part_);
    (void) zlink_msg_close (body_part_);
}

typedef void (*loop_tick_fn_t) (void *);

inline int run_server_event_loop (session_t *session_,
                                  void *server_socket_,
                                  const char *stop_token_,
                                  loop_tick_fn_t loop_tick_,
                                  void *loop_tick_ctx_)
{
    if (!session_ || !server_socket_ || !stop_token_ || !*stop_token_) {
        errno = EINVAL;
        return 1;
    }

    int rc = 0;
    while (!stop_requested ().load (std::memory_order_acquire) && rc == 0) {
        if (loop_tick_)
            loop_tick_ (loop_tick_ctx_);

        if (pending_size (session_) > 0)
            drain_pending (session_);

        if (pending_size (session_) == 0) {
            const int idle_rc = socket_poll (NULL, 0, aux_poll_wait_ms ());
            if (idle_rc < 0 && zlink_errno () != EINTR && zlink_errno () != EAGAIN) {
                if (debug_enabled ()) {
                    std::cerr << "[multi-stream-server] idle poll failed err=" << zlink_errno ()
                              << std::endl;
                }
                rc = 1;
                break;
            }
            continue;
        }

        zlink_pollitem_t item;
        std::memset (&item, 0, sizeof (item));
        item.socket = server_socket_;
        item.fd = 0;
        item.events = ZLINK_POLLOUT;
        item.revents = 0;

        const int poll_rc = socket_poll (&item, 1, aux_poll_wait_ms ());
        if (poll_rc < 0) {
            if (zlink_errno () == EINTR || zlink_errno () == EAGAIN)
                continue;
            if (debug_enabled ()) {
                std::cerr << "[multi-stream-server] poll failed err=" << zlink_errno ()
                          << std::endl;
            }
            rc = 1;
            break;
        }

        if ((item.revents & ZLINK_POLLOUT) != 0)
            drain_pending (session_);
    }

    return rc;
}

} // namespace test_multi_stream

#endif
