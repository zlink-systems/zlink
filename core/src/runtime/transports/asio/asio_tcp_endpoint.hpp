/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_ASIO_TCP_ENDPOINT_HPP_INCLUDED
#define ZLINK_ASIO_TCP_ENDPOINT_HPP_INCLUDED

#include "utils/config.hpp"

#include <boost/asio/ip/tcp.hpp>

#include <cstring>

#ifndef ZLINK_HAVE_WINDOWS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace zlink
{
inline boost::asio::ip::tcp::endpoint asio_tcp_endpoint_from_sockaddr (
  const struct sockaddr *addr_)
{
    if (addr_->sa_family == AF_INET) {
        const struct sockaddr_in *sin = reinterpret_cast<const struct sockaddr_in *> (addr_);
        return boost::asio::ip::tcp::endpoint (
          boost::asio::ip::address_v4 (ntohl (sin->sin_addr.s_addr)), ntohs (sin->sin_port));
    }

    const struct sockaddr_in6 *sin6 = reinterpret_cast<const struct sockaddr_in6 *> (addr_);
    boost::asio::ip::address_v6::bytes_type bytes;
    std::memcpy (bytes.data (), sin6->sin6_addr.s6_addr, bytes.size ());
    return boost::asio::ip::tcp::endpoint (
      boost::asio::ip::address_v6 (bytes, sin6->sin6_scope_id), ntohs (sin6->sin6_port));
}

inline boost::asio::ip::tcp asio_tcp_protocol_for_endpoint (
  const boost::asio::ip::tcp::endpoint &endpoint_)
{
    return endpoint_.address ().is_v6 () ? boost::asio::ip::tcp::v6 ()
                                         : boost::asio::ip::tcp::v4 ();
}

inline void asio_tcp_endpoint_to_sockaddr (const boost::asio::ip::tcp::endpoint &endpoint_,
                                           struct sockaddr_storage *storage_,
                                           socklen_t *storage_len_)
{
    if (!storage_ || !storage_len_)
        return;

    std::memset (storage_, 0, sizeof (*storage_));
    if (endpoint_.address ().is_v4 ()) {
        struct sockaddr_in *sin = reinterpret_cast<struct sockaddr_in *> (storage_);
        sin->sin_family = AF_INET;
        sin->sin_port = htons (endpoint_.port ());
        sin->sin_addr.s_addr = htonl (endpoint_.address ().to_v4 ().to_uint ());
        *storage_len_ = sizeof (struct sockaddr_in);
        return;
    }

    struct sockaddr_in6 *sin6 = reinterpret_cast<struct sockaddr_in6 *> (storage_);
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = htons (endpoint_.port ());
    const boost::asio::ip::address_v6::bytes_type bytes =
      endpoint_.address ().to_v6 ().to_bytes ();
    std::memcpy (sin6->sin6_addr.s6_addr, bytes.data (), bytes.size ());
    sin6->sin6_scope_id = endpoint_.address ().to_v6 ().scope_id ();
    *storage_len_ = sizeof (struct sockaddr_in6);
}
}

#endif
