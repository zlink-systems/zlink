/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "core/endpoint.hpp"

#include <atomic>

namespace
{
std::atomic<uint64_t> next_connection_id (1);
}

uint64_t zlink::allocate_connection_id ()
{
    return next_connection_id.fetch_add (1, std::memory_order_relaxed);
}

zlink::endpoint_uri_pair_t::endpoint_uri_pair_t () :
    local_type (endpoint_type_none),
    connection_id (0)
{
}

zlink::endpoint_uri_pair_t::endpoint_uri_pair_t (
  const std::string &local_,
  const std::string &remote_,
  endpoint_type_t local_type_) :
    local (local_),
    remote (remote_),
    local_type (local_type_),
    connection_id (allocate_connection_id ())
{
}

zlink::endpoint_uri_pair_t
zlink::make_unconnected_connect_endpoint_pair (const std::string &endpoint_)
{
    return endpoint_uri_pair_t (std::string (), endpoint_, endpoint_type_connect);
}

zlink::endpoint_uri_pair_t zlink::make_unconnected_bind_endpoint_pair (const std::string &endpoint_)
{
    return endpoint_uri_pair_t (endpoint_, std::string (), endpoint_type_bind);
}
