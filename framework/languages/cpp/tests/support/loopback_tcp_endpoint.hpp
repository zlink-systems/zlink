/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <string>

namespace zlink::framework::tests
{
inline std::uint16_t reserve_loopback_tcp_port ()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor reservation (
      io, {boost::asio::ip::address_v4::loopback (), 0});
    return reservation.local_endpoint ().port ();
}

inline std::string reserve_loopback_tcp_endpoint ()
{
    return "tcp://127.0.0.1:"
           + std::to_string (reserve_loopback_tcp_port ());
}
}
