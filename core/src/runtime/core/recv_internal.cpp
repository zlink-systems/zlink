/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/recv_internal.hpp"

#include "core/msg.hpp"
#include "core/recv_tls_view.hpp"
#include "core/socket_poller.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/clock.hpp"
#include "utils/sleep.hpp"

#include <climits>
#include <string.h>

int zlink::recv_msg_socket (socket_base_t *socket_, int type_, zlink_msg_t *msg_, int flags_)
{
    // Hot path: direct recv-mode entry for single-message public recv. Changes
    // here affect PAIR/DEALER small-message throughput even without contention.
    if (!socket_ || !msg_) {
        errno = EFAULT;
        return -1;
    }

    switch (type_) {
        case ZLINK_CORE_SOCKET_SUB:
        case ZLINK_CORE_SOCKET_XSUB:
            if (socket_->sub_dispatch_active ()) {
                errno = EBUSY;
                return -1;
            }
            break;

        case ZLINK_CORE_SOCKET_XPUB:
            if (socket_->xpub_dispatch_active ()) {
                errno = EBUSY;
                return -1;
            }
            break;

        case ZLINK_CORE_SOCKET_STREAM:
            if (socket_->stream_dispatch_active ()) {
                errno = EBUSY;
                return -1;
            }
            break;

        default:
            if (socket_->socket_msg_dispatch_active ()) {
                errno = EBUSY;
                return -1;
            }
            break;
    }

    const int rc = socket_->recv (reinterpret_cast<msg_t *> (msg_), flags_);
    if (rc < 0)
        return -1;

    const size_t size = zlink_msg_size (msg_);
    return static_cast<int> (size < static_cast<size_t> (INT_MAX) ? size : INT_MAX);
}

int zlink::recv_msg_internal (void *socket_, zlink_msg_t *msg_, int flags_)
{
    socket_base_t *socket = static_cast<socket_base_t *> (socket_);
    if (!socket || !socket->check_tag ()) {
        errno = EFAULT;
        return -1;
    }

    return recv_msg_socket (socket, socket->socket_type (), msg_, flags_);
}

int zlink::recv_msg_routed_socket (socket_base_t *socket_,
                                   zlink_msg_t *msg_,
                                   zlink_routing_id_t *source_rid_out_,
                                   int flags_,
                                   uint64_t *connection_id_out_)
{
    if (!socket_ || !msg_) {
        errno = EFAULT;
        return -1;
    }

    if (socket_->socket_msg_dispatch_active ()) {
        errno = EBUSY;
        return -1;
    }

    const int rc = socket_->recv_routed (
      reinterpret_cast<msg_t *> (msg_), source_rid_out_, flags_,
      connection_id_out_);
    if (rc < 0)
        return -1;

    const size_t size = zlink_msg_size (msg_);
    return static_cast<int> (size < static_cast<size_t> (INT_MAX) ? size : INT_MAX);
}

int zlink::recv_msg_routed_internal (void *socket_,
                                     zlink_msg_t *msg_,
                                     zlink_routing_id_t *source_rid_out_,
                                     int flags_,
                                     uint64_t *connection_id_out_)
{
    socket_base_t *socket = static_cast<socket_base_t *> (socket_);
    if (!socket || !socket->check_tag ()) {
        errno = EFAULT;
        return -1;
    }

    const int type = socket->socket_type ();
    if (type != ZLINK_CORE_SOCKET_ROUTER) {
        errno = ENOTSUP;
        return -1;
    }

    return recv_msg_routed_socket (
      socket, msg_, source_rid_out_, flags_, connection_id_out_);
}

int zlink::recv_followup_msg_socket (socket_base_t *socket_, zlink_msg_t *msg_)
{
    if (!socket_ || !msg_) {
        errno = EFAULT;
        return -1;
    }

    const int rc = socket_->recv (reinterpret_cast<msg_t *> (msg_), ZLINK_DONTWAIT);
    if (rc >= 0) {
        const size_t size = zlink_msg_size (msg_);
        return static_cast<int> (size < static_cast<size_t> (INT_MAX) ? size : INT_MAX);
    }

    if (errno == EAGAIN || errno == EINTR)
        errno = EPROTO;
    return -1;
}

int zlink::recv_followup_msg_internal (void *socket_, zlink_msg_t *msg_)
{
    socket_base_t *socket = static_cast<socket_base_t *> (socket_);
    if (!socket || !socket->check_tag ()) {
        errno = EFAULT;
        return -1;
    }
    return recv_followup_msg_socket (socket, msg_);
}

int zlink::recv_followup_msg_socket_wait (socket_base_t *socket_, zlink_msg_t *msg_, int flags_)
{
    if (!socket_ || !msg_) {
        errno = EFAULT;
        return -1;
    }

    while (socket_->recv (reinterpret_cast<msg_t *> (msg_), flags_) != 0) {
        const int saved_errno = errno;
        if ((flags_ & ZLINK_DONTWAIT) != 0 || (saved_errno != EAGAIN && saved_errno != EINTR)) {
            errno = saved_errno;
            return -1;
        }
        if (wait_socket_events_internal (socket_, ZLINK_POLLIN, -1) <= 0) {
            errno = saved_errno;
            return -1;
        }
    }

    errno = 0;
    return 0;
}

bool zlink::msg_frame_has_more (const zlink_msg_t &msg_)
{
    return (reinterpret_cast<const msg_t *> (&msg_)->flags () & msg_t::more) != 0;
}

void zlink::close_msg_frames (std::vector<zlink_msg_t> *frames_)
{
    if (!frames_)
        return;

    for (size_t i = 0; i < frames_->size (); ++i)
        zlink_msg_close (&(*frames_)[i]);
    frames_->clear ();
}

bool zlink::recv_msg_sequence_socket_wait (socket_base_t *socket_,
                                           std::vector<zlink_msg_t> *frames_,
                                           long followup_timeout_ms_)
{
    if (!socket_ || !frames_) {
        errno = EFAULT;
        return false;
    }

    frames_->clear ();
    while (true) {
        zlink_msg_t frame;
        zlink_msg_init (&frame);
        if (socket_->recv (reinterpret_cast<msg_t *> (&frame), 0) != 0) {
            zlink_msg_close (&frame);
            close_msg_frames (frames_);
            return false;
        }

        frames_->push_back (frame);
        if (!msg_frame_has_more (frame))
            return !frames_->empty ();

        if (wait_socket_events_internal (socket_, ZLINK_POLLIN, followup_timeout_ms_) <= 0) {
            errno = EAGAIN;
            close_msg_frames (frames_);
            return false;
        }
    }
}

int zlink::drain_followup_msg_sequence (socket_base_t *socket_,
                                        zlink_msg_t *frame_,
                                        bool wait_for_input_)
{
    if (!socket_ || !frame_) {
        errno = EFAULT;
        return -1;
    }

    bool more = msg_frame_has_more (*frame_);
    zlink_msg_close (frame_);
    while (more) {
        zlink_msg_t next;
        zlink_msg_init (&next);
        const int rc = wait_for_input_ ? recv_followup_msg_socket_wait (socket_, &next, 0)
                                       : recv_followup_msg_socket (socket_, &next);
        if (rc < 0) {
            zlink_msg_close (&next);
            return -1;
        }
        more = msg_frame_has_more (next);
        zlink_msg_close (&next);
    }

    errno = 0;
    return 0;
}

int zlink::export_payload_msg_sequence (socket_base_t *socket_,
                                        zlink_msg_t *first_payload_,
                                        zlink_msg_t **parts_out_,
                                        size_t *part_count_out_,
                                        bool allow_single_)
{
    if (!socket_ || !first_payload_ || !parts_out_ || !part_count_out_) {
        errno = EFAULT;
        return -1;
    }

    if (allow_single_ && !msg_frame_has_more (*first_payload_))
        return recv_tls_view::export_single (first_payload_, parts_out_, part_count_out_);

    zlink_msg_t current;
    zlink_msg_init (&current);
    if (zlink_msg_move (&current, first_payload_) != 0) {
        zlink_msg_close (&current);
        errno = EFAULT;
        return -1;
    }

    while (true) {
        const bool more = msg_frame_has_more (current);
        if (recv_tls_view::push (&current) != 0) {
            const int saved_errno = errno;
            (void) drain_followup_msg_sequence (socket_, &current, true);
            recv_tls_view::abort ();
            errno = saved_errno;
            return -1;
        }

        if (!more)
            return recv_tls_view::commit (parts_out_, part_count_out_);

        zlink_msg_t next;
        zlink_msg_init (&next);
        if (recv_followup_msg_socket_wait (socket_, &next, 0) < 0) {
            zlink_msg_close (&next);
            recv_tls_view::abort ();
            return -1;
        }
        if (zlink_msg_move (&current, &next) != 0) {
            zlink_msg_close (&next);
            recv_tls_view::abort ();
            errno = EFAULT;
            return -1;
        }
    }
}

int zlink::export_reserved_followup_msg_sequence (socket_base_t *socket_,
                                                  zlink_msg_t **parts_out_,
                                                  size_t *part_count_out_,
                                                  bool wait_for_input_)
{
    if (!socket_ || !parts_out_ || !part_count_out_) {
        errno = EFAULT;
        return -1;
    }

    if (recv_tls_view::storage ().count == 0 && recv_tls_view::reserve_first_slot () != 0) {
        return -1;
    }

    while (true) {
        recv_tls_view::storage_t &tls = recv_tls_view::storage ();
        const bool more = msg_frame_has_more (tls.parts[tls.count - 1]);
        if (!more)
            return recv_tls_view::commit (parts_out_, part_count_out_);

        zlink_msg_t next;
        zlink_msg_init (&next);
        const int rc = wait_for_input_ ? recv_followup_msg_socket_wait (socket_, &next, 0)
                                       : recv_followup_msg_socket (socket_, &next);
        if (rc < 0) {
            zlink_msg_close (&next);
            recv_tls_view::abort ();
            return -1;
        }

        if (recv_tls_view::push (&next) != 0) {
            const int saved_errno = errno;
            if (wait_for_input_)
                zlink_msg_close (&next);
            else
                (void) drain_followup_msg_sequence (socket_, &next, false);
            recv_tls_view::abort ();
            errno = saved_errno;
            return -1;
        }
    }
}

int zlink::recv_buffer_internal (void *socket_, void *buf_, size_t len_, int flags_)
{
    zlink_msg_t msg;
    if (zlink_msg_init (&msg) != 0) {
        errno = EFAULT;
        return -1;
    }

    const int nbytes = recv_msg_internal (socket_, &msg, flags_);
    if (nbytes < 0) {
        const int err = errno;
        zlink_msg_close (&msg);
        errno = err;
        return -1;
    }

    const size_t to_copy =
      static_cast<size_t> (nbytes) < len_ ? static_cast<size_t> (nbytes) : len_;
    if (to_copy > 0 && buf_)
        memcpy (buf_, zlink_msg_data (&msg), to_copy);

    zlink_msg_close (&msg);
    return nbytes;
}

int zlink::wait_socket_events_internal (void *socket_, short events_, long timeout_ms_)
{
    socket_base_t *socket = static_cast<socket_base_t *> (socket_);
    if (!socket || !socket->check_tag ()) {
        errno = EFAULT;
        return -1;
    }

    if (events_ == ZLINK_POLLIN) {
        zlink::socket_poller_t poller;
        if (poller.add (socket, NULL, ZLINK_POLLIN) != 0)
            return -1;
        zlink::socket_poller_t::event_t event;
        const int rc = poller.wait (&event, 1, timeout_ms_);
        if (rc > 0)
            return 1;
        if (rc == 0)
            return 0;
        if (errno == EAGAIN)
            return 0;
        return -1;
    }

    zlink::clock_t clock;
    const uint64_t deadline =
      timeout_ms_ >= 0 ? clock.now_ms () + static_cast<uint64_t> (timeout_ms_) : 0;

    while (true) {
        uint32_t ready = 0;
        const short non_out_events = events_ & ~ZLINK_POLLOUT;
        if (non_out_events != 0) {
            if (socket->get_events_internal (non_out_events, &ready) != 0)
                return -1;
        }
        if ((events_ & ZLINK_POLLOUT) != 0 && socket->transport_has_out ())
            ready |= ZLINK_POLLOUT;
        if ((ready & events_) == static_cast<uint32_t> (events_))
            return 1;

        if (timeout_ms_ == 0)
            return 0;
        if (timeout_ms_ > 0 && clock.now_ms () >= deadline)
            return 0;

        zlink::sleep_ms (1);
    }
}
