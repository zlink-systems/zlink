/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "transports/tls/wss_address.hpp"

#ifdef ZLINK_HAVE_WSS

#include <string>

zlink::wss_address_t::wss_address_t ()
{
}

zlink::wss_address_t::wss_address_t (const sockaddr *sa_, socklen_t sa_len_) :
    ws_address_t (sa_, sa_len_)
{
}

zlink::wss_address_t::~wss_address_t ()
{
}

int zlink::wss_address_t::to_string (std::string &addr_) const
{
    if (family () != AF_INET && family () != AF_INET6) {
        addr_.clear ();
        return -1;
    }

    //  Format: wss://host:port/path
    if (family () == AF_INET6) {
        addr_ = "wss://[" + host () + "]:" + std::to_string (port ()) + path ();
    } else {
        addr_ = "wss://" + host () + ":" + std::to_string (port ()) + path ();
    }

    return 0;
}

#endif // ZLINK_HAVE_WSS
