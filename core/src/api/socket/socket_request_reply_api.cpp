/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/socket/part_helper_internal.hpp"
#include "api/socket/socket_request_reply_submit_internal.hpp"

namespace reqrep = zlink::socket_reqrep_internal;

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

int reqrep::stage_request_payload_part (
  zlink::part_helper_internal::send_sequence_state_t *sequence_, zlink_msg_t *part_)
{
    if (!sequence_ || !part_) {
        errno = EFAULT;
        return -1;
    }

    zlink_msg_t *slot = NULL;
    try {
#ifdef ZLINK_BUILD_TESTS
        reqrep::test_throw_request_reply_allocation_failpoint (
          reqrep::request_reply_allocation_stage_payload);
#endif
        slot = &sequence_->buffered_parts.append_uninitialized ();
    } catch (...) {
        errno = ENOMEM;
        return -1;
    }
    zlink_msg_init (slot);
    if (zlink_msg_move (slot, part_) != 0) {
        zlink_msg_close (slot);
        sequence_->buffered_parts.pop_back ();
        errno = EFAULT;
        return -1;
    }

    return 0;
}
