#ifndef PERF_MULTI_STREAM_SESSION_HPP
#define PERF_MULTI_STREAM_SESSION_HPP

#include "perf_common.hpp"
#include "../../common/streamclient/perf_stream_common.hpp"

#include <atomic>
#include <chrono>
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
    // Counts DONTWAIT sends with a nonzero completion ID until their terminal
    // records are pulled from the socket completion queue.
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
    // zlink_close() must have completed before this pointer is cleared.
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

inline void finish_pending_submission (session_t *session)
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
}

inline bool record_send_completion (session_t *session,
                                    const zlink_completion_t *completion)
{
    if (!session || !completion || completion->kind != ZLINK_COMPLETION_SEND
        || completion->completion_id == 0 || completion->user_context != session)
        return false;

    if (completion->send_result == ZLINK_SEND_ADMITTED)
        session->send_count.fetch_add (1, std::memory_order_relaxed);
    else
        record_failure (session, completion->send_terminal_errno);
    finish_pending_submission (session);
    return true;
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

    zlink_completion_id_t completion_id = 0;
    const zlink_submit_result_t result = zlink_send_part_rid (
      session->send_socket, rid, packet, ZLINK_SEND_FLAGS_DONTWAIT,
      ZLINK_PART_FINAL, session, &completion_id);
    const int submit_errno = zlink_errno ();
    if (result != ZLINK_SUBMIT_OK) {
        errno = submit_errno;
        return false;
    }

    if (completion_id == 0)
        record_immediate_admission (session);
    else
        session->outstanding_count.fetch_add (1, std::memory_order_acq_rel);
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

inline bool drain_send_completions (session_t *session)
{
    if (!session || !session->send_socket)
        return false;
    for (;;) {
        zlink_completion_t completion;
        std::memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t rc = zlink_completion_recv (
          session->send_socket, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA)
            return true;
        if (rc != ZLINK_RECV_OK)
            return false;
        const bool valid = record_send_completion (session, &completion);
        zlink_completion_close (&completion);
        if (!valid)
            return false;
    }
}

inline bool drain_packets (session_t *session, const char *stop_token)
{
    if (!session || !session->send_socket)
        return false;
    for (;;) {
        const zlink_routing_id_t *rid = NULL;
        zlink_msg_t header_part;
        zlink_msg_t body_part;
        if (zlink_msg_init (&header_part) != 0)
            return false;
        if (zlink_msg_init (&body_part) != 0) {
            zlink_msg_close (&header_part);
            return false;
        }
        const zlink_recv_result_t rc = zlink_stream_recv_packet (
          session->send_socket, &rid, &header_part, &body_part,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            zlink_msg_close (&header_part);
            zlink_msg_close (&body_part);
            return true;
        }
        if (rc != ZLINK_RECV_OK || !rid || rid->size == 0) {
            zlink_msg_close (&header_part);
            zlink_msg_close (&body_part);
            return false;
        }
        const bool handled = handle_packet_message (
          session, rid, &header_part, &body_part, stop_token);
        zlink_msg_close (&header_part);
        zlink_msg_close (&body_part);
        if (!handled)
            return false;
        if (perf_stop_requested ().load (std::memory_order_acquire))
            return true;
    }
}

inline int run_server_event_loop (session_t *session, const char *stop_token)
{
    if (!session || !session->send_socket) {
        errno = EINVAL;
        return 1;
    }

    void *poller = zlink_poller_new ();
    if (!poller
        || zlink_poller_add (poller, session->send_socket, session,
                             static_cast<short> (ZLINK_POLLIN
                                                 | ZLINK_POLLCOMPLETION))
             != ZLINK_CONFIG_OK) {
        if (poller)
            zlink_poller_destroy (&poller);
        return 1;
    }

    std::chrono::steady_clock::time_point drain_deadline =
      std::chrono::steady_clock::time_point::max ();
    while (!session->failed.load (std::memory_order_acquire)) {
        const bool stopping = perf_stop_requested ().load (std::memory_order_acquire);
        if (stopping && drain_deadline == std::chrono::steady_clock::time_point::max ())
            drain_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
        if (stopping && outstanding_size (session) == 0)
            break;
        if (stopping && std::chrono::steady_clock::now () >= drain_deadline) {
            record_failure (session, ETIMEDOUT);
            break;
        }

        zlink_poller_event_t event;
        std::memset (&event, 0, sizeof (event));
        const int poll_rc = zlink_poller_wait (
          poller, &event, 1, perf_aux_poll_wait_ms (), NULL);
        if (poll_rc < 0) {
            if (zlink_errno () == EINTR)
                continue;
            record_failure (session, zlink_errno ());
            break;
        }
        if (poll_rc > 0 && (event.events & ZLINK_POLLCOMPLETION) != 0
            && !drain_send_completions (session)) {
            record_failure (session, zlink_errno ());
            break;
        }
        if (!stopping && poll_rc > 0 && (event.events & ZLINK_POLLIN) != 0
            && !drain_packets (session, stop_token)) {
            record_failure (session, zlink_errno ());
            break;
        }
    }
    const bool poller_closed = zlink_poller_destroy (&poller) == ZLINK_CLOSE_OK;
    return !poller_closed || session->failed.load (std::memory_order_acquire) ? 1 : 0;
}

} // namespace perf_multi_stream

#endif
