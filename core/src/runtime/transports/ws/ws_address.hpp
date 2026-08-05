/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_WS_ADDRESS_HPP_INCLUDED__
#define __ZLINK_WS_ADDRESS_HPP_INCLUDED__

#include "platform.hpp"

#ifdef ZLINK_HAVE_WS

#include "transports/tcp/tcp_address.hpp"
#include <string>

namespace zlink
{

//  WebSocket address class for ws:// protocol
//  URL format: ws://host:port/path or host:port/path (protocol stripped by caller)
//
//  This class parses WebSocket URLs and resolves them to underlying
//  TCP addresses. It stores the host, port, and path components needed
//  for WebSocket handshake.
class ws_address_t
{
  public:
    ws_address_t ();
    ws_address_t (const sockaddr *sa_, socklen_t sa_len_);
    ~ws_address_t ();

    //  Resolve the address (parse URL and resolve host)
    //  name_ format: "host:port/path" or "host:port" (path defaults to "/")
    //  local_: true for bind addresses, false for connect addresses
    //  ipv6_: true to allow IPv6 resolution
    //  Returns 0 on success, -1 on error (sets errno)
    int resolve (const char *name_, bool local_, bool ipv6_);

    //  Get the underlying TCP address for socket operations
    const sockaddr *addr () const;
    socklen_t addrlen () const;

#if defined ZLINK_HAVE_WINDOWS
    unsigned short family () const;
#else
    sa_family_t family () const;
#endif

    //  Get WebSocket-specific components
    const std::string &host () const { return _host; }
    const std::string &path () const { return _path; }
    uint16_t port () const { return _port; }

    //  Format address as string (ws://host:port/path)
    int to_string (std::string &addr_) const;

  protected:
    //  Parse URL into host, port, and path components
    int parse_url (const char *name_);

  private:
    //  Underlying TCP address for the resolved host:port
    tcp_address_t _tcp_address;

    //  WebSocket-specific URL components
    std::string _host;
    std::string _path;
    uint16_t _port;
};

} // namespace zlink

#endif // ZLINK_HAVE_WS

#endif // __ZLINK_WS_ADDRESS_HPP_INCLUDED__
