/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Configuration/sample_configuration.hpp"
#include "../Configuration/location_store.hpp"
#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_topology.hpp"
#include "../common_codecs.hpp"
#include "../sample_log_dir.hpp"
#include "../../Shared/Contracts/messages.hpp"
#include "../host_support.hpp"
#include "Infrastructure/ZLink/Actors/player_actor_factory.hpp"
#include "Infrastructure/ZLink/Actors/player_actor_relocation_adapter.hpp"
#include "Infrastructure/ZLink/Spots/EntrySpot/bingo_entry_spot.hpp"
#include "Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp"
#include "Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_relocation_adapter.hpp"

#include <memory>

namespace zlink::samples::bingo
{

using namespace framework;

class play_server_host_factory_t
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
        if (auto_stop) {
            app.add_hosted_service (std::make_unique<stop_after_start_service_t> (app));
        }
        app.logging ().use_console ().set_min_level (log_level_t::info);
        observe_runtime_metrics (app, topology.log_dir, "play-" + topology.play_node);
        app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
            options.configure_dispatch ()
              .message_flow (message_flow_log_mode_t::key_transitions)
              .trace_log_file (flow_log_path (topology.log_dir, "play-" + topology.play_node))
              .trace_label ("play-" + topology.play_node);
            options.services ()
              .add_singleton<sample_topology_t> (
                std::make_unique<sample_topology_t> (topology));
            use_default_bingo_codecs (options.codecs ());
            add_sample_location_store (options, topology);
            auto api_channel = options.add_client_server_channel (sample_names_t::api_channel);
            auto api_client = api_channel.client ();
            api_client.connect (topology.api_a_channel_endpoint);
            api_client.connect (topology.api_b_channel_endpoint);
            auto room_mesh = options.add_route_mesh (sample_names_t::room_spot_mesh);
            room_mesh
              .set_routing_id (zlink::routing_id_t::from (
                "bingo-play-" + topology.play_node))
              .set_object_role (object_role_t::server);
            room_mesh.channel_name (sample_names_t::room_spot_mesh).server ();
            room_mesh.listen (topology.selected_play_spot_router_endpoint ())
              .add_entry_spot<bingo_entry_spot_t> (
                [topology] (entry_spot_context_t context) {
                    return std::make_shared<bingo_entry_spot_t> (
                      std::move (context), topology);
                })
              .add_spot_factory<bingo_room_spot_t> (
                sample_names_t::room_spot,
                [] (spot_context_t context) {
                    return std::make_shared<bingo_room_spot_t> (
                      std::move (context));
                },
                [] (auto &factory) {
                    factory
                      .set_execution_mode (
                        user_spot_execution_mode_t::spot_wide)
                      .set_relocation_readiness (
                        spot_relocation_readiness_mode_t::application_signaled)
                      .template preserve_state_with<
                        bingo_room_relocation_adapter_t> ();
                })
              .add_actor_factory<player_actor_t, player_actor_factory_t> (
                sample_names_t::player_actor_type,
                std::make_shared<player_actor_factory_t> (),
                [] (auto &factory) {
                    factory
                      .template preserve_state_with<
                        player_actor_relocation_adapter_t> ();
                });
        });
        if (!auto_stop) {
            app.add_hosted_service (std::make_unique<
              play_api_channel_readiness_service_t> (topology.play_node));
        }
        return app;
    }
};

} // namespace zlink::samples::bingo
