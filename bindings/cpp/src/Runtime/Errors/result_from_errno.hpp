/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_ERRORS_RESULT_FROM_ERRNO_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_ERRORS_RESULT_FROM_ERRNO_HPP_INCLUDED

#include <zlink/Contracts/Errors/results.hpp>

#include <zlink.h>

#include <cerrno>

namespace zlink::detail
{

// Constructors without a Core result use the Core configuration errno table.
// A result returned by Core is used as it is.

inline bool errno_not_supported (int err_) noexcept
{
    return err_ == ENOTSUP
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
           || err_ == EOPNOTSUPP
#endif
      ;
}

inline config_result_t result_from_errno (config_result_t, int err_) noexcept
{
    if (errno_not_supported (err_))
        return config_result_t::not_supported;
    switch (err_) {
        case EFAULT:
            return config_result_t::invalid_handle;
        case EINVAL:
        case EMSGSIZE:
            return config_result_t::invalid_argument;
        case EBUSY:
        case ESHUTDOWN:
        case ESTALE:
        case EALREADY:
        case ENOTCONN:
        case ETIMEDOUT:
        case EPROTO:
            return config_result_t::invalid_state;
        case ENOENT:
            return config_result_t::not_found;
        case EEXIST:
            return config_result_t::conflict;
        case ENOBUFS:
            return config_result_t::buffer_too_small;
        default:
            return config_result_t::internal_error;
    }
}

} // namespace zlink::detail

#endif
