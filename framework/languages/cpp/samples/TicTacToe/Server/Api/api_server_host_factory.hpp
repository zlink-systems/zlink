/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Configuration/sample_configuration.hpp"
#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_topology.hpp"
#include "../../Shared/Contracts/messages.hpp"
#include "../host_support.hpp"
#include "../sample_log_dir.hpp"
#include "Handlers/authenticate_player_handler.hpp"
#include "Handlers/create_game_http_handler.hpp"
#include "Handlers/route_ready_handler.hpp"

#include <zlink/locations/redis.hpp>

#include <memory>

namespace zlink::samples::tictactoe
{

using namespace framework;

inline constexpr const char *sample_log_file = "tictactoe-server.log";

class api_server_host_factory_t
{
  public:
    static app_t build (const sample_topology_t &topology)
    {
        auto app = app_t::create ();
        configure (app, topology);
        return app;
    }

    static app_t &configure (app_t &app, const sample_topology_t &topology)
    {
        app.logging ().use_file (flow_log_path (topology.log_dir, "api-" + topology.api_node));
        auto &options = app.add_zlink_framework ();
        options.configure_dispatch ().message_flow (message_flow_log_mode_t::normal);

        options.add_location_store<redis::redis_location_store_t> ()
          .set_connection_string (topology.redis_endpoint)
          .set_key_prefix (topology.redis_key_prefix + "location:");
        options.services ().add_singleton<sample_topology_t> (
          std::make_unique<sample_topology_t> (topology));

        options.add_client_server_channel (sample_names_t::api_channel)
          .server ()
          .set_bind_host (host_from_tcp_endpoint (topology.selected_api_endpoint ()))
          .set_advertise_host (host_from_tcp_endpoint (topology.selected_api_endpoint ()))
          .listen (port_from_tcp_endpoint (topology.selected_api_endpoint ()))
          .add_handler_group ("api");

        options.http ()
          .listen (topology.selected_api_http_endpoint ())
          .map_get<route_ready_handler_t> ("/ready")
          .map_post<create_game_http_handler_t> ("/games");

        /* API는 Object Client RouteMesh로 Play Object Server에 연결한다. 새 room의
             * owner는 Framework가 선택하며 API는 NodeRid를 지정하지 않는다. */
        auto mesh = options.add_route_mesh (sample_names_t::game_spot_node);
        mesh.set_routing_id (zlink::routing_id_t::from ("tictactoe-api-" + topology.api_node))
          .listen (topology.selected_api_route_endpoint ());
        mesh.objects ().client ();
        mesh.peer_connections ().connect (zlink::routing_id_t::from ("tictactoe-play-a"),
                                          topology.play_a_route_endpoint);
        mesh.peer_connections ().connect (zlink::routing_id_t::from ("tictactoe-play-b"),
                                          topology.play_b_route_endpoint);

        options.handlers ().group ("api").add<authenticate_player_handler_t> ();
        app.add_hosted_service (
          std::make_unique<api_http_readiness_service_t> ("api-" + topology.api_node));
        app.add_hosted_service (std::make_unique<api_spot_route_readiness_service_t> (
          sample_names_t::game_spot_node, "api-" + topology.api_node));
        return app;
    }
};

} // namespace zlink::samples::tictactoe
