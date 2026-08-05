/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/socket/part_helper_internal.hpp"
#include "api/socket/socket_request_reply_submit_internal.hpp"

namespace reqrep = zlink::socket_reqrep_internal;

int reqrep::validate_socket_type (void *socket_, int expected_type_)
{
    const socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket)
        return -1;

    if (socket_type (handle) != expected_type_) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

int reqrep::validate_request_send_flags (zlink_send_flags_t flags_)
{
    if (flags_ != 0 && flags_ != ZLINK_DONTWAIT) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

int reqrep::send_request_frame (zlink::socket_base_t *socket_,
                                zlink::part_helper_internal::handle_state_t *helper_state_,
                                const zlink_routing_id_t *peer_rid_,
                                const void *data_,
                                size_t size_,
                                int flags_,
                                zlink::pipe_t **application_pipe_out_)
{
    zlink::msg_t msg;
    if (msg.init_size (size_) != 0)
        return -1;
    if (size_ > 0 && data_)
        memcpy (msg.data (), data_, size_);

    const int rc = peer_rid_ ? socket_->send_routed_scoped (
                                peer_rid_, &msg, flags_,
                                *helper_state_->send.send_scope, NULL, 0,
                                application_pipe_out_)
                             : socket_->send_scoped (&msg, flags_, *helper_state_->send.send_scope,
                                                    application_pipe_out_);
    const int saved_errno = errno;
    (void) msg.close ();
    errno = saved_errno;
    return rc;
}

int reqrep::send_request_payload_part (zlink::socket_base_t *socket_,
                                       zlink::part_helper_internal::handle_state_t *helper_state_,
                                       const zlink_routing_id_t *peer_rid_,
                                       zlink_msg_t *part_,
                                       zlink_send_flags_t flags_,
                                       zlink_part_flag_t part_flag_)
{
    LIBZLINK_UNUSED (peer_rid_);

    return socket_->send_scoped (reinterpret_cast<zlink::msg_t *> (part_),
                                 static_cast<int> (flags_ & ZLINK_DONTWAIT)
                                   | (part_flag_ == ZLINK_PART_MORE ? ZLINK_SNDMORE : 0),
                                 *helper_state_->send.send_scope);
}

int reqrep::stage_request_payload_part (
  zlink::part_helper_internal::handle_state_t *helper_state_, zlink_msg_t *part_)
{
    if (!helper_state_ || !part_) {
        errno = EFAULT;
        return -1;
    }

    helper_state_->send.buffered_parts.resize (helper_state_->send.buffered_parts.size () + 1);
    zlink_msg_t &slot = helper_state_->send.buffered_parts.back ();
    zlink_msg_init (&slot);
    if (zlink_msg_move (&slot, part_) != 0) {
        zlink_msg_close (&slot);
        helper_state_->send.buffered_parts.pop_back ();
        errno = EFAULT;
        return -1;
    }

    return 0;
}
