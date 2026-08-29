/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "core/endpoint.hpp"

#include <atomic>
#include <utility>

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

zlink::endpoint_uri_pair_t::endpoint_uri_pair_t (
  const endpoint_uri_pair_t &other_) :
    local (other_.local),
    remote (other_.remote),
    local_type (other_.local_type),
    connection_id (other_.connection_id.load ())
{
}

zlink::endpoint_uri_pair_t::endpoint_uri_pair_t (
  endpoint_uri_pair_t &&other_) :
    local (std::move (other_.local)),
    remote (std::move (other_.remote)),
    local_type (other_.local_type),
    connection_id (other_.connection_id.load ())
{
}

zlink::endpoint_uri_pair_t &zlink::endpoint_uri_pair_t::operator= (
  const endpoint_uri_pair_t &other_)
{
    if (this != &other_) {
        endpoint_uri_pair_t replacement (other_);
        swap (replacement);
    }
    return *this;
}

zlink::endpoint_uri_pair_t &zlink::endpoint_uri_pair_t::operator= (
  endpoint_uri_pair_t &&other_)
{
    if (this != &other_) {
        endpoint_uri_pair_t replacement (std::move (other_));
        swap (replacement);
    }
    return *this;
}

void zlink::endpoint_uri_pair_t::swap (endpoint_uri_pair_t &other_)
{
    local.swap (other_.local);
    remote.swap (other_.remote);
    std::swap (local_type, other_.local_type);
    const uint64_t this_connection_id = connection_id.load ();
    connection_id = other_.connection_id.load ();
    other_.connection_id = this_connection_id;
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
