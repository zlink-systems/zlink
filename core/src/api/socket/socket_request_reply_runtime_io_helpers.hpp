/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_SOCKET_REQUEST_REPLY_RUNTIME_IO_HELPERS_HPP_INCLUDED__
#define __ZLINK_API_SOCKET_REQUEST_REPLY_RUNTIME_IO_HELPERS_HPP_INCLUDED__

#include <zlink.h>

#include "api/socket/socket_api_internal.hpp"
#include "core/c_api_copy_internal.hpp"
#include "core/recv_internal.hpp"
#include "core/recv_tls_view.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/routing_id.hpp"

namespace zlink
{
namespace socket_reqrep_internal
{
inline void assign_routing_id_compact (zlink_routing_id_t *dest_, const zlink_routing_id_t &src_)
{
    if (!dest_)
        return;

    zlink::copy_routing_id_from_bytes (src_.data, src_.size, dest_);
}

inline int effective_recv_timeout_ms (zlink::socket_base_t *socket_,
                                      zlink_recv_flags_t flags_,
                                      int *timeout_ms_out_)
{
    if (!socket_ || !timeout_ms_out_) {
        errno = EFAULT;
        return -1;
    }
    if ((flags_ & ZLINK_DONTWAIT) != 0) {
        *timeout_ms_out_ = 0;
        return 0;
    }

    size_t timeout_size = sizeof (*timeout_ms_out_);
    return socket_->getsockopt (ZLINK_INTERNAL_OPT_RCVTIMEO, timeout_ms_out_, &timeout_size);
}

struct router_mandatory_scope_t
{
    router_mandatory_scope_t () : socket (NULL), restore_required (false), original_value (0) {}

    ~router_mandatory_scope_t () { restore (); }

    int arm (socket_handle_t handle_)
    {
        if (!handle_.socket || socket_type (handle_) != ZLINK_CORE_SOCKET_ROUTER)
            return 0;

        size_t size = sizeof (original_value);
        if (handle_.socket->getsockopt (ZLINK_INTERNAL_OPT_ROUTER_MANDATORY, &original_value, &size)
            != 0)
            return -1;

        if (original_value != 0)
            return 0;

        const int mandatory = 1;
        if (handle_.socket->setsockopt (ZLINK_INTERNAL_OPT_ROUTER_MANDATORY, &mandatory,
                                        sizeof (mandatory))
            != 0)
            return -1;

        socket = handle_.socket;
        restore_required = true;
        return 0;
    }

    void restore ()
    {
        if (!restore_required || !socket)
            return;

        (void) socket->setsockopt (ZLINK_INTERNAL_OPT_ROUTER_MANDATORY, &original_value,
                                   sizeof (original_value));
        restore_required = false;
        socket = NULL;
    }

    zlink::socket_base_t *socket;
    bool restore_required;
    int original_value;
};

struct router_control_frames_t
{
    router_control_frames_t () : active (true)
    {
        zlink_msg_init (&source_node);
        zlink_msg_init (&seq);
    }

    ~router_control_frames_t () { close (); }

    void close ()
    {
        if (!active)
            return;
        zlink_msg_close (&source_node);
        zlink_msg_close (&seq);
        active = false;
    }

    int fail (int err_)
    {
        close ();
        zlink::recv_tls_view::abort ();
        errno = err_;
        return -1;
    }

    zlink_msg_t source_node;
    zlink_msg_t seq;
    bool active;
};

struct routing_id_frame_view_t
{
    const void *data;
    size_t size;
};

inline routing_id_frame_view_t routing_id_frame_view (const zlink_routing_id_t *rid_)
{
    if (!zlink::valid_routing_id (rid_))
        return routing_id_frame_view_t{NULL, 0};

    return routing_id_frame_view_t{rid_->data, rid_->size};
}

inline int recv_internal_queue_frame (zlink::socket_base_t *socket_,
                                      zlink_msg_t *msg_,
                                      int flags_,
                                      int timeout_ms_,
                                      uint64_t deadline_ms_)
{
    if (!socket_ || !msg_) {
        errno = EFAULT;
        return -1;
    }

    while (socket_->recv (reinterpret_cast<zlink::msg_t *> (msg_), ZLINK_DONTWAIT) != 0) {
        const int saved_errno = errno;
        if (saved_errno != EAGAIN) {
            errno = saved_errno;
            return -1;
        }

        if ((flags_ & ZLINK_DONTWAIT) != 0 || timeout_ms_ == 0) {
            errno = EAGAIN;
            return -1;
        }

        long wait_ms = -1;
        if (timeout_ms_ > 0) {
            zlink::clock_t clock;
            const uint64_t now_ms = clock.now_ms ();
            if (now_ms >= deadline_ms_) {
                errno = EAGAIN;
                return -1;
            }
            wait_ms = static_cast<long> (deadline_ms_ - now_ms);
        }

        const int wait_rc = zlink::wait_socket_events_internal (socket_, ZLINK_POLLIN, wait_ms);
        if (wait_rc < 0)
            return -1;
        if (wait_rc == 0) {
            errno = EAGAIN;
            return -1;
        }
    }

    return 0;
}

inline int recv_router_followup_frame (zlink::socket_base_t *socket_, zlink_msg_t *msg_)
{
    if (!socket_ || !msg_) {
        errno = EFAULT;
        return -1;
    }

    if (socket_->recv (reinterpret_cast<zlink::msg_t *> (msg_), 0) != 0)
        return -1;

    return 0;
}

inline bool router_raw_part_has_more (const zlink_msg_t *part_)
{
    if (!part_)
        return false;

    const zlink::msg_t *msg = reinterpret_cast<const zlink::msg_t *> (part_);
    if (!msg->check ())
        return false;

    return (msg->flags () & zlink::msg_t::more) != 0;
}
}
}

#endif
