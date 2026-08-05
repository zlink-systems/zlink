/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "Configuration/consumer_options.hpp"
#include "Endpoints/consumer_endpoints.hpp"

#include "../../Shared/location_store_registration.hpp"
#include "../../Shared/registry_messaging_contracts.hpp"
#include "../Shared/peer_locations_handler.hpp"
#include "../Shared/client_server_status_handler.hpp"

#include <zlink/framework.hpp>

namespace rm = zlink::framework::e2e::registry_messaging;
namespace rm_consumer = zlink::framework::e2e::registry_messaging::consumer;

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    app.config ().load_cli (argc, argv);
    const auto config_path = app.config ().model ().get ("config");
    if (!config_path) {
        throw std::runtime_error ("RegistryMessaging consumer requires --config=<path>");
    }
    app.config ().load_json (*config_path);
    const auto options = app.config ().bind_required<rm_consumer::consumer_options_t> ("e2e");
    app.logging ()
      .use_file (options.log_dir + "/" + options.trace_label + ".log")
      .set_min_level (zlink::framework::log_level_t::debug);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &framework) {
        framework.configure_dispatch ()
          .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
          .trace_log_file (options.log_dir + "/" + options.trace_label + "-flow.log")
          .trace_label (options.trace_label);
        auto channel = framework.add_client_server_channel (rm::api_channel);
        auto client = channel.client ();
        framework.add_client_server_channel (rm::workflow_channel)
          .client ();
        if (!options.redis_endpoint.empty ()) {
            rm::add_redis_location_store (framework, options.redis_endpoint, options.redis_key_prefix);
        }
        for (const auto &endpoint : options.provider_endpoints) {
            client.connect (endpoint);
        }
        if (!options.http_endpoint.empty ()) {
            framework.http ()
              .listen (options.http_endpoint)
              .configure_server ([] (zlink::framework::http_server_options_builder_t &server) {
                  server.set_max_request_body_size (4 * 1024 * 1024);
              })
              .map_health ("/health")
              .map_get<rm::peer_locations_handler_t> ("/locations/peers")
              .map_get<rm::client_server_status_handler_t> ("/client-server/status")
              .map_post<rm_consumer::batch_request_handler_t> ("/profile/batch-request")
              .map_post<rm_consumer::profile_request_handler_t> ("/profile/request")
              .map_post<rm_consumer::scale_in_transition_handler_t> (
                "/profile/scale-in-transition")
              .map_post<rm_consumer::workflow_request_handler_t> (
                "/workflow/request")
              .map_post<rm_consumer::slow_request_handler_t> ("/profile/slow-request")
              .map_post<rm_consumer::missing_request_handler_t> ("/profile/missing-request")
              .map_post<rm_consumer::missing_command_handler_t> ("/profile/missing-command")
              .map_post<rm_consumer::payload_request_handler_t> ("/profile/payload")
              .map_post<rm_consumer::backpressure_reset_handler_t> (
                "/profile/backpressure/reset")
              .map_post<rm_consumer::backpressure_send_handler_t> (
                "/profile/backpressure/send");
        }
    });
    return app.run (argc, argv);
}
