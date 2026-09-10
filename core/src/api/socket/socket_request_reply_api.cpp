/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

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
