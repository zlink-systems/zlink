/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "transports/ws/ws_address.hpp"

#ifdef ZLINK_HAVE_WS

#include "utils/macros.hpp"
#include "utils/err.hpp"
#include "utils/ip.hpp"

#ifndef ZLINK_HAVE_WINDOWS
#include <sys/types.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ctype.h>
#endif

#include <cstring>
#include <cstdlib>
#include <string>

zlink::ws_address_t::ws_address_t () : _path ("/"), _port (0)
{
}

zlink::ws_address_t::ws_address_t (const sockaddr *sa_, socklen_t sa_len_) :
    _tcp_address (sa_, sa_len_), _path ("/"), _port (0)
{
    //  Extract port from the sockaddr
    if (sa_->sa_family == AF_INET) {
        const struct sockaddr_in *sin = reinterpret_cast<const struct sockaddr_in *> (sa_);
        _port = ntohs (sin->sin_port);
    } else if (sa_->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = reinterpret_cast<const struct sockaddr_in6 *> (sa_);
        _port = ntohs (sin6->sin6_port);
    }
}

zlink::ws_address_t::~ws_address_t ()
{
}

int zlink::ws_address_t::parse_url (const char *name_)
{
    //  Expected format: host:port/path or host:port (path defaults to "/")
    //  Examples:
    //    127.0.0.1:8080/zlink
    //    localhost:9000
    //    [::1]:8080/zlink (IPv6)
    //    *:8080 (wildcard bind)

    const char *pos = name_;

    //  Handle IPv6 address in brackets
    if (*pos == '[') {
        const char *bracket = strchr (pos, ']');
        if (!bracket) {
            errno = EINVAL;
            return -1;
        }
        _host.assign (pos + 1, bracket - pos - 1);
        pos = bracket + 1;
        if (*pos == ':')
            pos++;
        else if (*pos != '\0') {
            errno = EINVAL;
            return -1;
        }
    } else {
        //  IPv4 or hostname - find the colon that separates host from port
        const char *colon = strchr (pos, ':');
        if (!colon) {
            errno = EINVAL;
            return -1;
        }
        _host.assign (pos, colon - pos);
        pos = colon + 1;
    }

    //  Parse port number (handle wildcard '*' for ephemeral port)
    char *endptr;
    if (*pos == '*') {
        _port = 0; // Wildcard - let the system assign a port
        endptr = const_cast<char *> (pos + 1);
    } else {
        long port = strtol (pos, &endptr, 10);
        if (port < 0 || port > 65535) {
            errno = EINVAL;
            return -1;
        }
        _port = static_cast<uint16_t> (port);
    }

    //  Parse path (optional, defaults to "/")
    if (*endptr == '/') {
        _path = endptr;
    } else if (*endptr == '\0') {
        _path = "/";
    } else {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

int zlink::ws_address_t::resolve (const char *name_, bool local_, bool ipv6_)
{
    //  Parse the WebSocket URL to extract host, port, and path
    if (parse_url (name_) != 0)
        return -1;

    //  Build TCP address string for resolution (host:port)
    std::string tcp_addr;
    if (_host.find (':') != std::string::npos) {
        //  IPv6 address - need brackets for tcp_address resolution
        tcp_addr = "[" + _host + "]:" + std::to_string (_port);
    } else {
        tcp_addr = _host + ":" + std::to_string (_port);
    }

    //  Resolve the TCP address
    return _tcp_address.resolve (tcp_addr.c_str (), local_, ipv6_);
}

const sockaddr *zlink::ws_address_t::addr () const
{
    return _tcp_address.addr ();
}

socklen_t zlink::ws_address_t::addrlen () const
{
    return _tcp_address.addrlen ();
}

#if defined ZLINK_HAVE_WINDOWS
unsigned short zlink::ws_address_t::family () const
#else
sa_family_t zlink::ws_address_t::family () const
#endif
{
    return _tcp_address.family ();
}

int zlink::ws_address_t::to_string (std::string &addr_) const
{
    if (_tcp_address.family () != AF_INET && _tcp_address.family () != AF_INET6) {
        addr_.clear ();
        return -1;
    }

    //  Format: ws://host:port/path
    if (_tcp_address.family () == AF_INET6) {
        addr_ = "ws://[" + _host + "]:" + std::to_string (_port) + _path;
    } else {
        addr_ = "ws://" + _host + ":" + std::to_string (_port) + _path;
    }

    return 0;
}

#endif // ZLINK_HAVE_WS
