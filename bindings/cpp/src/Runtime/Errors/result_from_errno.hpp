/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_ERRORS_RESULT_FROM_ERRNO_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_ERRORS_RESULT_FROM_ERRNO_HPP_INCLUDED

#include <zlink/Contracts/Errors/results.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <zlink.h>

#include <cerrno>

namespace zlink::detail
{

// The binding's only errno -> result projection, one table per result family.
// Each table is Core's from_errno (core/src/api/core/config_result_internal.hpp,
// close_result_internal.hpp and core/src/api/message/{recv,bind,connect,
// submit}_result_internal.hpp). The binding uses it only when a call ends
// without a Core result: a closed handle that does not reach Core, or a
// binding-local failure. A result Core returned is used as it is.

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
        case 0:
            return config_result_t::ok;
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

inline close_result_t result_from_errno (close_result_t, int err_) noexcept
{
    switch (err_) {
        case 0:
            return close_result_t::ok;
        case EBUSY:
        case EDEADLK:
            return close_result_t::busy;
        case ESHUTDOWN:
            return close_result_t::shutdown;
        case EFAULT:
        case ESTALE:
            return close_result_t::invalid_handle;
        default:
            return close_result_t::internal_error;
    }
}

inline recv_result_t result_from_errno (recv_result_t, int err_) noexcept
{
    if (errno_not_supported (err_))
        return recv_result_t::not_supported;
    switch (err_) {
        case 0:
            return recv_result_t::ok;
        case EAGAIN:
        case ETIMEDOUT:
            return recv_result_t::no_data;
        case EBUSY:
            return recv_result_t::busy;
        case ETERM:
            return recv_result_t::terminated;
        case EFAULT:
            return recv_result_t::invalid_handle;
        case ENOBUFS:
            return recv_result_t::buffer_too_small;
        case EINVAL:
        case ESTALE:
        case ESHUTDOWN:
            return recv_result_t::invalid_state;
        default:
            return recv_result_t::internal_error;
    }
}

inline bind_result_t result_from_errno (bind_result_t, int err_) noexcept
{
    if (errno_not_supported (err_) || err_ == EPROTONOSUPPORT)
        return bind_result_t::not_supported;
    switch (err_) {
        case 0:
            return bind_result_t::ok;
        case EINVAL:
            return bind_result_t::invalid_argument;
        case EADDRINUSE:
            return bind_result_t::addr_in_use;
        case EFAULT:
            return bind_result_t::invalid_handle;
        default:
            return bind_result_t::internal_error;
    }
}

inline connect_result_t result_from_errno (connect_result_t, int err_) noexcept
{
    if (errno_not_supported (err_) || err_ == EPROTONOSUPPORT)
        return connect_result_t::not_supported;
    switch (err_) {
        case 0:
            return connect_result_t::ok;
        case EINVAL:
            return connect_result_t::invalid_argument;
        case EFAULT:
            return connect_result_t::invalid_handle;
        case ENOENT:
            return connect_result_t::not_found;
        case EADDRINUSE:
        case EEXIST:
        case ESTALE:
            return connect_result_t::conflict;
        case EBUSY:
        case ESHUTDOWN:
            return connect_result_t::busy;
        case EACCES:
            return connect_result_t::auth_failed;
        default:
            return connect_result_t::internal_error;
    }
}

inline submit_result_t result_from_errno (submit_result_t, int err_) noexcept
{
    if (errno_not_supported (err_))
        return submit_result_t::not_supported;
    switch (err_) {
        case 0:
            return submit_result_t::ok;
        case EAGAIN:
        case ETIMEDOUT:
        case ENOBUFS:
            return submit_result_t::backpressured;
        case ENOTCONN:
        case EHOSTUNREACH:
            return submit_result_t::not_connected;
        case ECONNREFUSED:
        case EACCES:
        case EPROTOTYPE:
            return submit_result_t::not_admitted;
        case ENOENT:
            return submit_result_t::not_found;
        case ETERM:
        case ESHUTDOWN:
            return submit_result_t::terminated;
        case EFAULT:
            return submit_result_t::invalid_handle;
        case EINVAL:
        case EMSGSIZE:
            return submit_result_t::invalid_argument;
        case EFSM:
        case EBUSY:
        case ESTALE:
        case EALREADY:
            return submit_result_t::invalid_state;
        case EDEADLK:
        case EPERM:
        case EMTHREAD:
            return submit_result_t::thread_violation;
        case EOVERFLOW:
            return submit_result_t::seq_exhausted;
        case ENOMEM:
            return submit_result_t::out_of_memory;
        default:
            return submit_result_t::internal_error;
    }
}

} // namespace zlink::detail

#endif
