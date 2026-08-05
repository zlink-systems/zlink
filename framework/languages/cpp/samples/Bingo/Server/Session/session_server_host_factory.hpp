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
#include "Sessions/bingo_session.hpp"

namespace zlink::samples::bingo
{

using namespace framework;

class session_server_host_factory_t
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
        observe_runtime_metrics (app, topology.log_dir, "session-" + topology.session_node);
        app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
            options.configure_dispatch ()
              .message_flow (message_flow_log_mode_t::key_transitions)
              .trace_log_file (flow_log_path (topology.log_dir, "session-" + topology.session_node))
              .trace_label ("session-" + topology.session_node);
            options.services ().add_singleton<sample_topology_t> (
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
                "bingo-session-" + topology.session_node))
              .set_object_role (object_role_t::client);
            room_mesh.channel_name (sample_names_t::room_spot_mesh).client ();
            room_mesh.listen (topology.selected_session_route_endpoint ());
            options.add_stream_node (sample_names_t::stream_node)
              .bind (topology.selected_stream_endpoint ())
              .register_session<bingo_session_t> ();
        });
        if (!auto_stop) {
            app.add_hosted_service (std::make_unique<route_mesh_readiness_service_t> (
              "session-" + topology.session_node,
              sample_names_t::room_spot_mesh,
              "room"));
        }
        return app;
    }
};

} // namespace zlink::samples::bingo
