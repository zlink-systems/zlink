/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Shared/codecs.hpp"
#include "../Shared/location_store.hpp"
#include "Support/session_support.hpp"
#include "Support/await_session.hpp"

#include <zlink/framework.hpp>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::session {

namespace yd = zlink::framework::e2e::automatic_turn_dispatch;

inline void configure_session_host (zlink::framework::app_t &app,
                                    const session_options_t &session_options)
{
    app.logging ()
      .use_file (session_options.log_dir + "/" + session_options.node_rid + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([=] (zlink::framework::zlink_framework_options_t &framework_options) {
        framework_options.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (session_options.log_dir + "/" + session_options.node_rid + "-flow.log")
          .trace_label ("cpp-atd-" + session_options.node_rid);
        server::configure_codecs (framework_options.codecs ());
        server::add_redis_location_store (framework_options, session_options.redis_endpoint,
                                          session_options.redis_key_prefix);
        auto control_route = framework_options.add_route_mesh (yd::control_channel);
        control_route.listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from (session_options.node_rid))
          .channel_name (yd::control_channel);
        control_route.peer_connections ().connect (session_options.control_endpoint);
        if (!session_options.control_peer_endpoint.empty ()) {
            control_route.peer_connections ().connect (
              session_options.control_peer_endpoint);
        }
        auto spot_route = framework_options.add_route_mesh (yd::spot_route_channel);
        spot_route.listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from (session_options.node_rid))
          .channel_name (yd::spot_route_channel);
        spot_route.peer_connections ().connect (session_options.spot_route_endpoint);
        if (!session_options.spot_route_peer_endpoint.empty ()) {
            spot_route.peer_connections ().connect (
              session_options.spot_route_peer_endpoint);
        }
        auto spot = framework_options.add_route_mesh (yd::spot_channel);
        spot.channel_name (yd::spot_route_channel);
        spot.set_routing_id (zlink::routing_id_t::from (session_options.node_rid))
          .listen (session_options.spot_router_endpoint);
        framework_options.add_stream_node (yd::stream_node)
          .bind (session_options.stream_endpoint)
          .register_session<await_session_t> ();
        framework_options.http ().listen (session_options.http_endpoint).map_health ("/health");
    });
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::session
