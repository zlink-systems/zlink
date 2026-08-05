/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CLOSE_RESULT_INTERNAL_HPP_INCLUDED__
#define __ZLINK_CLOSE_RESULT_INTERNAL_HPP_INCLUDED__

#include "api/message/result_errno_internal.hpp"

namespace zlink
{
namespace close_result_internal
{
inline zlink_close_result_t from_errno (int err_)
{
    switch (err_) {
        case 0:
            return ZLINK_CLOSE_OK;
        case EBUSY:
            return ZLINK_CLOSE_BUSY;
#ifdef ESHUTDOWN
        case ESHUTDOWN:
            return ZLINK_CLOSE_SHUTDOWN;
#endif
        case EFAULT:
        case ESTALE:
            return ZLINK_CLOSE_INVALID_HANDLE;
        default:
            return ZLINK_CLOSE_INTERNAL_ERROR;
    }
}

inline zlink_close_result_t from_rc (int rc_)
{
    if (rc_ == 0)
        return ZLINK_CLOSE_OK;

    return from_errno (zlink::result_errno_internal::rc_errno_or_io ());
}
}
}

#endif
