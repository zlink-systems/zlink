/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/macros.hpp"
#include "core/address.hpp"
#include "core/ctx.hpp"
#include "utils/err.hpp"
#include "transports/tcp/tcp_address.hpp"
#include "transports/ws/ws_address.hpp"
#ifdef ZLINK_HAVE_WSS
#include "transports/tls/wss_address.hpp"
#endif
#include "transports/ipc/ipc_address.hpp"

#include <string>
#include <sstream>

zlink::address_t::address_t (const std::string &protocol_,
                             const std::string &address_,
                             ctx_t *parent_) :
    protocol (protocol_), address (address_), parent (parent_)
{
    resolved.dummy = NULL;
}

zlink::address_t::~address_t ()
{
    if (protocol == protocol_name::tcp
#ifdef ZLINK_HAVE_TLS
        || protocol == protocol_name::tls
#endif
    ) {
        LIBZLINK_DELETE (resolved.tcp_addr);
    }
#if defined ZLINK_HAVE_IPC
    else if (protocol == protocol_name::ipc) {
        LIBZLINK_DELETE (resolved.ipc_addr);
    }
#endif
#ifdef ZLINK_HAVE_WS
    else if (protocol == protocol_name::ws) {
        LIBZLINK_DELETE (resolved.ws_addr);
    }
#endif
#ifdef ZLINK_HAVE_WSS
    else if (protocol == protocol_name::wss) {
        LIBZLINK_DELETE (resolved.wss_addr);
    }
#endif
}

int zlink::address_t::to_string (std::string &addr_) const
{
    if ((protocol == protocol_name::tcp
#ifdef ZLINK_HAVE_TLS
         || protocol == protocol_name::tls
#endif
         )
        && resolved.tcp_addr) {
        const int rc = resolved.tcp_addr->to_string (addr_);
#ifdef ZLINK_HAVE_TLS
        if (rc == 0 && protocol == protocol_name::tls && addr_.compare (0, 6, "tcp://") == 0) {
            addr_.replace (0, 6, "tls://");
        }
#endif
        return rc;
    }
#if defined ZLINK_HAVE_IPC
    if (protocol == protocol_name::ipc && resolved.ipc_addr)
        return resolved.ipc_addr->to_string (addr_);
#endif

    if (!protocol.empty () && !address.empty ()) {
        std::stringstream s;
        s << protocol << "://" << address;
        addr_ = s.str ();
        return 0;
    }
    addr_.clear ();
    return -1;
}

zlink::zlink_socklen_t
zlink::get_socket_address (fd_t fd_, socket_end_t socket_end_, sockaddr_storage *ss_)
{
    zlink_socklen_t sl = static_cast<zlink_socklen_t> (sizeof (*ss_));

    const int rc = socket_end_ == socket_end_local
                     ? getsockname (fd_, reinterpret_cast<struct sockaddr *> (ss_), &sl)
                     : getpeername (fd_, reinterpret_cast<struct sockaddr *> (ss_), &sl);

    return rc != 0 ? 0 : sl;
}
