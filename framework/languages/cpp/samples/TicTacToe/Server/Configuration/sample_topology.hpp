/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/configuration.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace zlink::samples::tictactoe
{

using namespace framework;

inline std::string host_from_tcp_endpoint (const std::string &endpoint)
{
    const auto start = endpoint.rfind ("tcp://", 0) == 0 ? 6U : 0U;
    const auto colon = endpoint.rfind (':');
    if (colon == std::string::npos || colon <= start) {
        throw std::runtime_error ("TCP endpoint must use tcp://host:port");
    }
    return endpoint.substr (start, colon - start);
}

inline std::uint16_t port_from_tcp_endpoint (const std::string &endpoint)
{
    const auto colon = endpoint.rfind (':');
    if (colon == std::string::npos || colon + 1 >= endpoint.size ()) {
        throw std::runtime_error ("TCP endpoint must use tcp://host:port");
    }
    const auto port = std::stoul (endpoint.substr (colon + 1));
    if (port > 65535U) {
        throw std::runtime_error ("TCP endpoint port exceeds 65535");
    }
    return static_cast<std::uint16_t> (port);
}

struct sample_topology_t
{
    static sample_topology_t bind (const configuration_section_t &section)
    {
        sample_topology_t topology;
        topology.api_endpoint = section.get ("apiEndpoint").value_or (topology.api_endpoint);
        topology.api_a_endpoint = section.get ("apiAEndpoint").value_or (topology.api_a_endpoint);
        topology.api_b_endpoint = section.get ("apiBEndpoint").value_or (topology.api_b_endpoint);
        topology.api_http_endpoint =
          section.get ("apiHttpEndpoint").value_or (topology.api_http_endpoint);
        topology.api_a_http_endpoint =
          section.get ("apiAHttpEndpoint").value_or (topology.api_a_http_endpoint);
        topology.api_b_http_endpoint =
          section.get ("apiBHttpEndpoint").value_or (topology.api_b_http_endpoint);
        topology.play_endpoint = section.get ("playEndpoint").value_or (topology.play_endpoint);
        topology.play_a_endpoint =
          section.get ("playAEndpoint").value_or (topology.play_a_endpoint);
        topology.play_b_endpoint =
          section.get ("playBEndpoint").value_or (topology.play_b_endpoint);
        topology.play_a_route_endpoint =
          section.get ("playARouteEndpoint").value_or (topology.play_a_route_endpoint);
        topology.play_b_route_endpoint =
          section.get ("playBRouteEndpoint").value_or (topology.play_b_route_endpoint);
        topology.api_a_route_endpoint =
          section.get ("apiARouteEndpoint").value_or (topology.api_a_route_endpoint);
        topology.api_b_route_endpoint =
          section.get ("apiBRouteEndpoint").value_or (topology.api_b_route_endpoint);
        topology.play_spot_endpoint =
          section.get ("playSpotEndpoint").value_or (topology.play_spot_endpoint);
        topology.play_a_spot_endpoint =
          section.get ("playASpotEndpoint").value_or (topology.play_a_spot_endpoint);
        topology.play_b_spot_endpoint =
          section.get ("playBSpotEndpoint").value_or (topology.play_b_spot_endpoint);
        topology.play_spot_router_endpoint =
          section.get ("playSpotRouterEndpoint").value_or (topology.play_spot_router_endpoint);
        topology.play_a_spot_router_endpoint =
          section.get ("playASpotRouterEndpoint").value_or (topology.play_a_spot_router_endpoint);
        topology.play_b_spot_router_endpoint =
          section.get ("playBSpotRouterEndpoint").value_or (topology.play_b_spot_router_endpoint);
        topology.stream_endpoint =
          section.get ("streamEndpoint").value_or (topology.stream_endpoint);
        topology.play_a_stream_endpoint =
          section.get ("playAStreamEndpoint").value_or (topology.play_a_stream_endpoint);
        topology.play_b_stream_endpoint =
          section.get ("playBStreamEndpoint").value_or (topology.play_b_stream_endpoint);
        topology.api_node = section.get ("apiNode").value_or (topology.api_node);
        topology.play_node = section.get ("playNode").value_or (topology.play_node);
        // The sample owns its Redis via run_sample.sh/run_sample.ps1, which
        // provisions an isolated container; require the endpoint so a stray
        // direct run never silently falls back to a developer's local Redis.
        if (auto value = section.get ("redisEndpoint")) {
            topology.redis_endpoint = *value;
        } else {
            throw std::runtime_error (
              "redisEndpoint is required; run the sample via run_sample.sh/run_sample.ps1, "
              "which provisions an isolated Redis container.");
        }
        topology.redis_key_prefix =
          section.get ("redisKeyPrefix").value_or (topology.redis_key_prefix);
        topology.log_dir = section.require ("logDir");
        return topology;
    }

    std::string api_endpoint = "tcp://127.0.0.1:48103";
    std::string api_a_endpoint = "tcp://127.0.0.1:48103";
    std::string api_b_endpoint = "tcp://127.0.0.1:48123";
    std::string api_http_endpoint = "http://127.0.0.1:48113";
    std::string api_a_http_endpoint = "http://127.0.0.1:48113";
    std::string api_b_http_endpoint = "http://127.0.0.1:48124";
    std::string play_endpoint = "tcp://127.0.0.1:48104";
    std::string play_a_endpoint = "tcp://127.0.0.1:48104";
    std::string play_b_endpoint = "tcp://127.0.0.1:48114";
    std::string play_a_route_endpoint = "tcp://127.0.0.1:48115";
    std::string play_b_route_endpoint = "tcp://127.0.0.1:48116";
    std::string api_a_route_endpoint = "tcp://127.0.0.1:48117";
    std::string api_b_route_endpoint = "tcp://127.0.0.1:48118";
    std::string play_spot_endpoint = "tcp://127.0.0.1:48110";
    std::string play_a_spot_endpoint = "tcp://127.0.0.1:48110";
    std::string play_b_spot_endpoint = "tcp://127.0.0.1:48120";
    std::string play_spot_router_endpoint = "tcp://127.0.0.1:48111";
    std::string play_a_spot_router_endpoint = "tcp://127.0.0.1:48111";
    std::string play_b_spot_router_endpoint = "tcp://127.0.0.1:48121";
    std::string stream_endpoint = "tcp://127.0.0.1:48112";
    std::string play_a_stream_endpoint = "tcp://127.0.0.1:48112";
    std::string play_b_stream_endpoint = "tcp://127.0.0.1:48122";
    std::string log_dir = "logs";
    std::string api_node = "a";
    std::string play_node = "a";
    std::string redis_endpoint;
    std::string redis_key_prefix = "zlink:tictactoe-cpp:room:";

    std::string selected_api_endpoint () const
    {
        return api_node == "b" ? api_b_endpoint : api_a_endpoint;
    }

    std::string selected_api_http_endpoint () const
    {
        return api_node == "b" ? api_b_http_endpoint : api_a_http_endpoint;
    }

    std::string selected_play_endpoint () const
    {
        return play_node == "b" ? play_b_endpoint : play_a_endpoint;
    }

    /* TicTacToe 정본은 수동 endpoint scale-out을 보여 준다: channel client는 모든 상대
     * 역할 endpoint를 직접 지정한다(공통 sample spec §6). */
    std::vector<std::string> all_play_endpoints () const
    {
        return {play_a_endpoint, play_b_endpoint};
    }

    std::vector<std::string> all_api_endpoints () const
    {
        return {api_a_endpoint, api_b_endpoint};
    }

    std::string peer_play_endpoint () const
    {
        return play_node == "b" ? play_a_endpoint : play_b_endpoint;
    }

    std::string selected_play_route_endpoint () const
    {
        return play_node == "b" ? play_b_route_endpoint : play_a_route_endpoint;
    }

    std::string selected_api_route_endpoint () const
    {
        return api_node == "b" ? api_b_route_endpoint : api_a_route_endpoint;
    }

    std::vector<std::string> all_play_route_endpoints () const
    {
        return {play_a_route_endpoint, play_b_route_endpoint};
    }

    std::string peer_play_route_endpoint () const
    {
        return play_node == "b" ? play_a_route_endpoint : play_b_route_endpoint;
    }

    std::string selected_play_spot_endpoint () const
    {
        return play_node == "b" ? play_b_spot_endpoint : play_a_spot_endpoint;
    }

    std::string peer_play_spot_endpoint () const
    {
        return play_node == "b" ? play_a_spot_endpoint : play_b_spot_endpoint;
    }

    std::string selected_play_spot_router_endpoint () const
    {
        return play_node == "b" ? play_b_spot_router_endpoint : play_a_spot_router_endpoint;
    }

    std::string peer_play_spot_router_endpoint () const
    {
        return play_node == "b" ? play_a_spot_router_endpoint : play_b_spot_router_endpoint;
    }

    std::string selected_stream_endpoint () const
    {
        return play_node == "b" ? play_b_stream_endpoint : play_a_stream_endpoint;
    }

};

} // namespace zlink::samples::tictactoe
