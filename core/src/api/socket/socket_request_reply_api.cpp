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

    return validate_socket_type (handle, expected_type_);
}

int reqrep::validate_socket_type (const socket_handle_t &handle_, int expected_type_)
{
    if (!handle_.socket) {
        errno = EFAULT;
        return -1;
    }

    if (socket_type (handle_) != expected_type_) {
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

int reqrep::send_request_payload_part (zlink::socket_base_t *socket_,
                                       zlink::part_helper_internal::handle_state_t *helper_state_,
                                       const zlink_routing_id_t *peer_rid_,
                                       zlink_msg_t *part_,
                                       zlink_send_flags_t flags_,
                                       zlink_part_flag_t part_flag_,
                                       bool first_part_,
                                       uint64_t transport_pair_id_,
                                       uint64_t transport_pair_generation_,
                                       zlink::pipe_t **application_pipe_out_,
                                       zlink::pipe_write_observer_fn observer_,
                                       void *observer_userdata_)
{
    if (!socket_ || !helper_state_ || !part_ || !helper_state_->send.send_scope) {
        errno = EFAULT;
        return -1;
    }

    const int send_flags = static_cast<int> (flags_ & ZLINK_DONTWAIT)
                           | (part_flag_ == ZLINK_PART_MORE ? ZLINK_SNDMORE : 0);
    zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (part_);
    if (first_part_ && peer_rid_) {
        return socket_->send_routed_scoped (
          peer_rid_, msg, send_flags, *helper_state_->send.send_scope, NULL, 0,
          application_pipe_out_, transport_pair_id_, transport_pair_generation_,
          true, observer_, observer_userdata_);
    }
    return socket_->send_scoped (
      msg, send_flags, *helper_state_->send.send_scope,
      first_part_ ? application_pipe_out_ : NULL, true, observer_,
      observer_userdata_);
}

int reqrep::stage_request_payload_part (
  zlink::part_helper_internal::handle_state_t *helper_state_, zlink_msg_t *part_)
{
    if (!helper_state_ || !part_) {
        errno = EFAULT;
        return -1;
    }

    try {
#ifdef ZLINK_BUILD_TESTS
        reqrep::test_throw_request_reply_allocation_failpoint (
          reqrep::request_reply_allocation_stage_payload);
#endif
        helper_state_->send.buffered_parts.resize (
          helper_state_->send.buffered_parts.size () + 1);
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
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
