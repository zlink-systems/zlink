/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Configuration/provider_options.hpp"
#include "Endpoints/provider_endpoints.hpp"
#include "Handlers/provider_handlers.hpp"
#include "Infrastructure/scenario_state.hpp"

#include "../../Shared/location_store_registration.hpp"
#include "../../Shared/registry_messaging_contracts.hpp"
#include "../../Shared/tcp_endpoint.hpp"
#include "../Shared/peer_locations_handler.hpp"
#include "../Shared/client_server_status_handler.hpp"

#include <zlink/framework.hpp>

#include <memory>
#include <string>

namespace e2e = zlink::framework::e2e::registry_messaging;
namespace rm_provider = zlink::framework::e2e::registry_messaging::provider;

namespace
{

void configure_common_codecs (zlink::framework::codec_options_builder_t codecs)
{
}

std::string dispatch_reason_name (zlink::framework::dispatch_error_reason_t reason)
{
    switch (reason) {
        case zlink::framework::dispatch_error_reason_t::handler_missing:
            return "handler_missing";
        case zlink::framework::dispatch_error_reason_t::payload_decode_failed:
            return "payload_decode_failed";
        case zlink::framework::dispatch_error_reason_t::handler_exception:
            return "handler_exception";
        case zlink::framework::dispatch_error_reason_t::invalid_frame:
            return "invalid_frame";
        case zlink::framework::dispatch_error_reason_t::reply_path_missing:
            return "reply_path_missing";
        case zlink::framework::dispatch_error_reason_t::unexpected_reply:
            return "unexpected_reply";
    }
    return "unknown";
}

std::string dispatch_action_name (zlink::framework::dispatch_error_action_t action)
{
    switch (action) {
        case zlink::framework::dispatch_error_action_t::drop:
            return "drop";
        case zlink::framework::dispatch_error_action_t::reply_error:
            return "reply_error";
    }
    return "unknown";
}

} // namespace

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    app.config ().load_cli (argc, argv);
    const auto config_path = app.config ().model ().get ("config");
    if (!config_path) {
        throw std::runtime_error ("RegistryMessaging provider requires --config=<path>");
    }
    app.config ().load_json (*config_path);
    const auto options = app.config ().bind_required<rm_provider::provider_options_t> ("e2e");
    app.logging ()
      .use_file (options.log_dir + "/" + options.rid + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &framework) {
        framework.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (options.log_dir + "/" + options.rid + "-flow.log")
          .trace_label ("cpp-rm-" + options.rid);
        auto state =
          std::make_unique<rm_provider::scenario_state_t> (options.rid, options.instance_id);
        auto *state_ptr = state.get ();
        framework.configure_dispatch ().set_message_flow_observer (
          [state_ptr] (const zlink::framework::message_flow_event_t &event) {
              if (event.outcome != zlink::framework::message_flow_outcome_t::error
                  || !event.error_reason || !event.error_action) {
                  return;
              }
              state_ptr->record ("DispatchError",
                                 dispatch_reason_name (*event.error_reason) + ":"
                                   + dispatch_action_name (*event.error_action));
          });
        framework.services ().add_singleton<rm_provider::scenario_state_t> (std::move (state));
        framework.services ().add_transient<rm_provider::route_ping_handler_t,
                                            rm_provider::scenario_state_t> ();
        framework.services ().add_transient<rm_provider::server_weight_handler_t,
                                            zlink::framework::channel_runtime_options_t> ();
        configure_common_codecs (framework.codecs ());
        e2e::add_redis_location_store (framework, options.redis_endpoint, options.redis_key_prefix);
        if (!options.api_endpoint.empty ()) {
            const auto endpoint = e2e::parse_tcp_endpoint (options.api_endpoint);
            auto channel = framework.add_client_server_channel (e2e::api_channel);
            channel.client ();
            auto server = channel.server ();
            server.set_bind_host (endpoint.host)
              .set_advertise_host (endpoint.host)
              .listen (endpoint.port)
              .add_handler_group (e2e::handler_group);
            if (options.server_weight) {
                server.set_weight (*options.server_weight);
            }
        }
        if (!options.route_endpoint.empty ()) {
            auto route = framework.add_route_mesh (e2e::route_channel);
            route.listen (options.route_endpoint)
              .set_routing_id (zlink::routing_id_t::from (options.rid))
              .channel_name (e2e::route_channel)
              .server ();
            route.add_route_request_handler<rm_provider::route_ping_handler_t,
                                            e2e::scenario_route_req_t,
                                            e2e::scenario_route_res_t> (
              "ScenarioRouteReq");
            for (const auto &peer : options.route_peers) {
                if (peer != options.route_endpoint) {
                    route.peer_connections ().connect (peer);
                }
            }
        }
        if (!options.http_endpoint.empty ()) {
            framework.http ()
              .listen (options.http_endpoint)
              .map_health ("/health")
              .map_get<rm_provider::evidence_handler_t> ("/evidence")
              .map_post<rm_provider::http_profile_request_handler_t> ("/profile/request")
              .map_post<rm_provider::http_profile_command_handler_t> ("/profile/command")
              .map_post<rm_provider::http_route_request_handler_t> ("/profile/route/request")
              .map_post<rm_provider::http_route_missing_handler_t> ("/profile/route/missing")
              .map_get<e2e::peer_locations_handler_t> ("/locations/peers")
              .map_get<e2e::client_server_status_handler_t> ("/client-server/status")
              .map_post<rm_provider::server_weight_handler_t> ("/admin/server-weight");
        }
        framework.handlers ()
          .group (e2e::handler_group)
          .add<rm_provider::profile_request_handler_t> ()
          .add<rm_provider::payload_request_handler_t> ()
          .add_send<rm_provider::profile_command_handler_t> ();
    });
    return app.run (argc, argv);
}
