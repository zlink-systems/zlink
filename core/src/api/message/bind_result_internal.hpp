/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_BIND_RESULT_INTERNAL_HPP_INCLUDED__
#define __ZLINK_BIND_RESULT_INTERNAL_HPP_INCLUDED__

#include "api/message/result_errno_internal.hpp"

namespace zlink
{
namespace bind_result_internal
{
inline zlink_bind_result_t from_errno (int err_)
{
    if (zlink::result_errno_internal::is_not_supported (err_)
        || err_ == EPROTONOSUPPORT)
        return ZLINK_BIND_NOT_SUPPORTED;

    switch (err_) {
        case 0:
            return ZLINK_BIND_OK;
        case EINVAL:
            return ZLINK_BIND_INVALID_ARGUMENT;
        case EADDRINUSE:
            return ZLINK_BIND_ADDR_IN_USE;
        case EFAULT:
            return ZLINK_BIND_INVALID_HANDLE;
        default:
            return ZLINK_BIND_INTERNAL_ERROR;
    }
}

inline zlink_bind_result_t from_rc (int rc_)
{
    if (rc_ == 0)
        return ZLINK_BIND_OK;

    return from_errno (zlink::result_errno_internal::rc_errno_or_io ());
}
}
}

#endif
