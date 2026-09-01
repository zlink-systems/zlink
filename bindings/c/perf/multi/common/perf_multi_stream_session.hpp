#ifndef PERF_MULTI_STREAM_SESSION_HPP
#define PERF_MULTI_STREAM_SESSION_HPP

#include "perf_common.hpp"
#include "../../common/streamclient/perf_stream_common.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace perf_multi_stream
{

struct session_t
{
    session_t () :
        send_socket (NULL),
        recv_count (0),
        send_count (0),
        outstanding_count (0),
        failure_count (0),
        first_failure_errno (0),
        failed (false),
        stop_cv ()
    {
    }

    void *send_socket;
    std::atomic<unsigned long long> recv_count;
    std::atomic<unsigned long long> send_count;
    // Includes a public zlink_send_async call until its immediate result is
    // known and every Core-owned pending operation until its terminal
    // completion callback runs.
    std::atomic<unsigned long long> outstanding_count;
    std::atomic<unsigned long long> failure_count;
    std::atomic<int> first_failure_errno;
    std::atomic<bool> failed;
    std::mutex stop_mutex;
    std::condition_variable stop_cv;
};

inline bool is_stop_payload (const unsigned char *data, size_t size, const char *stop_token)
{
    return data && stop_token && *stop_token && size == std::strlen (stop_token)
           && std::memcmp (data, stop_token, size) == 0;
}

inline void reset_session (session_t *session, void *send_socket)
{
    if (!session)
        return;
    session->send_socket = send_socket;
    session->recv_count.store (0, std::memory_order_release);
    session->send_count.store (0, std::memory_order_release);
    session->outstanding_count.store (0, std::memory_order_release);
    session->failure_count.store (0, std::memory_order_release);
    session->first_failure_errno.store (0, std::memory_order_release);
    session->failed.store (false, std::memory_order_release);
    session->stop_cv.notify_all ();
}

inline void clear_session (session_t *session)
{
    if (!session)
        return;
    // zlink_close() must have completed before this pointer is cleared. It
    // fences both packet callbacks and Core-owned send completions.
    session->send_socket = NULL;
    session->stop_cv.notify_all ();
}

inline size_t outstanding_size (const session_t *session)
{
    if (!session)
        return 0;
    return static_cast<size_t> (
      session->outstanding_count.load (std::memory_order_acquire));
}

inline void request_stop (session_t *session)
{
    perf_stop_requested ().store (true, std::memory_order_release);
    if (session)
        session->stop_cv.notify_all ();
}

inline void record_failure (session_t *session, int error_code)
{
    if (!session)
        return;
    if (error_code == 0)
        error_code = EIO;
    int expected = 0;
    (void) session->first_failure_errno.compare_exchange_strong (
      expected, error_code, std::memory_order_acq_rel, std::memory_order_acquire);
    session->failure_count.fetch_add (1, std::memory_order_relaxed);
    session->failed.store (true, std::memory_order_release);
    request_stop (session);
}

inline void begin_async_submission (session_t *session)
{
    session->outstanding_count.fetch_add (1, std::memory_order_acq_rel);
}

inline void finish_async_submission (session_t *session)
{
    if (!session)
        return;
    const unsigned long long previous =
      session->outstanding_count.fetch_sub (1, std::memory_order_acq_rel);
    if (previous == 0) {
        session->outstanding_count.store (0, std::memory_order_release);
        record_failure (session, EPROTO);
        return;
    }
}

inline void record_immediate_admission (session_t *session)
{
    if (!session)
        return;
    session->send_count.fetch_add (1, std::memory_order_relaxed);
    finish_async_submission (session);
}

inline void stream_send_complete_callback (
  void *, const zlink_send_complete_event_t *event, void *userdata)
{
    session_t *session = static_cast<session_t *> (userdata);
    if (!session || !event)
        return;

    // Completion is the final word for this operation. In particular, this
    // callback never resubmits: Core owns admission wait and exact-route FIFO.
    if (event->result == ZLINK_SEND_ADMITTED)
        session->send_count.fetch_add (1, std::memory_order_relaxed);
    else
        record_failure (session, event->terminal_errno);
    finish_async_submission (session);
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

inline bool submit_packet_async (session_t *session,
                                 const zlink_routing_id_t *rid,
                                 zlink_msg_t *packet)
{
    if (!session || !session->send_socket || !rid || rid->size == 0 || !packet)
        return false;

    // The packet callback's source RID is the complete STREAM routing target.
    // Zero pair fields deliberately request Core's exact current-pair snapshot
    // for that RID; the application does not maintain or poll a route queue.
    zlink_routed_submit_target_t target;
    std::memset (&target, 0, sizeof (target));
    target.peer_rid = *rid;

    zlink_send_async_options_t options;
    std::memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    options.target = &target;

    // A non-zero operation may complete before zlink_send_async returns, so
    // publish the accounting slot first. On SUBMIT_OK, Core owns packet even
    // when admission is pending.
    begin_async_submission (session);
    zlink_send_op_id_t op_id = 0;
    const zlink_submit_result_t result = zlink_send_async (
      session->send_socket, packet, 1, &options, &op_id);
    const int submit_errno = zlink_errno ();
    if (result != ZLINK_SUBMIT_OK) {
        finish_async_submission (session);
        (void) zlink_msg_close (packet);
        errno = submit_errno;
        return false;
    }

    if (op_id == 0)
        record_immediate_admission (session);
    return true;
}

inline bool handle_packet_message (session_t *session,
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
        request_stop (session);
        return true;
    }

    session->recv_count.fetch_add (1, std::memory_order_relaxed);
    zlink_msg_t packet;
    if (!build_packet_frame (&packet, header_part, body_part))
        return false;

    // Core chooses the current-pipe immediate path when it is FIFO-safe and
    // retains a backpressured operation until its terminal completion.
    return submit_packet_async (session, rid, &packet);
}

struct packet_handler_context_t
{
    packet_handler_context_t () : session (NULL), stop_token (NULL) {}

    session_t *session;
    const char *stop_token;
};

inline void stream_packet_handler_callback (void *,
                                            const zlink_routing_id_t *rid,
                                            zlink_msg_t *header_part,
                                            zlink_msg_t *body_part,
                                            void *userdata)
{
    packet_handler_context_t *ctx = static_cast<packet_handler_context_t *> (userdata);
    if (!ctx || !ctx->session || !rid || !header_part || !body_part) {
        if (header_part)
            (void) zlink_msg_close (header_part);
        if (body_part)
            (void) zlink_msg_close (body_part);
        return;
    }

    if (!handle_packet_message (ctx->session, rid, header_part, body_part,
                                ctx->stop_token))
        record_failure (ctx->session, zlink_errno ());

    (void) zlink_msg_close (header_part);
    (void) zlink_msg_close (body_part);
}

inline int run_server_event_loop (session_t *session)
{
    if (!session || !session->send_socket) {
        errno = EINVAL;
        return 1;
    }

    // Backpressured admission progress is Core-owned. This thread only waits
    // for a real STOP/failure signal; there is no POLLOUT probe or timer retry.
    std::unique_lock<std::mutex> lock (session->stop_mutex);
    session->stop_cv.wait (lock, [session] {
        return perf_stop_requested ().load (std::memory_order_acquire)
               || session->failed.load (std::memory_order_acquire);
    });
    return session->failed.load (std::memory_order_acquire) ? 1 : 0;
}

} // namespace perf_multi_stream

#endif
