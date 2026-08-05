/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_RECV_RESULT_INTERNAL_HPP_INCLUDED__
#define __ZLINK_RECV_RESULT_INTERNAL_HPP_INCLUDED__

#include "api/message/result_errno_internal.hpp"

namespace zlink
{
namespace recv_result_internal
{
inline zlink_recv_result_t from_errno (int err_)
{
    if (zlink::result_errno_internal::is_not_supported (err_))
        return ZLINK_RECV_NOT_SUPPORTED;

    switch (err_) {
        case 0:
            return ZLINK_RECV_OK;
        case EAGAIN:
        case ETIMEDOUT:
            return ZLINK_RECV_NO_DATA;
        case EBUSY:
            return ZLINK_RECV_BUSY;
        case ETERM:
            return ZLINK_RECV_TERMINATED;
        case EFAULT:
            return ZLINK_RECV_INVALID_HANDLE;
        case ENOBUFS:
            return ZLINK_RECV_BUFFER_TOO_SMALL;
        case EINVAL:
        case ESTALE:
        case ESHUTDOWN:
            return ZLINK_RECV_INVALID_STATE;
        default:
            return ZLINK_RECV_INTERNAL_ERROR;
    }
}

inline zlink_recv_result_t from_rc (int rc_)
{
    if (rc_ == 0)
        return ZLINK_RECV_OK;

    return from_errno (zlink::result_errno_internal::rc_errno_or_io ());
}
}
}

#endif
