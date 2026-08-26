/* SPDX-License-Identifier: MPL-2.0 */

//  Public entry points for Core-owned asynchronous send admission.
//
//  The whole record crosses the boundary in one call. That is what removes the
//  per-handle send-sequence hazard structurally: the sequence is taken and
//  released inside this call, so a multipart record never holds it across
//  application code.

#include "utils/precompiled.hpp"

#include "api/message/submit_result_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "core/msg.hpp"
#include "utils/err.hpp"
#include "utils/routing_id.hpp"

zlink_submit_result_t zlink_send_async (void *s_,
                                        zlink_msg_t *parts_,
                                        size_t part_count_,
                                        const zlink_send_async_options_t *options_,
                                        zlink_send_op_id_t *op_id_out_)
{
    if (op_id_out_)
        *op_id_out_ = 0;

    //  Ownership only moves on ZLINK_SUBMIT_OK, so every failure below leaves
    //  the caller's parts untouched.
    if (!parts_ || part_count_ == 0 || !options_) {
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }

    zlink::socket_base_t *socket = try_as_socket (s_);
    if (!socket) {
        errno = EFAULT;
        return zlink::submit_result_internal::from_errno (errno);
    }

    //  A multipart sequence in flight on this handle owns the send gate. A
    //  record submit cannot interleave with it.
    if (zlink::part_helper_internal::send_sequence_active (s_)) {
        errno = EINVAL;
        return zlink::submit_result_internal::from_errno (errno);
    }

    if (part_count_ == 1
        && options_->struct_size == sizeof (zlink_send_async_options_t)
        && socket->socket_type () == ZLINK_CORE_SOCKET_ROUTER
        && options_->target
        && options_->target->transport_pair_id == 0
        && options_->target->transport_pair_generation == 0
        && zlink::valid_routing_id (&options_->target->peer_rid)
        && socket->send_complete_handler_active ()
        && socket->try_send_async_routed_single_immediate (
          &options_->target->peer_rid, parts_))
        return ZLINK_SUBMIT_OK;

    return zlink::submit_result_internal::from_rc (
      socket->send_async_submit (parts_, part_count_, options_, op_id_out_));
}

zlink_submit_result_t zlink_send_async_cancel (void *s_,
                                                zlink_send_op_id_t op_id_)
{
    zlink::socket_base_t *socket = try_as_socket (s_);
    if (!socket) {
        errno = EFAULT;
        return zlink::submit_result_internal::from_errno (errno);
    }

    const int rc = socket->send_async_cancel (op_id_);
    if (rc == 0)
        return ZLINK_SUBMIT_OK;
    //  EBUSY means another resolver already claimed the operation. It is no
    //  longer cancellable but still completes exactly once as that resolver's
    //  ADMITTED or TERMINAL result.
    if (errno == EBUSY)
        return ZLINK_SUBMIT_INVALID_STATE;
    if (errno == ENOENT)
        return ZLINK_SUBMIT_NOT_FOUND;
    return zlink::submit_result_internal::from_errno (errno);
}
