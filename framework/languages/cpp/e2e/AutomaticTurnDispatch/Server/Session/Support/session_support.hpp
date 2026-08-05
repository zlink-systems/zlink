/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include <zlink/framework.hpp>

#include <string>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::session {

struct session_options_t
{
    std::string log_dir;
    std::string node_rid;
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string control_endpoint;
    std::string control_peer_endpoint;
    std::string spot_route_endpoint;
    std::string spot_route_peer_endpoint;
    std::string spot_router_endpoint;
    std::string spot_pub_endpoint;
    std::string stream_endpoint;

    static session_options_t bind (const configuration_section_t &section)
    {
        return {.log_dir = section.require ("logDir"),
                .node_rid = section.require ("nodeRid"),
                .http_endpoint = section.require ("httpEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .control_endpoint = section.require ("controlEndpoint"),
                .control_peer_endpoint = section.get ("controlPeerEndpoint").value_or (""),
                .spot_route_endpoint = section.require ("spotRouteEndpoint"),
                .spot_route_peer_endpoint = section.get ("spotRoutePeerEndpoint").value_or (""),
                .spot_router_endpoint = section.require ("spotRouterEndpoint"),
                .spot_pub_endpoint = section.require ("spotPubEndpoint"),
                .stream_endpoint = section.require ("streamEndpoint")};
    }
};

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::session
