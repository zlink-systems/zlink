/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Configuration/sample_configuration.hpp"
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
#include <zlink/locations/redis.hpp>

namespace zlink::samples::bingo
{

using namespace framework;

class play_server_host_factory_t
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
        app.logging ().use_console ().set_min_level (log_level_t::info);
        app.logging ().use_file (flow_log_path (topology.log_dir, "play-" + topology.play_node));
        observe_runtime_metrics (app, topology.log_dir, "play-" + topology.play_node);
        auto &options = app.add_zlink_framework ();
        options.configure_dispatch ().message_flow (message_flow_log_mode_t::normal);
        options.codecs ().use<bingo_protobuf_codecs_t> ();
        options.add_location_store<redis::redis_location_store_t> ()
          .set_connection_string (topology.redis_endpoint)
          .set_key_prefix (topology.redis_key_prefix + "location:");
        options.add_relocation_store<redis::redis_relocation_store_t> ()
          .set_connection_string (topology.redis_endpoint)
          .set_key_prefix (topology.redis_key_prefix + "relocation:");
        options.add_client_server_channel (sample_names_t::api_channel).client ();
        auto room_mesh = options.add_route_mesh (sample_names_t::room_spot_mesh);
        room_mesh.set_routing_id (zlink::routing_id_t::from ("bingo-play-" + topology.play_node))
          .listen (topology.selected_play_spot_router_endpoint ());
        room_mesh.channel (sample_names_t::room_spot_mesh).server ();
        room_mesh.objects ()
          .server ()
          .add_entry_spot<bingo_entry_spot_t> ()
          .add_spot_factory<bingo_room_spot_t> (sample_names_t::room_spot)
          .set_relocation_coordination_mode (
            spot_relocation_coordination_mode_t::application_signaled)
          .preserve_state_with<bingo_room_relocation_adapter_t> ()
          .add_actor_factory<player_actor_t, player_actor_factory_t> (
            sample_names_t::player_actor_type)
          .preserve_state_with<player_actor_relocation_adapter_t> ();
        app.add_hosted_service (std::make_unique<play_peer_route_readiness_service_t> (
          sample_names_t::room_spot_mesh, topology.play_node,
          "bingo-play-" + (topology.play_node == "a" ? std::string ("b") : std::string ("a"))));
        app.add_hosted_service (
          std::make_unique<play_api_channel_readiness_service_t> (topology.play_node));
        return app;
    }
};

} // namespace zlink::samples::bingo
