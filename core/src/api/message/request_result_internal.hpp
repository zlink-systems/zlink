/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_REQUEST_RESULT_INTERNAL_HPP_INCLUDED__
#define __ZLINK_REQUEST_RESULT_INTERNAL_HPP_INCLUDED__

#include "api/message/result_errno_internal.hpp"

namespace zlink
{
namespace request_result_internal
{
inline int to_errno (zlink_request_result_t result_)
{
    switch (result_) {
        case ZLINK_REQUEST_OK:
            return 0;
        case ZLINK_REQUEST_TIMED_OUT:
            return ETIMEDOUT;
        case ZLINK_REQUEST_NOT_FOUND:
            return ENOENT;
        case ZLINK_REQUEST_TERMINATED:
            return ETERM;
        case ZLINK_REQUEST_PROTOCOL_ERROR:
            return EPROTO;
        case ZLINK_REQUEST_REJECTED:
            return EACCES;
        case ZLINK_REQUEST_CONFLICT:
            return ESTALE;
        case ZLINK_REQUEST_BUSY:
            return EBUSY;
        case ZLINK_REQUEST_NOT_CONNECTED:
            return ENOTCONN;
        case ZLINK_REQUEST_INVALID_ARGUMENT:
            return EINVAL;
        case ZLINK_REQUEST_INVALID_STATE:
            return EFSM;
        case ZLINK_REQUEST_NOT_SUPPORTED:
            return ENOTSUP;
        case ZLINK_REQUEST_BACKPRESSURED:
            return EAGAIN;
        case ZLINK_REQUEST_INTERNAL_ERROR:
        default:
            return EIO;
    }
}

inline zlink_request_result_t from_errno (int err_)
{
    if (zlink::result_errno_internal::is_not_supported (err_))
        return ZLINK_REQUEST_NOT_SUPPORTED;

    switch (err_) {
        case EACCES:
        case ECONNREFUSED:
        case ECANCELED:
            return ZLINK_REQUEST_REJECTED;
        case ESTALE:
        case EEXIST:
            return ZLINK_REQUEST_CONFLICT;
        case EBUSY:
            return ZLINK_REQUEST_BUSY;
        case ENOTCONN:
        case EHOSTUNREACH:
            return ZLINK_REQUEST_NOT_CONNECTED;
        case EINVAL:
        case EFAULT:
            return ZLINK_REQUEST_INVALID_ARGUMENT;
        case EFSM:
        case EALREADY:
            return ZLINK_REQUEST_INVALID_STATE;
        case EAGAIN:
        case ENOBUFS:
            return ZLINK_REQUEST_BACKPRESSURED;
        case ESHUTDOWN:
            return ZLINK_REQUEST_TERMINATED;
        case ENOCOMPATPROTO:
            return ZLINK_REQUEST_PROTOCOL_ERROR;
        default:
            break;
    }
    switch (zlink::internal_errno::classify_request_completion (err_)) {
        case zlink::internal_errno::request_completion_class::success:
            return ZLINK_REQUEST_OK;
        case zlink::internal_errno::request_completion_class::timeout:
            return ZLINK_REQUEST_TIMED_OUT;
        case zlink::internal_errno::request_completion_class::target_not_found:
            return ZLINK_REQUEST_NOT_FOUND;
        case zlink::internal_errno::request_completion_class::terminated:
            return ZLINK_REQUEST_TERMINATED;
        case zlink::internal_errno::request_completion_class::protocol_error:
            return ZLINK_REQUEST_PROTOCOL_ERROR;
        case zlink::internal_errno::request_completion_class::unknown:
        default:
            return ZLINK_REQUEST_INTERNAL_ERROR;
    }
}
}
}

#endif
