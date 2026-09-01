/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Configuration/sample_configuration.hpp"
#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_topology.hpp"
#include "../sample_log_dir.hpp"
#include "../../Shared/Contracts/messages.hpp"
#include "../host_support.hpp"
#include "Sessions/bingo_session.hpp"

#include <zlink/locations/redis.hpp>
#include <zlink/codecs/protobuf.hpp>

namespace zlink::samples::bingo
{

using namespace framework;

class session_server_host_factory_t
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
        app.logging ().use_file (
          flow_log_path (topology.log_dir, "session-" + topology.session_node));
        observe_runtime_metrics (app, topology.log_dir, "session-" + topology.session_node);
        auto &options = app.add_zlink_framework ();
        options.configure_dispatch ().message_flow (message_flow_log_mode_t::normal);
        options.codecs ().use (zlink::framework_codecs::protobuf ());
        options.add_location_store<redis::redis_location_store_t> ()
          .set_connection_string (topology.redis_endpoint)
          .set_key_prefix (topology.redis_key_prefix + "location:");
        options.add_relocation_store<redis::redis_relocation_store_t> ()
          .set_connection_string (topology.redis_endpoint)
          .set_key_prefix (topology.redis_key_prefix + "relocation:");
        options.add_client_server_channel (sample_names_t::api_channel).client ();
        auto room_mesh = options.add_route_mesh (sample_names_t::room_spot_mesh);
        room_mesh
          .set_routing_id (zlink::routing_id_t::from ("bingo-session-" + topology.session_node))
          .listen (topology.selected_session_route_endpoint ());
        room_mesh.objects ().client ();
        room_mesh.channel (sample_names_t::room_spot_mesh).client ();
        options.add_stream_node (sample_names_t::stream_node)
          .bind (topology.selected_stream_endpoint ())
          .register_session<bingo_session_t> ();
        app.add_hosted_service (std::make_unique<route_mesh_readiness_service_t> (
          "session-" + topology.session_node, sample_names_t::room_spot_mesh, "room"));
        return app;
    }
};

} // namespace zlink::samples::bingo
