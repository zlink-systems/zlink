/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "service_options.hpp"

#include "../Handlers/service_event_recorders.hpp"
#include "../Handlers/service_handlers.hpp"
#include "../../Shared/evidence_store.hpp"
#include "../../Shared/location_store.hpp"
#include "../../../Shared/runtime_monitoring_contracts.hpp"

#include <zlink/framework.hpp>

#include <memory>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::runtime_monitoring::service
{

inline int run_service_host (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    const auto options = read_service_options (app, argc, argv);
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &framework) {
        auto evidence =
          std::make_unique<server::evidence_store_t> (options.rid, options.evidence_file);
        auto *evidence_ptr = evidence.get ();
        framework.services ().add_singleton<server::evidence_store_t> (std::move (evidence));
        framework.services ().add_singleton<runtime_observation_store_t> ();
        auto gate = std::make_unique<application_gate_t> ();
        auto *gate_ptr = gate.get ();
        framework.services ().add_singleton<application_gate_t> (
          std::move (gate));
        server::add_redis_location_store (framework, options.redis_endpoint,
                                          options.redis_key_prefix);
        const auto channel_endpoint = parse_tcp_endpoint (options.channel_endpoint);
        framework.add_client_server_channel (profile_channel)
          .server ()
          .set_bind_host (channel_endpoint.host)
          .set_advertise_host (channel_endpoint.host)
          .listen (channel_endpoint.port)
          .add_handler_group (handler_group);
        if (!options.log_dir.empty ()) {
            framework.configure_dispatch ()
              .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
              .trace_log_file (options.log_dir + "/" + options.rid + "-flow.log")
              .trace_label ("cpp-mon-" + options.rid);
        }
        framework.handlers ().group (handler_group).add<profile_request_handler_t> ();
        auto mesh = framework.add_route_mesh (route_mesh_name);
        mesh.listen (options.mesh_endpoint)
          .set_routing_id (zlink::routing_id_t::from (options.rid));
        mesh.channel_name (route_mesh_channel)
          .server ()
          .use_handler_group (handler_group)
          .add_request_handler<mesh_profile_request_dispatch_handler_t,
                               profile_req_t,
                               profile_res_t> ()
          .add_request_handler<mesh_application_gate_dispatch_handler_t,
                               application_gate_req_t,
                               application_gate_res_t> ();
        mesh.configure_router_socket ().send_high_water_mark =
          zlink::byte_count_t::bytes (1);
        mesh.configure_router_socket ().send_timeout =
          std::chrono::milliseconds (250);
        mesh.configure_router_socket ().mailbox_message_budget = 1;
        mesh.configure_router_socket ().mailbox_byte_budget = 2 * 1024 * 1024;
        mesh.add_spot_factory<monitoring_spot_t> (
          spot_channel,
          [] (zlink::framework::spot_context_t context) {
              return std::make_shared<monitoring_spot_t> (
                std::move (context));
          },
          [] (auto &factory) {
              factory.disable_relocation ();
          });
        mesh.add_spot_factory<monitoring_subject_spot_t> (
          monitoring_subject_spot,
          [] (zlink::framework::spot_context_t context) {
              return std::make_shared<monitoring_subject_spot_t> (
                std::move (context));
          },
          [] (auto &factory) {
              factory.disable_relocation ();
          });
        for (const auto &endpoint : options.mesh_peer_endpoints)
            mesh.peer_connections ().connect (endpoint);
        app.logging ().use_callback_sink (
          [evidence_ptr, profile = options.monitor_profile] (
            const zlink::framework::log_record_t &record) {
              if (profile == "socket-filter"
                  && server::log_field (record, "source_name") == profile_channel
                  && server::log_field (record, "state") != "ready") {
                  return;
              }
              server::record_runtime_log (*evidence_ptr, record);
          });
        if (options.monitor_profile == "throwing") {
            app.logging ().use_callback_sink (
              [evidence_ptr] (const zlink::framework::log_record_t &record) {
                  record_throwing_runtime_log (*evidence_ptr, record);
              });
        }
        if (!options.http_endpoint.empty ()) {
            framework.http ()
              .listen (options.http_endpoint)
              .map_health ("/health")
              .map_get<server::evidence_handler_t> ("/evidence")
              .map_post<server::evidence_wait_handler_t> ("/evidence/wait")
              .map_post<server_weight_handler_t> ("/admin/server-weight")
              .map_post<create_spot_handler_t> ("/spot/create")
              .map_post<create_subject_handler_t> (
                "/admin/subject/create")
              .map_post<close_subject_handler_t> (
                "/admin/subject/close")
              .map_post<publish_probe_handler_t> (
                "/runtime/publish")
              .map_post<runtime_observe_handler_t> ("/runtime/observe")
              .map_post<runtime_observe_isolation_handler_t> (
                "/runtime/observe-isolation")
              .map_get<runtime_snapshot_handler_t> ("/runtime/snapshot")
              .map_post<application_gate_arm_handler_t> (
                "/admin/application-gate/arm")
              .map_post<application_gate_wait_handler_t> (
                "/admin/application-gate/wait")
              .map_post<application_gate_release_handler_t> (
                "/admin/application-gate/release")
              .map_post<mesh_profile_request_handler_t> (
                "/mesh/profile/request")
              .map_post<mesh_application_gate_request_handler_t> (
                "/mesh/application-gate/request")
              .map_post<mesh_weight_handler_t> ("/admin/mesh-weight")
              .map_get<runtime_validation_handler_t> ("/runtime/validation")
              .map_post<shutdown_handler_t> ("/shutdown");
        }
    });
    return app.run (argc, argv);
}

} // namespace zlink::framework::e2e::runtime_monitoring::service
