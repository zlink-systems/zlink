/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_HANDLER_RESULT_INTERNAL_HPP_INCLUDED__
#define __ZLINK_HANDLER_RESULT_INTERNAL_HPP_INCLUDED__

#include "api/message/result_errno_internal.hpp"

namespace zlink
{
namespace handler_result_internal
{
inline zlink_handler_result_t from_errno (int err_)
{
    if (zlink::result_errno_internal::is_not_supported (err_))
        return ZLINK_HANDLER_NOT_SUPPORTED;

    switch (err_) {
        case 0:
            return ZLINK_HANDLER_OK;
        case EINVAL:
            return ZLINK_HANDLER_INVALID_ARGUMENT;
        case EBUSY:
            return ZLINK_HANDLER_BUSY;
        case EDEADLK:
            return ZLINK_HANDLER_DEADLOCK;
        case EFAULT:
            return ZLINK_HANDLER_INVALID_HANDLE;
        default:
            return ZLINK_HANDLER_INTERNAL_ERROR;
    }
}

inline zlink_handler_result_t from_rc (int rc_)
{
    if (rc_ == 0)
        return ZLINK_HANDLER_OK;

    return from_errno (zlink::result_errno_internal::rc_errno_or_io ());
}
}
}

#endif
