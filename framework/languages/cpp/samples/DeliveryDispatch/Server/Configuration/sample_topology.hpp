/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <stdexcept>
#include <string>

namespace zlink::samples::deliverydispatch
{

/* Topology는 설정 파일의 `sample.topology` 절이 정본이다. 값이 빠지면 framework runtime을
 * 시작하기 전에 실패한다(공통 정책 sample-e2e-configuration-policy.ko.md §2). */
struct sample_topology_t
{
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string dispatch_api_http_url;
    std::string dispatch_route_endpoint;
    std::string dispatch_spot_router_endpoint;
    std::string dispatch_spot_endpoint;
    std::string tracking_route_endpoint;
    std::string tracking_spot_router_endpoint;
    std::string tracking_spot_endpoint;
    std::string customer_stream_endpoint;
    std::string customer_spot_router_endpoint;
    std::string customer_spot_endpoint;
    std::string courier_stream_endpoint;
    std::string courier_session_spot_router_endpoint;
    std::string courier_session_spot_endpoint;
    std::string courier_actor_node_1_route_endpoint;
    std::string courier_actor_node_1_router_endpoint;
    std::string courier_actor_node_1_endpoint;
    std::string courier_actor_node_2_route_endpoint;
    std::string courier_actor_node_2_router_endpoint;
    std::string courier_actor_node_2_endpoint;

    static sample_topology_t bind (const zlink::framework::configuration_section_t &section)
    {
        sample_topology_t topology;
        topology.redis_endpoint = section.require ("redisEndpoint");
        topology.redis_key_prefix = section.require ("redisKeyPrefix");
        topology.dispatch_api_http_url = section.require ("dispatchApiHttpUrl");
        topology.dispatch_route_endpoint = section.require ("dispatchRouteEndpoint");
        topology.dispatch_spot_router_endpoint = section.require ("dispatchSpotRouterEndpoint");
        topology.dispatch_spot_endpoint = section.require ("dispatchSpotEndpoint");
        topology.tracking_route_endpoint = section.require ("trackingRouteEndpoint");
        topology.tracking_spot_router_endpoint = section.require ("trackingSpotRouterEndpoint");
        topology.tracking_spot_endpoint = section.require ("trackingSpotEndpoint");
        topology.customer_stream_endpoint = section.require ("customerStreamEndpoint");
        topology.customer_spot_router_endpoint = section.require ("customerSpotRouterEndpoint");
        topology.customer_spot_endpoint = section.require ("customerSpotEndpoint");
        topology.courier_stream_endpoint = section.require ("courierStreamEndpoint");
        topology.courier_session_spot_router_endpoint =
          section.require ("courierSessionSpotRouterEndpoint");
        topology.courier_session_spot_endpoint = section.require ("courierSessionSpotEndpoint");
        topology.courier_actor_node_1_route_endpoint =
          section.require ("courierActorNode1RouteEndpoint");
        topology.courier_actor_node_1_router_endpoint =
          section.require ("courierActorNode1RouterEndpoint");
        topology.courier_actor_node_1_endpoint = section.require ("courierActorNode1Endpoint");
        topology.courier_actor_node_2_route_endpoint =
          section.require ("courierActorNode2RouteEndpoint");
        topology.courier_actor_node_2_router_endpoint =
          section.require ("courierActorNode2RouterEndpoint");
        topology.courier_actor_node_2_endpoint = section.require ("courierActorNode2Endpoint");
        return topology;
    }
};

inline int port_from_http_url (const std::string &url)
{
    const auto colon = url.rfind (':');
    if (colon == std::string::npos) {
        return 80;
    }
    return std::stoi (url.substr (colon + 1));
}

inline std::string host_from_tcp_endpoint (const std::string &endpoint)
{
    const auto start = endpoint.rfind ("tcp://", 0) == 0 ? 6U : 0U;
    const auto colon = endpoint.rfind (':');
    if (colon == std::string::npos || colon <= start) {
        throw std::runtime_error ("TCP endpoint must use tcp://host:port");
    }
    return endpoint.substr (start, colon - start);
}

} // namespace zlink::samples::deliverydispatch
