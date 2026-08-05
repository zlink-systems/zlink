/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "Endpoints/operational_endpoints.hpp"
#include "Endpoints/spot_failure_endpoints.hpp"
#include "Endpoints/spot_interaction_endpoints.hpp"
#include "Endpoints/spot_lifecycle_endpoints.hpp"
#include "Handlers/play_actor_handlers.hpp"
#include "Handlers/play_control_handlers.hpp"
#include "Handlers/play_session_handlers.hpp"
#include "Handlers/play_spot_route_handlers.hpp"
#include "Spots/play_actor_model.hpp"
#include "../Shared/Handlers/channel_control_ping_handler.hpp"
#include "../Shared/Support/codecs.hpp"
#include "../Shared/Support/configuration.hpp"
#include "../Shared/Support/location_store.hpp"

#include <zlink/framework.hpp>

#include <memory>
#include <string>
#include <vector>

struct play_options_t
{
    std::string log_dir;
    std::string node_rid;
    std::string route_endpoint;
    std::vector<std::string> route_peer_endpoints;
    std::string spot_router_endpoint;
    std::vector<std::string> spot_peer_endpoints;
    std::string pubsub_endpoint;
    std::vector<std::string> peer_pubsub_endpoints;
    std::string api_peer_endpoint;
    std::string api_endpoint;
    std::string publisher_endpoint;
    std::string http_endpoint;
    std::string play_a_http_endpoint;
    std::string play_b_http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    bool route_mesh_enabled;

    static play_options_t bind (const zlink::framework::configuration_section_t &section)
    {
        return {.log_dir = section.require ("logDir"),
                .node_rid = section.get ("nodeRid").value_or ("play-a"),
                .route_endpoint = section.require ("routeEndpoint"),
                .route_peer_endpoints =
                  split_endpoints (section.get ("routePeerEndpoints").value_or ("")),
                .spot_router_endpoint = section.require ("spotRouterEndpoint"),
                .spot_peer_endpoints =
                  split_endpoints (section.get ("spotPeerEndpoints").value_or ("")),
                .pubsub_endpoint = section.require ("pubsubEndpoint"),
                .peer_pubsub_endpoints = split_endpoints (
                  section.get ("peerPubsubEndpoints").value_or ("")),
                .api_peer_endpoint = section.get ("apiPeerEndpoint").value_or (""),
                .api_endpoint = section.get ("apiEndpoint").value_or (""),
                .publisher_endpoint = section.get ("publisherEndpoint").value_or (""),
                .http_endpoint = section.require ("httpEndpoint"),
                .play_a_http_endpoint = section.require ("playHttpEndpoints.playA"),
                .play_b_http_endpoint = section.require ("playHttpEndpoints.playB"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .route_mesh_enabled =
                  section.get ("routeMeshEnabled").value_or ("true") == "true"};
    }
};

inline int run_play_server (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    load_spot_service_config (app, argc, argv, "play");
    const auto config = app.config ().bind_required<play_options_t> ("e2e");
    const auto &log_dir = config.log_dir;
    const auto &node_rid = config.node_rid;
    const auto &route_endpoint = config.route_endpoint;
    const auto &route_peer_endpoints = config.route_peer_endpoints;
    const auto &spot_router_endpoint = config.spot_router_endpoint;
    const auto &spot_peer_endpoints = config.spot_peer_endpoints;
    const auto &pubsub_endpoint = config.pubsub_endpoint;
    const auto &api_peer_endpoint = config.api_peer_endpoint;
    const auto &api_endpoint = config.api_endpoint;
    const auto &publisher_endpoint = config.publisher_endpoint;
    const auto &http_endpoint = config.http_endpoint;
    const auto &redis_endpoint = config.redis_endpoint;
    const auto &redis_key_prefix = config.redis_key_prefix;
    const auto route_mesh_enabled = config.route_mesh_enabled;

    app.logging ()
      .use_file (log_dir + "/" + node_rid + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        auto state = std::make_unique<scenario_state_t> (node_rid);
        auto *state_ptr = state.get ();
        options.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (log_dir + "/" + node_rid + "-flow.log")
          .trace_label ("cpp-sm-" + node_rid)
          .set_message_flow_observer (
            [state_ptr] (const zlink::framework::message_flow_event_t &event) {
                if (event.outcome != zlink::framework::message_flow_outcome_t::error
                    || event.surface != zlink::framework::dispatch_error_surface_t::spot_route
                    || event.error_reason
                         != zlink::framework::dispatch_error_reason_t::handler_missing
                    || !event.error_action || !event.packet_name) {
                    return;
                }
                const auto action =
                  *event.error_action == zlink::framework::dispatch_error_action_t::reply_error
                    ? "reply_error"
                    : (*event.error_action == zlink::framework::dispatch_error_action_t::drop
                         ? "drop"
                         : "fail_caller");
                state_ptr->record ("SpotRouteDispatchFailure", *event.packet_name,
                                   event.spot_id.value_or (""),
                                   std::string ("handler_missing:") + action);
            });
        auto http_execution_turn =
          std::make_shared<zlink::http_client::framework_execution_turn_t> ();
        auto play_a_http = zlink::http_client::client_t::create (config.play_a_http_endpoint)
                             .build_server<play_a_owner_http_client_tag_t> (
                               http_execution_turn);
        auto play_b_http = zlink::http_client::client_t::create (config.play_b_http_endpoint)
                             .build_server<play_b_owner_http_client_tag_t> (
                               http_execution_turn);
        options.services ()
          .add_singleton<scenario_state_t> (std::move (state))
          .add_singleton<play_a_owner_http_client_t> (
            std::make_unique<play_a_owner_http_client_t> (std::move (play_a_http)))
          .add_singleton<play_b_owner_http_client_t> (
            std::make_unique<play_b_owner_http_client_t> (std::move (play_b_http)))
          .add_transient<ensure_actor_handler_t, scenario_state_t,
                         zlink::framework::spot_node_manager_t,
                         zlink::framework::session_actor_manager_t> ()
          .add_transient<spot_lifecycle_handler_t, scenario_state_t,
                         zlink::framework::spot_node_manager_t> ()
          .add_transient<join_spot_handler_t, scenario_state_t,
                         zlink::framework::session_actor_manager_t> ()
          .add_transient<complex_actor_handler_t, scenario_state_t,
                         zlink::framework::session_actor_manager_t> ()
          .add_transient<missing_actor_handler_t, scenario_state_t,
                         zlink::framework::session_actor_manager_t> ()
          .add_transient<push_bound_session_handler_t,
                         zlink::framework::session_actor_manager_t> ()
          .add_transient<remote_actor_flow_handler_t, scenario_state_t,
                         zlink::framework::session_actor_manager_t,
                         play_a_owner_http_client_t,
                         play_b_owner_http_client_t> ()
          .add_transient<remote_actor_request_handler_t, scenario_state_t,
                         zlink::framework::route_client_t,
                         zlink::framework::session_actor_manager_t,
                         play_a_owner_http_client_t,
                         play_b_owner_http_client_t> ()
          .add_transient<worker_spot_handler_t, zlink::framework::session_actor_manager_t> ()
          .add_transient<create_spot_handler_t, scenario_state_t,
                         zlink::framework::spot_node_manager_t> ()
          .add_transient<create_alternate_spot_handler_t, scenario_state_t,
                         zlink::framework::spot_node_manager_t> ()
          .add_transient<spot_state_command_route_handler_t,
                         zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<spot_publish_route_handler_t,
                         zlink::framework::spot_publisher_client_t> ()
          .add_transient<spot_publish_wait_handler_t, scenario_state_t> ()
          .add_transient<spot_worker_start_route_handler_t,
                         zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<spot_worker_complete_handler_t, scenario_state_t> ()
          .add_transient<spot_stage_probe_route_handler_t,
                         zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<spot_stage_timer_route_handler_t,
                         zlink::framework::route_client_t,
                         scenario_state_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<spot_idle_close_route_handler_t, scenario_state_t,
                         zlink::framework::spot_node_manager_t> ()
          .add_transient<spot_overrun_start_route_handler_t, scenario_state_t,
                         zlink::framework::spot_node_manager_t> ()
          .add_transient<spot_slow_route_handler_t, zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<spot_missing_handler_request_handler_t,
                         zlink::framework::route_client_t,
                         scenario_state_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<spot_missing_handler_command_handler_t,
                         zlink::framework::route_client_t,
                         scenario_state_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<spot_missing_target_request_handler_t,
                         zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<spot_missing_route_handler_t, zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<spot_outbound_route_handler_t, zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<spot_outbound_negative_route_handler_t,
                         zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<spot_to_spot_route_handler_t, zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<spot_to_spot_timeout_route_handler_t,
                         zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<spot_to_spot_negative_route_handler_t,
                         zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<lifecycle_spot_handler_t, scenario_state_t,
                         zlink::framework::spot_node_manager_t> ()
          .add_transient<close_spot_handler_t, scenario_state_t,
                         zlink::framework::spot_node_manager_t> ()
          .add_transient<type_mismatch_spot_handler_t, scenario_state_t,
                         zlink::framework::spot_node_manager_t,
                         zlink::framework::session_actor_manager_t> ();
        configure_codecs (options.codecs ());
        add_redis_location_store (options, redis_endpoint, redis_key_prefix);

        if (route_mesh_enabled) {
            auto route = options.add_route_mesh (e2e::route_channel);
            route.listen (route_endpoint)
              .set_routing_id (zlink::routing_id_t::from (node_rid))
              .channel_name (e2e::route_channel);
            for (const auto &peer : route_peer_endpoints)
                route.peer_connections ().connect (peer);
            route
              .add_route_request_handler<ensure_actor_handler_t,
                                          e2e::ensure_actor_req_t,
                                          e2e::ensure_actor_res_t> ("EnsureActor")
              .add_route_request_handler<channel_echo_handler_t,
                                          e2e::channel_echo_req_t,
                                          e2e::channel_echo_res_t> (
                "ChannelEchoReq")
              .add_route_request_handler<spot_lifecycle_handler_t,
                                          e2e::lifecycle_req_t,
                                          e2e::lifecycle_res_t> ("LifecycleReq");
        }
        if (!api_endpoint.empty () || !api_peer_endpoint.empty ()) {
            auto api = options.add_client_server_channel (e2e::api_channel);
            if (!api_endpoint.empty ()) {
                api.enable_server (api_endpoint).use_handler_group (e2e::handler_group);
            }
            if (!api_peer_endpoint.empty ()) {
                api.enable_client (api_peer_endpoint);
            }
        }
        if (!publisher_endpoint.empty ()) {
            options.add_fanout_channel (e2e::publisher_channel)
              .enable_publisher (publisher_endpoint);
        }
        auto spot = options.add_route_mesh (e2e::spot_mesh);
        spot.listen (spot_router_endpoint)
          .set_routing_id (zlink::routing_id_t::from (node_rid))
          .channel_name (e2e::spot_mesh);
        for (const auto &peer : spot_peer_endpoints)
            spot.peer_connections ().connect (peer);
        spot.add_route_request_handler<channel_echo_handler_t,
                                       e2e::channel_echo_req_t,
                                       e2e::channel_echo_res_t> ("ChannelEchoReq");
        spot.add_entry_spot<entry_spot_t> (
          [state_ptr] (entry_spot_context_t context) {
              return std::make_shared<entry_spot_t> (
                std::move (context), *state_ptr);
          })
          .add_spot_factory<user_spot_t> (
            e2e::user_spot,
            [state_ptr] (spot_context_t context) {
                return std::make_shared<user_spot_t> (
                  std::move (context), *state_ptr);
            },
            [] (auto &factory) {
                factory.disable_relocation ();
            })
          .add_spot_factory<alternate_user_spot_t> (
            e2e::alternate_spot,
            [] (spot_context_t context) {
                return std::make_shared<alternate_user_spot_t> (
                  std::move (context));
            },
            [] (auto &factory) {
                factory.disable_relocation ();
            })
          .add_actor_factory<scenario_actor_t, scenario_actor_factory_t> (
            e2e::actor_type,
            std::make_shared<scenario_actor_factory_t> (),
            [] (auto &factory) { factory.disable_relocation (); });
        auto &http = options.http ().listen (http_endpoint);
        map_operational_endpoints (http);
        map_spot_lifecycle_endpoints (http);
        map_spot_interaction_endpoints (http);
        map_spot_failure_endpoints (http);
        options.handlers ()
          .group (e2e::handler_group)
          .add<channel_echo_handler_t> ()
          .add_send<channel_command_handler_t> ()
          .add<channel_slow_handler_t> ();
    });
    return app.run (argc, argv);
}
