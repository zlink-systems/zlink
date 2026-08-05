/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "Handlers/multi_node_handlers.hpp"
#include "../Shared/Endpoints/evidence_endpoint.hpp"
#include "../Shared/Support/codecs.hpp"
#include "../Shared/Support/configuration.hpp"
#include "../Shared/Support/location_store.hpp"

#include <zlink/framework.hpp>

#include <memory>
#include <string>

struct multi_node_options_t
{
    std::string log_dir;
    std::string node_rid;
    std::string route_endpoint;
    std::string spot_router_endpoint;
    std::string pubsub_endpoint;
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    bool disable_route_mesh;

    static multi_node_options_t bind (const zlink::framework::configuration_section_t &section)
    {
        return {.log_dir = section.require ("logDir"),
                .node_rid = section.get ("nodeRid").value_or (multi_node_a_name),
                .route_endpoint = section.get ("routeEndpoint").value_or (""),
                .spot_router_endpoint = section.require ("spotRouterEndpoint"),
                .pubsub_endpoint = section.require ("pubsubEndpoint"),
                .http_endpoint = section.require ("httpEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .disable_route_mesh = section.get ("disableRouteMesh").value_or ("false") == "true"};
    }
};

inline int run_multi_node_server (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    load_spot_service_config (app, argc, argv, "multi-node");
    const auto config = app.config ().bind_required<multi_node_options_t> ("e2e");
    const auto &log_dir = config.log_dir;
    const auto &node_rid = config.node_rid;
    const auto &route_endpoint = config.route_endpoint;
    const auto &spot_router_endpoint = config.spot_router_endpoint;
    const auto &pubsub_endpoint = config.pubsub_endpoint;
    const auto &http_endpoint = config.http_endpoint;
    const auto &redis_endpoint = config.redis_endpoint;
    const auto &redis_key_prefix = config.redis_key_prefix;
    const auto disable_route_mesh = config.disable_route_mesh;

    app.logging ()
      .use_file (log_dir + "/" + node_rid + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        auto state = std::make_unique<scenario_state_t> (node_rid);
        auto *state_ptr = state.get ();
        options.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (log_dir + "/" + node_rid + "-flow.log")
          .trace_label ("cpp-sm-" + node_rid);
        options.services ()
          .add_singleton<scenario_state_t> (std::move (state))
          .add_transient<multi_node_route_ping_handler_t, scenario_state_t> ()
          .add_transient<multi_node_create_local_handler_t, scenario_state_t,
                         zlink::framework::spot_node_manager_t,
                         zlink::framework::route_client_t> ()
          .add_transient<multi_node_state_route_handler_t, scenario_state_t,
                         zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ()
          .add_transient<multi_node_create_user_local_handler_t, scenario_state_t,
                         zlink::framework::spot_node_manager_t> ()
          .add_transient<multi_node_spot_only_mesh_handler_t, scenario_state_t,
                         zlink::framework::spot_node_manager_t> ()
          .add_transient<multi_node_spot_only_join_handler_t, scenario_state_t,
                         zlink::framework::session_actor_manager_t> ()
          .add_transient<multi_node_state_member_handler_t, zlink::framework::route_client_t,
                         zlink::framework::spot_handle_resolver_t> ();
        configure_codecs (options.codecs ());
        add_redis_location_store (options, redis_endpoint, redis_key_prefix);

        if (!disable_route_mesh) {
            const auto route_name =
              multi_node_route_channel_for (node_rid);
            auto route = options.add_route_mesh (route_name);
            route.listen (route_endpoint)
              .set_routing_id (zlink::routing_id_t::from (node_rid))
              .channel_name (route_name);
            route
              .add_route_request_handler<multi_node_route_ping_handler_t,
                                          e2e::channel_control_ping_req_t,
                                          e2e::channel_control_ping_res_t> (
                "MultiNodeRoutePing")
              .add_route_request_handler<multi_node_create_local_handler_t,
                                          e2e::multi_node_create_spot_req_t,
                                          e2e::multi_node_create_spot_res_t> (
                "MultiNodeCreateSpotReq")
              .add_route_request_handler<multi_node_state_member_handler_t,
                                          e2e::multi_node_state_route_req_t,
                                          e2e::state_res_t> (
                e2e::multi_node_state_route_req_t::packet_name);
        }
        const auto spot_name =
          disable_route_mesh ? e2e::spot_only_mesh : e2e::spot_mesh;
        auto spot = options.add_route_mesh (spot_name);
        spot.listen (spot_router_endpoint)
          .set_routing_id (zlink::routing_id_t::from (node_rid))
          .channel_name (spot_name);
        spot.add_entry_spot<multi_node_entry_spot_t> (
          [state_ptr] (entry_spot_context_t context) {
              return std::make_shared<multi_node_entry_spot_t> (
                std::move (context), *state_ptr);
          })
          .add_actor_factory<multi_node_actor_t, multi_node_actor_factory_t> (
            e2e::actor_type,
            std::make_shared<multi_node_actor_factory_t> (),
            [] (auto &factory) { factory.disable_relocation (); });
        if (node_rid == multi_node_a_name) {
            spot.add_spot_factory<multi_node_spot_a_t> (
              e2e::multi_spot_a,
              [state_ptr] (spot_context_t context) {
                  return std::make_shared<multi_node_spot_a_t> (
                    std::move (context), *state_ptr);
              },
              [] (auto &factory) {
                  factory.disable_relocation ();
              });
        } else {
            spot.add_spot_factory<multi_node_spot_b_t> (
              e2e::multi_spot_b,
              [state_ptr] (spot_context_t context) {
                  return std::make_shared<multi_node_spot_b_t> (
                    std::move (context), *state_ptr);
              },
              [] (auto &factory) {
                  factory.disable_relocation ();
              });
        }
        options.http ()
          .listen (http_endpoint)
          .map_health ("/health")
          .map_get<evidence_handler_t> ("/evidence")
          .map_post<evidence_wait_handler_t> ("/evidence/wait")
          .map_post<multi_node_create_local_handler_t> ("/spot/create-local")
          .map_post<multi_node_create_user_local_handler_t> ("/spot/create-user-local")
          .map_post<multi_node_spot_only_mesh_handler_t> ("/spot/spot-only/request-send")
          .map_post<multi_node_spot_only_join_handler_t> ("/actor/spot-only-join")
          .map_post<multi_node_state_route_handler_t> ("/spot/state/request")
          .map_post<shutdown_handler_t> ("/shutdown");
    });
    return app.run (argc, argv);
}
