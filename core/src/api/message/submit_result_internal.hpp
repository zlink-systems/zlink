/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SUBMIT_RESULT_INTERNAL_HPP_INCLUDED__
#define __ZLINK_SUBMIT_RESULT_INTERNAL_HPP_INCLUDED__

#include "api/message/result_errno_internal.hpp"

namespace zlink
{
namespace submit_result_internal
{
inline zlink_submit_result_t from_errno (int err_)
{
    if (zlink::result_errno_internal::is_not_supported (err_))
        return ZLINK_SUBMIT_NOT_SUPPORTED;

    switch (zlink::internal_errno::classify_submit (err_)) {
        case zlink::internal_errno::submit_error_class::none:
            return ZLINK_SUBMIT_OK;
        case zlink::internal_errno::submit_error_class::control_flow:
            switch (err_) {
                case EAGAIN:
                case ETIMEDOUT:
                case ENOBUFS:
                    return ZLINK_SUBMIT_BACKPRESSURED;
                case ENOTCONN:
                case EHOSTUNREACH:
                    return ZLINK_SUBMIT_NOT_CONNECTED;
                case ECONNREFUSED:
                case EACCES:
                    return ZLINK_SUBMIT_NOT_ADMITTED;
                case ENOENT:
                    return ZLINK_SUBMIT_NOT_FOUND;
                default:
                    return ZLINK_SUBMIT_INTERNAL_ERROR;
            }
        case zlink::internal_errno::submit_error_class::runtime_failure:
            return ZLINK_SUBMIT_TERMINATED;
        case zlink::internal_errno::submit_error_class::contract_failure:
            switch (err_) {
                case EFAULT:
                    return ZLINK_SUBMIT_INVALID_HANDLE;
                case EINVAL:
                case EMSGSIZE:
                    return ZLINK_SUBMIT_INVALID_ARGUMENT;
                case EFSM:
                case EBUSY:
                case ESTALE:
                case EALREADY:
                    return ZLINK_SUBMIT_INVALID_STATE;
                case EDEADLK:
                case EPERM:
                case EMTHREAD:
                    return ZLINK_SUBMIT_THREAD_VIOLATION;
                case EOVERFLOW:
                    return ZLINK_SUBMIT_SEQ_EXHAUSTED;
                default:
                    return ZLINK_SUBMIT_INTERNAL_ERROR;
            }
        case zlink::internal_errno::submit_error_class::internal_failure:
            switch (err_) {
                case ENOMEM:
                    return ZLINK_SUBMIT_OUT_OF_MEMORY;
                default:
                    return ZLINK_SUBMIT_INTERNAL_ERROR;
            }
        case zlink::internal_errno::submit_error_class::unknown:
        default:
            return ZLINK_SUBMIT_INTERNAL_ERROR;
    }
}

inline zlink_submit_result_t from_rc (int rc_)
{
    if (rc_ == 0)
        return ZLINK_SUBMIT_OK;

    return from_errno (zlink::result_errno_internal::rc_errno_or_io ());
}

inline zlink_submit_result_t from_request_submit_errno (int err_)
{
    // Older request paths report EBUSY while the 10.0.0 contract uses
    // EOVERFLOW for operation-sequence exhaustion.
    if (err_ == EBUSY || err_ == EOVERFLOW)
        return ZLINK_SUBMIT_SEQ_EXHAUSTED;

    return from_errno (err_);
}

inline zlink_submit_result_t from_request_submit_rc (int rc_)
{
    if (rc_ == 0)
        return ZLINK_SUBMIT_OK;

    return from_request_submit_errno (zlink::result_errno_internal::rc_errno_or_io ());
}
}
}

#endif
