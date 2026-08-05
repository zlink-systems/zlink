/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_ASIO_SOCKET_LIFECYCLE_HPP_INCLUDED
#define ZLINK_ASIO_SOCKET_LIFECYCLE_HPP_INCLUDED

#include <boost/system/error_code.hpp>

namespace zlink
{
template <typename socket_t, typename closed_fn_t>
inline void close_asio_socket_if_open (socket_t &socket_, closed_fn_t closed_fn_)
{
    if (!socket_.is_open ())
        return;

    const auto fd = socket_.native_handle ();
    boost::system::error_code ec;
    socket_.close (ec);
    closed_fn_ (fd);
}
}

#endif
