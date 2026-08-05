/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "Handlers/play_actor_handlers.hpp"
#include "Handlers/play_control_handlers.hpp"
#include "Spots/play_spot_runtime.hpp"
#include "Spots/play_spot_types.hpp"
#include "Support/play_support.hpp"
#include "../Shared/codecs.hpp"
#include "../Shared/location_store.hpp"

#include <zlink/framework.hpp>
#include <zlink/http_client.hpp>

#include <memory>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::play {

namespace yd = zlink::framework::e2e::automatic_turn_dispatch;

inline void configure_play_host (zlink::framework::app_t &app,
                                 const play_options_t &play_options)
{
    auto http_client_builder =
      zlink::http_client::client_t::create (play_options.external_api_base_url);
    http_client_builder.timeout (std::chrono::seconds (5));
    auto external_api =
      http_client_builder.build_server<external_api_http_client_tag_t> (
        std::make_shared<zlink::http_client::framework_execution_turn_t> ());
    app.logging ()
      .use_file (play_options.log_dir + "/" + play_options.node_rid + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([=] (zlink::framework::zlink_framework_options_t &options) {
        auto evidence =
          std::make_unique<evidence_store_t> (
            play_options.node_rid,
            play_options.log_dir + "/" + play_options.node_rid + ".evidence.log");
        auto *evidence_ptr = evidence.get ();
        options.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (play_options.log_dir + "/" + play_options.node_rid + "-flow.log")
          .trace_label ("cpp-atd-" + play_options.node_rid);
        options.services ()
          .add_singleton<evidence_store_t> (std::move (evidence))
          .add_transient<bind_await_actors_handler_t, evidence_store_t,
                         zlink::framework::spot_node_manager_t,
                         zlink::framework::session_actor_manager_t> ()
          .add_transient<ensure_spot_handler_t, evidence_store_t,
                         zlink::framework::spot_node_manager_t> ()
          .add_transient<evidence_handler_t, evidence_store_t> ()
          .add_transient<evidence_wait_handler_t, evidence_store_t> ();
        server::configure_codecs (options.codecs ());
        server::add_redis_location_store (options, play_options.redis_endpoint,
                                          play_options.redis_key_prefix);
        options.add_client_server_channel (yd::delay_channel)
          .client ()
          .connect (play_options.delay_endpoint);
        auto control = options.add_route_mesh (yd::control_channel);
        control.listen (play_options.control_endpoint)
          .set_routing_id (zlink::routing_id_t::from (play_options.node_rid))
          .channel_name (yd::control_channel);
        control
          .add_route_request_handler<bind_await_actors_handler_t,
                                      yd::bind_await_actors_req_t,
                                      yd::bind_await_actors_res_t> (
            yd::bind_await_actors_req_t::packet_name)
          .add_route_request_handler<ensure_spot_handler_t,
                                      yd::ensure_spot_req_t,
                                      yd::ensure_spot_res_t> (
            yd::ensure_spot_req_t::packet_name)
          .add_route_request_handler<evidence_handler_t,
                                      yd::await_evidence_req_t,
                                      yd::await_evidence_res_t> (
            yd::await_evidence_req_t::packet_name)
          .add_route_request_handler<evidence_wait_handler_t,
                                      yd::await_evidence_wait_req_t,
                                      yd::await_evidence_res_t> (
            yd::await_evidence_wait_req_t::packet_name);
        auto spot_route = options.add_route_mesh (yd::spot_route_channel);
        spot_route.listen (play_options.spot_route_endpoint)
          .set_routing_id (zlink::routing_id_t::from (play_options.node_rid))
          .channel_name (yd::spot_route_channel);
        auto spot = options.add_route_mesh (yd::spot_channel);
        spot.channel_name (yd::spot_route_channel);
        spot.set_routing_id (zlink::routing_id_t::from (play_options.node_rid))
          .listen (play_options.spot_router_endpoint)
          .add_entry_spot<await_entry_spot_t> (
            [evidence_ptr] (entry_spot_context_t context) {
                return std::make_shared<await_entry_spot_t> (
                  std::move (context), *evidence_ptr);
            })
          .add_spot_factory<await_probe_spot_t> (
            yd::probe_spot_name,
            [evidence_ptr, external_api] (spot_context_t context) {
                return std::make_shared<await_probe_spot_t> (
                  std::move (context), *evidence_ptr, external_api);
            },
            [] (auto &factory) {
                factory.disable_relocation ();
            })
          .add_actor_factory<await_actor_t, await_actor_factory_t> (
            yd::actor_type,
            std::make_shared<await_actor_factory_t> (),
            [] (auto &factory) { factory.disable_relocation (); });
        options.http ().listen (play_options.http_endpoint).map_health ("/health");
    });
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
