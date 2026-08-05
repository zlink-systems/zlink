/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include <zlink/framework.hpp>

#include <cstdint>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::e2e::runtime_monitoring::service
{

struct tcp_endpoint_t
{
    std::string host;
    std::uint16_t port = 0;
};

inline tcp_endpoint_t parse_tcp_endpoint (const std::string &endpoint)
{
    constexpr std::string_view prefix = "tcp://";
    if (!endpoint.starts_with (prefix))
        throw std::runtime_error ("RuntimeMonitoring endpoint must use tcp://: " + endpoint);
    const auto separator = endpoint.rfind (':');
    if (separator <= prefix.size () || separator + 1 >= endpoint.size ())
        throw std::runtime_error ("RuntimeMonitoring endpoint must contain host and port: " + endpoint);
    const auto port = std::stoul (endpoint.substr (separator + 1));
    if (port == 0 || port > 65535)
        throw std::runtime_error ("RuntimeMonitoring endpoint port is out of range: " + endpoint);
    return {.host = endpoint.substr (prefix.size (), separator - prefix.size ()),
            .port = static_cast<std::uint16_t> (port)};
}

struct service_options_t
{
    std::string rid;
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string channel_endpoint;
    std::string spot_router_endpoint;
    std::string spot_pub_endpoint;
    std::string evidence_file;
    std::string monitor_profile;
    std::string log_dir;
    std::string mesh_endpoint;
    std::vector<std::string> mesh_peer_endpoints;

    static service_options_t bind (const configuration_section_t &section)
    {
        std::vector<std::string> mesh_peers;
        std::stringstream peers (
          section.get ("meshPeerEndpoints").value_or (""));
        for (std::string endpoint; std::getline (peers, endpoint, ',');) {
            if (!endpoint.empty ())
                mesh_peers.push_back (std::move (endpoint));
        }
        return {.rid = section.require ("rid"),
                .http_endpoint = section.require ("httpEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .channel_endpoint = section.require ("channelEndpoint"),
                .spot_router_endpoint = section.require ("spotRouterEndpoint"),
                .spot_pub_endpoint = section.require ("spotPubEndpoint"),
                .evidence_file = section.require ("evidenceFile"),
                .monitor_profile = section.require ("monitorProfile"),
                .log_dir = section.require ("logDir"),
                .mesh_endpoint = section.require ("meshEndpoint"),
                .mesh_peer_endpoints = std::move (mesh_peers)};
    }
};

inline service_options_t read_service_options (app_t &app, int argc, char **argv)
{
    app.config ().load_cli (argc, argv);
    const auto path = app.config ().model ().get ("config");
    if (!path) {
        throw std::runtime_error ("RuntimeMonitoring service requires --config=<path>");
    }
    app.config ().load_json (*path);
    return app.config ().bind_required<service_options_t> ("e2e");
}

} // namespace zlink::framework::e2e::runtime_monitoring::service
