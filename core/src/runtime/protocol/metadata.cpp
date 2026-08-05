/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "protocol/metadata.hpp"

//  Draft API message property names (kept for internal use)
#define ZLINK_MSG_PROPERTY_ROUTING_ID "Routing-Id"
#define ZLINK_MSG_PROPERTY_SOCKET_TYPE "Socket-Type"
#define ZLINK_MSG_PROPERTY_USER_ID "User-Id"
#define ZLINK_MSG_PROPERTY_PEER_ADDRESS "Peer-Address"

zlink::metadata_t::metadata_t (const dict_t &dict_) : _ref_cnt (1), _dict (dict_)
{
}

const char *zlink::metadata_t::get (const std::string &property_) const
{
    const dict_t::const_iterator it = _dict.find (property_);
    if (it == _dict.end ())
        return NULL;
    return it->second.c_str ();
}

void zlink::metadata_t::add_ref ()
{
    _ref_cnt.add (1);
}

bool zlink::metadata_t::drop_ref ()
{
    return !_ref_cnt.sub (1);
}
