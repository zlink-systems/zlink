/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "Configuration/provider_options.hpp"
#include "Endpoints/provider_endpoints.hpp"
#include "Handlers/evidence_dispatch_error_observer.hpp"
#include "Handlers/provider_handlers.hpp"
#include "Infrastructure/evidence_store.hpp"
#include "Infrastructure/fault_state.hpp"
#include "Infrastructure/server_weight_state.hpp"

#include "../../Shared/resilience_lifecycle_messages.hpp"
#include "../Shared/location_store.hpp"

#include <zlink/framework.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>

namespace zlink::framework::e2e::resilience_lifecycle::provider
{

inline void configure_common_codecs (zlink::framework::codec_options_builder_t)
{
}

inline std::string host_from_tcp_endpoint (const std::string &endpoint)
{
    const auto start = endpoint.rfind ("tcp://", 0) == 0 ? 6U : 0U;
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator <= start)
        throw std::invalid_argument ("ClientServer endpoint must use tcp://host:port");
    return endpoint.substr (start, separator - start);
}

inline std::uint16_t port_from_tcp_endpoint (const std::string &endpoint)
{
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator + 1 >= endpoint.size ())
        throw std::invalid_argument ("ClientServer endpoint must use tcp://host:port");
    const auto value = std::stoul (endpoint.substr (separator + 1));
    if (value == 0 || value > 65535)
        throw std::invalid_argument ("ClientServer endpoint port is out of range");
    return static_cast<std::uint16_t> (value);
}

inline void configure_provider_host (zlink::framework::zlink_framework_options_t &framework,
                                     const provider_options_t &options)
{
    framework.configure_dispatch ()
      .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
      .trace_log_file (options.log_dir + "/" + options.rid + "-flow.log")
      .trace_label ("cpp-rl-" + options.rid);
    server::add_redis_location_store (framework, options.redis_endpoint, options.redis_key_prefix);
    auto evidence =
      std::make_shared<evidence_store_t> (options.rid, options.instance_id, options.evidence_file);
    auto fault_state = std::make_shared<fault_state_t> ();
    configure_evidence_dispatch_error_observer (framework, evidence, fault_state);
    framework.services ().add_factory<evidence_store_t> (
      [evidence] (zlink::framework::service_provider_t &) { return evidence; },
      zlink::framework::service_lifetime_t::singleton);
    framework.services ().add_factory<fault_state_t> (
      [fault_state] (zlink::framework::service_provider_t &) { return fault_state; },
      zlink::framework::service_lifetime_t::singleton);
    framework.services ().add_singleton<server_weight_state_t> (
      std::make_unique<server_weight_state_t> ());
    framework.services ().add_transient<route_ping_handler_t, evidence_store_t> ();
    framework.services ().add_transient<server_weight_handler_t,
                                        zlink::framework::channel_runtime_options_t,
                                        evidence_store_t,
                                        server_weight_state_t> ();
    configure_common_codecs (framework.codecs ());
    if (!options.api_endpoint.empty ()) {
        auto server = framework.add_client_server_channel (api_channel).server ();
        server
          .set_bind_host (host_from_tcp_endpoint (options.api_endpoint))
          .set_advertise_host (host_from_tcp_endpoint (options.api_endpoint))
          .listen (port_from_tcp_endpoint (options.api_endpoint))
          .add_handler_group (handler_group);
        if (options.server_weight) {
            server.set_weight (*options.server_weight);
        }
    }
    if (!options.route_endpoint.empty ()) {
        auto route = framework.add_route_mesh (route_channel);
        route.listen (options.route_endpoint)
          .set_routing_id (zlink::routing_id_t::from (options.rid))
          .channel_name (route_channel)
          .server ();
        route.add_route_request_handler<route_ping_handler_t,
                                        scenario_route_req_t,
                                        scenario_route_res_t> ();
    }
    if (!options.http_endpoint.empty ()) {
        framework.http ()
          .listen (options.http_endpoint)
          .map_health ("/health")
          .map_get<evidence_handler_t> ("/evidence")
          .map_post<drain_handler_t> ("/admin/drain")
          .map_post<restore_handler_t> ("/admin/restore")
          .map_post<crash_handler_t> ("/admin/crash")
          .map_post<shutdown_handler_t> ("/shutdown")
          .map_get<weight_handler_t> ("/admin/weight")
          .map_post<weight_wait_handler_t> ("/admin/weight/wait")
          .map_post<server_weight_handler_t> ("/admin/server-weight")
          .map_post<observer_fault_handler_t> ("/admin/fault/observer-throws")
          .map_post<gray_fault_handler_t> ("/admin/fault/gray")
          .map_post<clear_fault_handler_t> ("/admin/fault/none");
    }
    framework.handlers ()
      .group (handler_group)
      .add<profile_request_handler_t> ()
      .add<payload_request_handler_t> ()
      .add_send<profile_command_handler_t> ();
}

} // namespace zlink::framework::e2e::resilience_lifecycle::provider
