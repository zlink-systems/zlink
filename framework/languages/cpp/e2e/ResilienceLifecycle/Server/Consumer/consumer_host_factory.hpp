/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "Configuration/consumer_options.hpp"
#include "Endpoints/consumer_endpoints.hpp"
#include "../Shared/location_store.hpp"

#include <zlink/framework.hpp>

#include <memory>

namespace zlink::framework::e2e::resilience_lifecycle::consumer
{

inline void configure_consumer_host (zlink::framework::zlink_framework_options_t &framework,
                                     const consumer_options_t &options)
{
    framework.configure_dispatch ()
      .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
      .trace_log_file (options.log_dir + "/consumer-flow.log")
      .trace_label ("consumer");
    server::add_redis_location_store (framework, options.redis_endpoint, options.redis_key_prefix);
    framework.services ().add_singleton<consumer_options_t> (
      std::make_unique<consumer_options_t> (options));
    framework.add_client_server_channel (api_channel).client ();
    if (!options.provider_endpoints.empty ()) {
        framework.add_client_server_channel ("resilience.lifecycle.api.manual")
          .client ()
          .connect (options.provider_endpoints.front ());
    }
    if (options.provider_endpoints.size () > 1) {
        framework.add_client_server_channel ("resilience.lifecycle.api.manual.b")
          .client ()
          .connect (options.provider_endpoints[1]);
    }
    framework.http ()
      .listen (options.http_endpoint)
      .configure_server ([] (zlink::framework::http_server_options_builder_t &server) {
          server.set_max_request_body_size (2 * 1024 * 1024);
      })
      .map_health ("/health")
      .map_post<profile_request_handler_t> ("/profile/request")
      .map_post<manual_profile_request_handler_t> ("/profile/request/manual")
      .map_post<manual_b_profile_request_handler_t> ("/profile/request/manual-b")
      .map_post<slow_request_handler_t> ("/profile/request/timeout/100")
      .map_post<missing_request_handler_t> ("/profile/request/missing")
      .map_post<profile_command_handler_t> ("/profile/command")
      .map_post<missing_profile_command_handler_t> ("/profile/command/missing")
      .map_post<new_client_profile_request_handler_t> ("/profile/request/new-client")
      .map_post<recreated_mesh_request_handler_t> (
        "/route/request/recreated-mesh")
      .map_get<topology_handler_t> ("/topology")
      .map_post<topology_wait_handler_t> ("/topology/wait");
}

} // namespace zlink::framework::e2e::resilience_lifecycle::consumer
