/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/route_connection_set.hpp"

#include "runtime/transport/endpoint_notation.hpp"

#include <utility>

namespace zlink::framework::detail
{

bool route_connection_set_t::connect (std::string endpoint)
{
    /* Write-time normalization (endpoint-notation policy §2.3): this is an
     * application-supplied RouteMesh connect endpoint. */
    return _manual_connections
      .emplace (runtime::transport::normalize_endpoint (endpoint), std::nullopt)
      .second;
}

bool route_connection_set_t::connect (zlink::routing_id_t peer_rid, std::string endpoint)
{
    auto [it, inserted] = _manual_connections.emplace (
      runtime::transport::normalize_endpoint (endpoint), std::nullopt);
    if (inserted || !it->second || *it->second != peer_rid) {
        it->second = std::move (peer_rid);
        return true;
    }
    return false;
}

bool route_connection_set_t::disconnect (const std::string &endpoint)
{
    return _manual_connections.erase (runtime::transport::normalize_endpoint (endpoint)) != 0;
}

bool route_connection_set_t::contains (const std::string &endpoint) const
{
    return _manual_connections.contains (endpoint);
}

std::vector<std::string> route_connection_set_t::list () const
{
    std::vector<std::string> endpoints;
    endpoints.reserve (_manual_connections.size ());
    for (const auto &[endpoint, _] : _manual_connections) {
        endpoints.push_back (endpoint);
    }
    return endpoints;
}

std::vector<route_connection_set_t::target_t> route_connection_set_t::targets () const
{
    std::vector<target_t> targets;
    targets.reserve (_manual_connections.size ());
    for (const auto &[endpoint, peer_rid] : _manual_connections) {
        targets.push_back (target_t{endpoint, peer_rid});
    }
    return targets;
}

} // namespace zlink::framework::detail
