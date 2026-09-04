#ifndef PERF_MULTI_STREAM_SESSION_HPP
#define PERF_MULTI_STREAM_SESSION_HPP

#include "perf_common.hpp"
#include "../../common/streamclient/perf_stream_common.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace perf_multi_stream
{

struct session_t
{
    session_t () :
        send_socket (NULL),
        recv_count (0),
        send_count (0),
        outstanding_count (0),
        wait_token (0),
        retry_ready (false),
        pollout_suppressed (false),
        retained_rid (),
        retained_packet (),
        failure_count (0),
        first_failure_errno (0),
        failed (false),
        stop_cv ()
    {
    }

    void *send_socket;
    std::atomic<unsigned long long> recv_count;
    std::atomic<unsigned long long> send_count;
    // Counts the one application-owned packet whose backpressure wait token is
    // live until WRITABLE permits its exact-byte resubmission.
    std::atomic<unsigned long long> outstanding_count;
    zlink_completion_id_t wait_token;
    bool retry_ready;
    bool pollout_suppressed;
    zlink_routing_id_t retained_rid;
    std::vector<unsigned char> retained_packet;
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
    session->wait_token = 0;
    session->retry_ready = false;
    session->pollout_suppressed = false;
    std::memset (&session->retained_rid, 0, sizeof (session->retained_rid));
    session->retained_packet.clear ();
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
    session->wait_token = 0;
    session->retry_ready = false;
    session->pollout_suppressed = false;
    std::memset (&session->retained_rid, 0, sizeof (session->retained_rid));
    session->retained_packet.clear ();
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

inline bool stream_routing_ids_equal (const zlink_routing_id_t &left,
                                      const zlink_routing_id_t &right)
{
    return left.size == right.size
           && (left.size == 0
               || std::memcmp (left.data, right.data, left.size) == 0);
}

inline void release_retained_packet (session_t *session)
{
    if (!session)
        return;
    session->wait_token = 0;
    session->retry_ready = false;
    session->pollout_suppressed = false;
    std::memset (&session->retained_rid, 0, sizeof (session->retained_rid));
    session->retained_packet.clear ();
}

inline bool record_writable_completion (session_t *session,
                                        const zlink_completion_t *completion)
{
    if (!session || !completion
        || completion->kind != ZLINK_COMPLETION_WRITABLE
        || completion->completion_id == 0
        || completion->user_context != session->send_socket
        || session->wait_token == 0 || session->retained_packet.empty ()
        || outstanding_size (session) != 1
        || completion->completion_id != session->wait_token
        || !stream_routing_ids_equal (completion->peer_rid,
                                      session->retained_rid)) {
        errno = EPROTO;
        return false;
    }

    session->wait_token = 0;
    session->pollout_suppressed = false;
    if (completion->send_result == ZLINK_SEND_ADMITTED
        && completion->send_terminal_errno == 0) {
        session->retry_ready = true;
        return true;
    }

    const int terminal_errno = completion->send_terminal_errno;
    release_retained_packet (session);
    finish_pending_submission (session);
    record_failure (session, terminal_errno != 0 ? terminal_errno : EIO);
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

// Snapshot the refused framed bytes only after Core reported backpressure:
// the message handle is consumed by every submit, but the received header and
// body parts outlive the attempt, so the admitted hot path copies nothing.
inline bool retain_packet_bytes (session_t *session,
                                 const zlink_routing_id_t *rid,
                                 const zlink_msg_t *header_part,
                                 const zlink_msg_t *body_part)
{
    if (!session || !rid || !header_part || !body_part)
        return false;
    const size_t header_size = zlink_msg_size (const_cast<zlink_msg_t *> (header_part));
    const size_t body_size = zlink_msg_size (const_cast<zlink_msg_t *> (body_part));
    session->retained_packet.resize (6 + header_size + body_size);
    unsigned char *dst = session->retained_packet.data ();
    dst[0] = static_cast<unsigned char> ((header_size >> 8) & 0xFF);
    dst[1] = static_cast<unsigned char> (header_size & 0xFF);
    perf_stream_common::perf_stream_store_u32_be (dst + 2, static_cast<uint32_t> (body_size));
    if (header_size > 0)
        std::memcpy (dst + 6, zlink_msg_data (const_cast<zlink_msg_t *> (header_part)),
                     header_size);
    if (body_size > 0)
        std::memcpy (dst + 6 + header_size, zlink_msg_data (const_cast<zlink_msg_t *> (body_part)),
                     body_size);
    session->retained_rid = *rid;
    return true;
}

inline bool submit_packet_async (session_t *session,
                                 const zlink_routing_id_t *rid,
                                 zlink_msg_t *packet,
                                 const zlink_msg_t *header_part,
                                 const zlink_msg_t *body_part)
{
    if (!session || !session->send_socket || !rid || rid->size == 0 || !packet
        || !header_part || !body_part
        || session->wait_token != 0 || !session->retained_packet.empty ()
        || outstanding_size (session) != 0) {
        errno = EBUSY;
        return false;
    }

    const size_t packet_size = zlink_msg_size (packet);
    if (packet_size == 0) {
        errno = EINVAL;
        return false;
    }

    zlink_completion_id_t wait_token = 0;
    const zlink_submit_result_t result = zlink_send_part_rid (
      session->send_socket, rid, packet, ZLINK_SEND_FLAGS_DONTWAIT,
      ZLINK_PART_FINAL, session->send_socket, &wait_token);
    const int submit_errno = result == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    if (result == ZLINK_SUBMIT_OK && wait_token == 0) {
        record_immediate_admission (session);
        return true;
    }
    if (result == ZLINK_SUBMIT_BACKPRESSURED
        && (submit_errno == EAGAIN || submit_errno == EWOULDBLOCK)
        && wait_token != 0) {
        if (!retain_packet_bytes (session, rid, header_part, body_part)) {
            release_retained_packet (session);
            errno = EINVAL;
            return false;
        }
        session->wait_token = wait_token;
        session->retry_ready = false;
        session->pollout_suppressed = false;
        session->outstanding_count.fetch_add (1, std::memory_order_acq_rel);
        errno = submit_errno;
        return true;
    }

    release_retained_packet (session);
    errno = result == ZLINK_SUBMIT_OK
              || result == ZLINK_SUBMIT_BACKPRESSURED
              ? EPROTO
              : (submit_errno != 0 ? submit_errno : EIO);
    return false;
}

inline bool retry_retained_packet (session_t *session)
{
    if (!session || !session->send_socket || session->wait_token != 0
        || !session->retry_ready || session->retained_packet.empty ()
        || session->retained_rid.size == 0 || outstanding_size (session) != 1) {
        errno = EPROTO;
        return false;
    }

    zlink_msg_t packet;
    if (zlink_msg_init_size (&packet, session->retained_packet.size ()) != 0)
        return false;
    std::memcpy (zlink_msg_data (&packet), session->retained_packet.data (),
                 session->retained_packet.size ());

    zlink_completion_id_t wait_token = 0;
    const zlink_submit_result_t result = zlink_send_part_rid (
      session->send_socket, &session->retained_rid, &packet,
      ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, session->send_socket,
      &wait_token);
    const int submit_errno = result == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    zlink_msg_close (&packet);

    if (result == ZLINK_SUBMIT_OK && wait_token == 0) {
        release_retained_packet (session);
        finish_pending_submission (session);
        record_immediate_admission (session);
        return true;
    }
    if (result == ZLINK_SUBMIT_BACKPRESSURED
        && (submit_errno == EAGAIN || submit_errno == EWOULDBLOCK)
        && wait_token != 0) {
        session->wait_token = wait_token;
        session->retry_ready = false;
        session->pollout_suppressed = false;
        errno = submit_errno;
        return true;
    }

    release_retained_packet (session);
    finish_pending_submission (session);
    errno = result == ZLINK_SUBMIT_OK
              || result == ZLINK_SUBMIT_BACKPRESSURED
              ? EPROTO
              : (submit_errno != 0 ? submit_errno : EIO);
    return false;
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

    // The message handle is consumed regardless of admission outcome; the
    // exact bytes are rebuilt from header/body only when Core refuses it.
    const bool submitted = submit_packet_async (session, rid, &packet, header_part, body_part);
    const int submit_errno = !submitted || outstanding_size (session) != 0
                               ? zlink_errno ()
                               : 0;
    zlink_msg_close (&packet);
    if (submit_errno != 0)
        errno = submit_errno;
    return submitted;
}

inline bool drain_writable_completions (session_t *session,
                                        bool suppress_pollout_if_empty)
{
    if (!session || !session->send_socket)
        return false;
    for (;;) {
        zlink_completion_t completion;
        std::memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t rc = zlink_completion_recv (
          session->send_socket, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            if (suppress_pollout_if_empty && session->wait_token != 0)
                session->pollout_suppressed = true;
            break;
        }
        if (rc != ZLINK_RECV_OK)
            return false;
        const bool valid = record_writable_completion (session, &completion);
        const int completion_errno = valid ? 0 : errno;
        zlink_completion_close (&completion);
        if (!valid) {
            errno = completion_errno;
            return false;
        }
    }

    // The completion queue must reach NO_DATA before the exact retained packet
    // is submitted again.
    return !session->retry_ready || retry_retained_packet (session);
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
        if (outstanding_size (session) != 0)
            return true;
        if (perf_stop_requested ().load (std::memory_order_acquire))
            return true;
    }
}

inline short session_poll_events (const session_t *session, bool accept_input)
{
    short events = ZLINK_POLLCOMPLETION;
    if (session && session->wait_token != 0
        && !session->pollout_suppressed)
        events = static_cast<short> (events | ZLINK_POLLOUT);
    if (accept_input && session && outstanding_size (session) == 0)
        events = static_cast<short> (events | ZLINK_POLLIN);
    return events;
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
                             session_poll_events (session, true))
             != ZLINK_CONFIG_OK) {
        if (poller)
            zlink_poller_destroy (&poller);
        return 1;
    }

    short registered_events = session_poll_events (session, true);
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

        // POLLIN/POLLOUT interest changes only around a live wait token or
        // the stop edge; skip the poller call on the steady admitted path.
        const short desired_events = session_poll_events (session, !stopping);
        if (desired_events != registered_events) {
            if (zlink_poller_modify (poller, session->send_socket, desired_events)
                != ZLINK_CONFIG_OK) {
                record_failure (session, zlink_errno ());
                break;
            }
            registered_events = desired_events;
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
        if (poll_rc > 0
            && (event.events & (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION)) != 0
            && !drain_writable_completions (session, true)) {
            record_failure (session, zlink_errno ());
            break;
        }
        if (!stopping && outstanding_size (session) == 0 && poll_rc > 0
            && (event.events & ZLINK_POLLIN) != 0
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
