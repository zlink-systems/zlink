/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_SOCKETS_SOCKET_ACCESS_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_SOCKETS_SOCKET_ACCESS_HPP_INCLUDED

#include <zlink/Contracts/Sockets/socket_contracts.hpp>
#include <Runtime/Native/socket_handle.hpp>

namespace zlink
{
namespace detail
{

struct socket_access_t
{
    static void *native_handle (socket_t &socket_) noexcept
    {
        return socket_._socket ? detail::native_handle (*socket_._socket) : nullptr;
    }

    static const void *native_handle (const socket_t &socket_) noexcept
    {
        return socket_._socket ? detail::native_handle (*socket_._socket) : nullptr;
    }
};

inline void *native_handle (socket_t &socket_) noexcept
{
    return socket_access_t::native_handle (socket_);
}

inline const void *native_handle (const socket_t &socket_) noexcept
{
    return socket_access_t::native_handle (socket_);
}

} // namespace detail
} // namespace zlink

#endif
