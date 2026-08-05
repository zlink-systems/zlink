/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ADDRESS_HPP_INCLUDED__
#define __ZLINK_ADDRESS_HPP_INCLUDED__

#include "utils/fd.hpp"

#include <string>

#ifndef ZLINK_HAVE_WINDOWS
#include <sys/socket.h>
#else
#include <ws2tcpip.h>
#endif

namespace zlink
{
class ctx_t;
class tcp_address_t;
class ws_address_t;
#ifdef ZLINK_HAVE_WSS
class wss_address_t;
#endif
#if defined ZLINK_HAVE_IPC
class ipc_address_t;
#endif

namespace protocol_name
{
static const char inproc[] = "inproc";
static const char tcp[] = "tcp";
#ifdef ZLINK_HAVE_WS
static const char ws[] = "ws";
#endif
#ifdef ZLINK_HAVE_WSS
static const char wss[] = "wss";
#endif
#ifdef ZLINK_HAVE_TLS
static const char tls[] = "tls";
#endif
#if defined ZLINK_HAVE_IPC
static const char ipc[] = "ipc";
#endif
}

struct address_t
{
    address_t (const std::string &protocol_, const std::string &address_, ctx_t *parent_);

    ~address_t ();

    const std::string protocol;
    const std::string address;
    ctx_t *const parent;

    //  Protocol specific resolved address
    //  All members must be pointers to allow for consistent initialization
    union
    {
        void *dummy;
        tcp_address_t *tcp_addr;
#ifdef ZLINK_HAVE_WS
        ws_address_t *ws_addr;
#endif
#ifdef ZLINK_HAVE_WSS
        wss_address_t *wss_addr;
#endif
#if defined ZLINK_HAVE_IPC
        ipc_address_t *ipc_addr;
#endif
    } resolved;

    int to_string (std::string &addr_) const;
};

#if defined(ZLINK_HAVE_HPUX) || defined(ZLINK_HAVE_VXWORKS) || defined(ZLINK_HAVE_WINDOWS)
typedef int zlink_socklen_t;
#else
typedef socklen_t zlink_socklen_t;
#endif

enum socket_end_t
{
    socket_end_local,
    socket_end_remote
};

zlink_socklen_t get_socket_address (fd_t fd_, socket_end_t socket_end_, sockaddr_storage *ss_);

template <typename T> std::string get_socket_name (fd_t fd_, socket_end_t socket_end_)
{
    struct sockaddr_storage ss;
    const zlink_socklen_t sl = get_socket_address (fd_, socket_end_, &ss);
    if (sl == 0) {
        return std::string ();
    }

    const T addr (reinterpret_cast<struct sockaddr *> (&ss), sl);
    std::string address_string;
    addr.to_string (address_string);
    return address_string;
}
}

#endif
