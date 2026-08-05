/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Configuration/sample_configuration.hpp"
#include "../Configuration/location_store.hpp"
#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_topology.hpp"
#include "../../Shared/Contracts/messages.hpp"
#include "../host_support.hpp"
#include "../sample_log_dir.hpp"
#include "Handlers/authenticate_player_handler.hpp"
#include "Handlers/create_game_http_handler.hpp"
#include "Handlers/route_ready_handler.hpp"

#include <memory>

namespace zlink::samples::tictactoe
{

using namespace framework;

inline constexpr const char *sample_log_file = "tictactoe-server.log";

class api_server_host_factory_t
{
  public:
    static app_t build (const sample_topology_t &topology, bool auto_stop = true)
    {
        auto app = app_t::create ();
        configure (app, topology, auto_stop);
        return app;
    }

    static app_t &configure (app_t &app, const sample_topology_t &topology, bool auto_stop = true)
    {
        app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
            options.configure_dispatch ()
              .message_flow (message_flow_log_mode_t::key_transitions)
              .trace_log_file (
                flow_log_path (topology.log_dir, "api-" + topology.api_node))
              .trace_label ("tictactoe-api-" + topology.api_node);

            add_sample_location_store (options, topology);
            options.services ().add_singleton<sample_topology_t> (
              std::make_unique<sample_topology_t> (topology));

            options.add_client_server_channel (sample_names_t::api_channel)
              .server ()
              .set_bind_host (
                host_from_tcp_endpoint (topology.selected_api_endpoint ()))
              .set_advertise_host (
                host_from_tcp_endpoint (topology.selected_api_endpoint ()))
              .listen (
                port_from_tcp_endpoint (topology.selected_api_endpoint ()))
              .add_handler_group ("api");

            options.http ()
              .listen (topology.selected_api_http_endpoint ())
              .map_get<route_ready_handler_t> ("/ready")
              .map_post<create_game_http_handler_t> ("/games");

            /* API는 Object Client RouteMesh로 Play Object Server에 연결한다. 새 room의
             * owner는 Framework가 선택하며 API는 NodeRid를 지정하지 않는다. */
            auto mesh = options.add_route_mesh (sample_names_t::game_spot_node);
            mesh.set_object_role (object_role_t::client)
              .set_routing_id (zlink::routing_id_t::from (
                "tictactoe-api-" + topology.api_node))
              .listen (topology.selected_api_route_endpoint ());
            mesh.peer_connections ().connect (
              zlink::routing_id_t::from ("tictactoe-play-a"),
              topology.play_a_route_endpoint);
            mesh.peer_connections ().connect (
              zlink::routing_id_t::from ("tictactoe-play-b"),
              topology.play_b_route_endpoint);

            options.handlers ()
              .group ("api")
              .add<authenticate_player_handler_t> ();
        });
        if (auto_stop) {
            app.add_hosted_service (std::make_unique<stop_after_start_service_t> (app));
        }
        return app;
    }
};

} // namespace zlink::samples::tictactoe
