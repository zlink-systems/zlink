/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/send_internal.hpp"

#include "core/msg.hpp"
#include "sockets/common/socket_base.hpp"

#include <climits>

int zlink::send_msg_internal (void *socket_, zlink_msg_t *msg_, int flags_)
{
    socket_base_t *socket = static_cast<socket_base_t *> (socket_);
    if (!socket || !socket->check_tag ()) {
        errno = EFAULT;
        return -1;
    }

    msg_t *msg = reinterpret_cast<msg_t *> (msg_);
    if (!msg || !msg->check ()) {
        errno = EFAULT;
        return -1;
    }

    const size_t size = msg->size ();
    const int rc = socket->send (msg, flags_);
    if (rc < 0)
        return -1;

    return static_cast<int> (size < static_cast<size_t> (INT_MAX) ? size : INT_MAX);
}

int zlink::send_msg_routed_internal (void *socket_,
                                     const zlink_routing_id_t *target_rid_,
                                     zlink_msg_t *msg_,
                                     int flags_)
{
    socket_base_t *socket = static_cast<socket_base_t *> (socket_);
    if (!socket || !socket->check_tag () || !target_rid_) {
        errno = EFAULT;
        return -1;
    }

    msg_t *msg = reinterpret_cast<msg_t *> (msg_);
    if (!msg || !msg->check ()) {
        errno = EFAULT;
        return -1;
    }

    const size_t size = msg->size ();
    const int rc = socket->send_routed (target_rid_, msg, flags_);
    if (rc < 0)
        return -1;

    return static_cast<int> (size < static_cast<size_t> (INT_MAX) ? size : INT_MAX);
}
