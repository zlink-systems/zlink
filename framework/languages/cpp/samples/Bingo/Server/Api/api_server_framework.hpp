/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_topology.hpp"
#include "../Configuration/location_store.hpp"
#include "../common_codecs.hpp"
#include "../sample_log_dir.hpp"
#include "../../Shared/Contracts/messages.hpp"
#include "Handlers/authenticate_player_handler.hpp"
#include "Handlers/get_player_record_handler.hpp"
#include "Handlers/match_bingo_handler.hpp"
#include "Handlers/report_bingo_result_handler.hpp"

namespace zlink::samples::bingo
{

using namespace framework;

inline app_t &add_bingo_api_server (app_t &app, const sample_topology_t &topology)
{
    app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
        options.configure_dispatch ()
          .message_flow (message_flow_log_mode_t::key_transitions)
          .trace_log_file (flow_log_path (topology.log_dir, "api-" + topology.api_node))
          .trace_label ("api-" + topology.api_node);
        use_default_bingo_codecs (options.codecs ());
        add_sample_location_store (options, topology);
        options.services ().add_singleton<bingo_player_record_store_t> ();

        options.add_client_server_channel (sample_names_t::api_channel)
          .server ()
          .set_bind_host (
            host_from_tcp_endpoint (topology.selected_api_channel_endpoint ()))
          .set_advertise_host (
            host_from_tcp_endpoint (topology.selected_api_channel_endpoint ()))
          .listen (port_from_tcp_endpoint (
            topology.selected_api_channel_endpoint ()))
          .add_handler_group ("api");

        auto matchmaking_mesh =
          options.add_route_mesh (sample_names_t::matchmaking_mesh);
        matchmaking_mesh
          .set_object_role (object_role_t::client)
          .set_routing_id (zlink::routing_id_t::from (
            "bingo-api-" + topology.api_node + "-matchmaking"))
          .listen (topology.selected_api_matchmaking_route_endpoint ());
        matchmaking_mesh.channel_name (sample_names_t::matchmaking_mesh).client ();
        auto room_mesh = options.add_route_mesh (sample_names_t::room_spot_mesh);
        room_mesh
          .set_object_role (object_role_t::client)
          .set_routing_id (zlink::routing_id_t::from (
            "bingo-api-" + topology.api_node + "-room"))
          .listen (topology.selected_api_play_route_endpoint ());
        room_mesh.channel_name (sample_names_t::room_spot_mesh).client ();

        options.handlers ()
          .group ("api")
          .add<authenticate_player_handler_t> ()
          .add<match_bingo_api_handler_t> ()
          .add<get_player_record_handler_t> ()
          .add<report_bingo_result_handler_t> ();
    });
    return app;
}

} // namespace zlink::samples::bingo
