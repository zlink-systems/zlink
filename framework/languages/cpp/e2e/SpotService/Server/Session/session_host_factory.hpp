/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "Handlers/session_session_handlers.hpp"
#include "../Shared/Endpoints/evidence_endpoint.hpp"
#include "../Shared/Handlers/channel_control_ping_handler.hpp"
#include "../Shared/Support/codecs.hpp"
#include "../Shared/Support/configuration.hpp"
#include "../Shared/Support/location_store.hpp"

#include <zlink/framework.hpp>

#include <memory>
#include <string>
#include <vector>

struct session_options_t
{
    std::string log_dir;
    std::string node_rid;
    std::string route_endpoint;
    std::vector<std::string> route_peer_endpoints;
    std::string spot_router_endpoint;
    std::vector<std::string> spot_peer_endpoints;
    std::string pubsub_endpoint;
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string stream_endpoint;
    std::string tls_stream_endpoint;
    std::string tls_cert_path;
    std::string tls_key_path;
    bool route_mesh_enabled;

    static session_options_t bind (const zlink::framework::configuration_section_t &section)
    {
        return {.log_dir = section.require ("logDir"),
                .node_rid = section.get ("nodeRid").value_or ("session-a"),
                .route_endpoint = section.require ("routeEndpoint"),
                .route_peer_endpoints =
                  split_endpoints (section.get ("routePeerEndpoints").value_or ("")),
                .spot_router_endpoint = section.require ("spotRouterEndpoint"),
                .spot_peer_endpoints =
                  split_endpoints (section.get ("spotPeerEndpoints").value_or ("")),
                .pubsub_endpoint = section.require ("pubsubEndpoint"),
                .http_endpoint = section.require ("httpEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .stream_endpoint = section.get ("streamEndpoint").value_or (""),
                .tls_stream_endpoint = section.get ("tls.streamEndpoint").value_or (""),
                .tls_cert_path = section.get ("tls.certPath").value_or (""),
                .tls_key_path = section.get ("tls.keyPath").value_or (""),
                .route_mesh_enabled =
                  section.get ("routeMeshEnabled").value_or ("true") == "true"};
    }
};

inline int run_session_server (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    load_spot_service_config (app, argc, argv, "session");
    const auto config = app.config ().bind_required<session_options_t> ("e2e");
    const auto &log_dir = config.log_dir;
    const auto &node_rid = config.node_rid;
    const auto &route_endpoint = config.route_endpoint;
    const auto &route_peer_endpoints = config.route_peer_endpoints;
    const auto &spot_router_endpoint = config.spot_router_endpoint;
    const auto &spot_peer_endpoints = config.spot_peer_endpoints;
    const auto &pubsub_endpoint = config.pubsub_endpoint;
    const auto &http_endpoint = config.http_endpoint;
    const auto &redis_endpoint = config.redis_endpoint;
    const auto &redis_key_prefix = config.redis_key_prefix;
    const auto &stream_endpoint = config.stream_endpoint;
    const auto &tls_stream_endpoint = config.tls_stream_endpoint;
    const auto &tls_cert_path = config.tls_cert_path;
    const auto &tls_key_path = config.tls_key_path;
    const auto route_mesh_enabled = config.route_mesh_enabled;

    app.logging ()
      .use_file (log_dir + "/" + node_rid + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        auto state = std::make_unique<scenario_state_t> (node_rid);
        options.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (log_dir + "/" + node_rid + "-flow.log")
          .trace_label ("cpp-sm-" + node_rid);
        options.services ().add_singleton<scenario_state_t> (std::move (state));
        configure_codecs (options.codecs ());
        add_redis_location_store (options, redis_endpoint, redis_key_prefix);

        if (route_mesh_enabled) {
            auto route = options.add_route_mesh (e2e::route_channel);
            route.listen (route_endpoint)
              .set_routing_id (zlink::routing_id_t::from (node_rid))
              .channel_name (e2e::route_channel);
            for (const auto &peer : route_peer_endpoints)
                route.peer_connections ().connect (peer);
        }
        auto spot = options.add_route_mesh (e2e::spot_mesh);
        spot.listen (spot_router_endpoint)
          .set_routing_id (zlink::routing_id_t::from (node_rid))
          .channel_name (e2e::spot_mesh);
        for (const auto &peer : spot_peer_endpoints)
            spot.peer_connections ().connect (peer);
        if (!stream_endpoint.empty ()) {
            options.add_stream_node ("spot-service-stream")
              .bind (stream_endpoint)
              .register_session<stream_session_t> ();
        }
        if (!tls_stream_endpoint.empty ()) {
            options.add_stream_node ("spot-service-tls-stream")
              .bind (tls_stream_endpoint)
              .set_tls_server (tls_cert_path, tls_key_path)
              .register_session<stream_session_t> ();
        }
        options.http ()
          .listen (http_endpoint)
          .map_health ("/health")
          .map_get<evidence_handler_t> ("/evidence")
          .map_post<evidence_wait_handler_t> ("/evidence/wait")
          .map_post<channel_control_ping_route_handler_t> ("/channel/control-ping")
          .map_post<shutdown_handler_t> ("/shutdown")
          .map_post<crash_handler_t> ("/crash");
    });
    return app.run (argc, argv);
}
