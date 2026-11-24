/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/runtime_metric_contract.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <opentelemetry/metrics/noop.h>
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
int actor_native_terminal_cause (std::string_view cause,
                                 zlink::framework::message_flow_log_mode_t mode,
                                 bool meter)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace rt = zlink::framework::runtime;
    namespace host = rt::host;
    namespace mesh = rt::mesh;
    namespace msg = rt::messaging;
    using namespace std::chrono_literals;
    completion_owner_t completion_owner;
    rt::install_host_context_hooks ();
    metric_test::provider_t metrics;
    auto builder = test::runtime_failure_builder ();
    builder.add_route_mesh ("receiver-mesh")
      .set_routing_id (zlink::routing_id_t::from ("receiver-node"))
      .listen (0);
    builder.add_route_mesh ("source-mesh")
      .set_routing_id (zlink::routing_id_t::from ("source-mesh-node"))
      .listen (0);
    auto registration = mesh_node_runtime_t::registrations (builder).front ();
    assert (registration->spot_state->snapshot.name == "receiver-mesh");
    assert (mesh_node_runtime_t::registrations (builder).back ()->spot_state->snapshot.name
            == "source-mesh");
    registration->core_context = std::make_shared<zlink::context_t> ();
    auto node = registration->spot_state;
    node->monitoring = test::runtime_failure_monitoring ();
    if (!meter)
        node->monitoring->use_metric_provider (
          opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider> (
            std::make_shared<opentelemetry::metrics::NoopMeterProvider> ()));
    const bool disconnect = cause.starts_with ("disconnect_");
    const bool replay = cause.starts_with ("replay_");
    const bool malformed = cause == "replay_invalid_context" || cause == "replay_invalid_header";
    const auto observer_failures_before = dispatch_error_reporter_t::observer_failures ();
    const bool forwarded = cause == "follow";
    const bool parked = cause == "park";
    const bool stale = cause == "stale_target" || cause == "disconnect_stale_target"
                       || cause == "replay_stale_target" || cause == "replay_invalid_context";
    const bool application_failure = cause == "application_not_found";
    const bool request = cause == "request_no_handler";
    const std::string reason = stale ? "stale_target" : "no_handler";
    auto worker = std::make_shared<rt::offload_executor_t> (1, "terminal-cause");
    node->worker_executor = worker;
    serializer_registry_t serializers;
    auto context = std::make_shared<spot_context_state_t> ();
    context->node = node;
    context->lane_owner = node;
    context->node_rid = node_rid_t::from_string ("receiver-node");
    context->mesh_name = "receiver-mesh";
    context->spot_id = spot_id_t ("terminal-spot");
    context->spot_name = "terminal-spot";
    context->spot_instance = std::make_shared<int> (1);
    context->serial_executor = worker;
    context->channel_runtime = test::runtime_failure_fixture<channel_runtime_state_t> ();
    context->channel_runtime->serializers = &serializers;
    node->spot_contexts_by_id.emplace (context->spot_id, spot_context_access_t::create (context));
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));
    auto actor = test_actor_ref (replay ? "source-mesh-node" : "receiver-node", "player",
                                 "terminal:actor:2799", 1);
    const std::string expected_actor_id (actor.actor_id ().value ());
    const std::string key = "player:terminal:actor:2799";
    node->actor_instances.emplace (key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (key, context->spot_id);
    node->actor_generations.emplace (key, 1);
    node->actor_types_by_id.emplace ("terminal:actor:2799", "player");
    std::atomic_int handler_calls{0};
    context->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_send, "KnownPacket", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    context->handler_invokers.push_back (
      [&] (void *, void *, service_provider_t &, serializer_registry_t &, const zlink::message_t &,
           const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          ++handler_calls;
          co_return result_t<zlink::message_t>::failure (framework_error_kind_t::not_found,
                                                         "application own not_found");
      });
    dispatch_options_t diagnostics;
    diagnostics.message_flow (mode);
    auto error_observations =
      std::make_shared<test::telemetry_observations_t<message_dispatch_error_event_t>> ();
    auto flow_observations =
      std::make_shared<test::telemetry_observations_t<message_flow_event_t>> ();
    dispatch_options_access_t::set_observer_for_tests (
      diagnostics, [flow_observations] (const auto &event) { flow_observations->record (event); });
    dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      diagnostics,
      [error_observations] (const auto &event) { error_observations->record (event); });
    node->dispatch = diagnostics;
    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    auto owned_store = std::make_unique<rt::in_memory_location_repository_t> ();
    auto &store = *owned_store;
    services.add_singleton<location_repository_t> (
      std::unique_ptr<location_repository_t> (owned_store.release ()));
    services.add_singleton<rt::location_runtime_t> (
      std::make_unique<rt::location_runtime_t> (store));
    services.add_factory<rt::live_location_reader_t> (
      [] (service_provider_t &p) {
          return std::make_unique<rt::live_location_reader_t> (
            p.get_required<location_repository_t> ());
      },
      service_lifetime_t::singleton);
    auto resolvers = std::make_shared<rt::store_location_resolvers_t> (store);
    services.add_factory<rt::spot_address_resolver_t> (
      [resolvers] (auto &) {
          return std::static_pointer_cast<rt::spot_address_resolver_t> (resolvers);
      },
      service_lifetime_t::singleton);
    services.add_factory<rt::actor_address_resolver_t> (
      [resolvers] (auto &) {
          return std::static_pointer_cast<rt::actor_address_resolver_t> (resolvers);
      },
      service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    node->root_services = provider;
    auto &location = provider.get_required<rt::location_runtime_t> ();
    location.start (*registration->routing_id);
    rt::mesh_node_host_service_t service (node->runtime_failures (), {registration}, serializers,
                                          diagnostics);
    service.start (provider).result ().value ();
    auto runtime = service.nodes ().front ();
    std::atomic_int retained{0}, released{0}, terminals{0};
    auto follow_completion =
      std::make_shared<task_completion_source_t<std::optional<zlink::message_t>>> ();
    std::promise<void> follow_entered;
    auto following = follow_entered.get_future ();
    std::atomic_int follow_calls{0};
    std::promise<void> terminal;
    auto completed = terminal.get_future ();
    msg::envelope_header_t header;
    header.kind = request ? msg::message_kind_t::request : msg::message_kind_t::command;
    header.message_name = disconnect ? rt::protocol::session_disconnected_packet_name
                          : application_failure || stale ? "KnownPacket"
                                                         : "UnknownPacket";
    header.flow_id = "01890a5d-ac96-774b-bcce-b302099a8057";
    header.flow_origin = flow_origin_t::application;
    header.channel_name = "actor";
    const rt::protocol::actor_route_fence_t fence{
      "terminal:actor:2799", 1, registration->routing_id->to_bytes (), 1, 1, 1};
    host::receive_record_t record{.kind = request ? host::record_kind_t::actor_request
                                                  : host::record_kind_t::actor_send,
                                  .domain = host::ready_domain_t::application};
    record.source_node_rid = zlink::routing_id_t::from ("source-mesh-node");
    const auto payload =
      disconnect ? zlink::message_t::from (
                     rt::protocol::encode_session_disconnected (
                       {zlink::routing_id_t::from ("source-session-owner").to_bytes (), 2, 7}))
                     .to_string ()
                 : std::string ("payload");
    record.application_record = accepted_actor_fifo_record (fence, 1, header, payload);
    record.retain_mailbox_reservation = [&] {
        ++retained;
        // Host invokes this only after the Actor queue has taken this record.
        if (stale)
            node->lane.run ([&] { node->spot_contexts_by_id.erase (context->spot_id); }).get ();
    };
    record.release_mailbox_reservation = [&] { ++released; };
    record.complete_stateful_dispatch = [&] {
        if (++terminals == 1)
            terminal.set_value ();
    };
    host::ready_record_t owner{.owner_kind = host::owner_kind_t::actor,
                               .domain = host::ready_domain_t::application,
                               .spot_id = std::string (context->spot_id),
                               .actor = actor};
    auto encoded = encode_actor_packet (header, zlink::message_t::from (payload));
    std::atomic_int takes{0};
    record.before_application_handler = [] {};
    spot_node_runtime_t spots (node);
    if (forwarded) {
        auto target_fence = fence;
        target_fence.target_node_routing_id = zlink::routing_id_t::from ("next-owner").to_bytes ();
        node->actor_transfer_coordinator->activate_message_follow (
          key, fence, test_actor_ref ("next-owner", "player", "terminal:actor:2799", 1),
          spot_route_t{node_rid_t::from_string ("next-owner"), spot_id_t ("next-spot"),
                       "source-mesh"},
          target_fence, std::chrono::steady_clock::now () + 30s, "terminal-follow");
        spots.on_actor_message_follow (
          [&] (const actor_ref_t &, const msg::envelope_header_t &, const zlink::message_t &,
               std::chrono::milliseconds, const zlink::routing_id_t &,
               const rt::protocol::actor_route_fence_t &, std::uint8_t,
               const rt::protocol::wire_operation_id_t &, std::uint64_t) {
              if (++follow_calls == 1)
                  follow_entered.set_value ();
              return follow_completion->task ();
          });
    }
    if (parked || replay)
        assert (node->actor_transfer_coordinator->try_begin_local (key));
    bool accepted = false;
    if (replay) {
        auto original = record.application_record;
        if (malformed) {
            auto &identity = *original->payload ()->application_record;
            std::visit (
              [] (auto &body) {
                  body.application = rt::protocol::application_payload_t::from_parts (
                    std::vector<zlink::message_t>{zlink::message_t::from ("invalid-header"),
                                                  zlink::message_t::from ("payload")});
              },
              identity.body);
        }
        original->transferred = [&] { ++released; };
        original->native_reply = [&] (auto) {
            if (++terminals == 1)
                terminal.set_value ();
            return true;
        };
        const auto appended = node->actor_transfer_coordinator->try_append_backlog (
          key, handoff_packet_t{header.message_name, {}, header.content_type, {}, false, original});
        assert (appended == handoff_append_result_t::appended);
        // The backlog already returned its original claim. This callback pins
        // the later prefix admission, after open has retained its target queue.
        original->admission.retain_initial ([&] {
            ++retained;
            if (stale)
                node->lane.run ([&] { node->spot_contexts_by_id.erase (context->spot_id); }).get ();
        });
        const auto opened = spots.open_local_actor_handoff (actor).result ();
        accepted = opened && opened.value ();
    } else {
        accepted = runtime->native_node ()
                     .transport ()
                     .mailbox ()
                     .notify_stateful_ready ("terminal:actor:2799",
                                             [&, supplied = false] (const auto &dispatch) mutable {
                                                 if (std::exchange (supplied, true))
                                                     return false;
                                                 ++takes;
                                                 dispatch (owner, record,
                                                           std::move (encoded).take_items (), {});
                                                 return true;
                                             })
                     .result ()
                     .value ();
    }
    if (forwarded) {
        assert (following.wait_for (5s) == std::future_status::ready);
        {
            test::acknowledge_delivery (std::chrono::steady_clock::now () + 5s);
            assert (error_observations->snapshot ().empty ());
        }
        for (const auto &fields : metrics.collect_fields ())
            assert (fields.at ("name") != "zlink.mesh_node.messages.dropped");
        follow_completion->complete (
          result_t<std::optional<zlink::message_t>>::success (std::nullopt));
    }
    if (parked) {
        assert (completed.wait_for (5s) == std::future_status::ready);
        auto backlog = spots.take_actor_handoff_backlog (actor);
        assert (backlog.size () == 1);
        assert (backlog.front ().application_record == record.application_record);
        assert (backlog.front ().packet_name == header.message_name);
        assert (!backlog.front ().is_request);
        {
            test::acknowledge_delivery (std::chrono::steady_clock::now () + 5s);
            assert (error_observations->snapshot ().empty ());
        }
        for (const auto &fields : metrics.collect_fields ())
            assert (fields.at ("name") != "zlink.mesh_node.messages.dropped");
        assert (
          node->actor_transfer_coordinator->try_append_backlog (key, std::move (backlog.front ()))
          == handoff_append_result_t::appended);
        spots.cancel_local_actor_handoff (actor);
    }
    const auto ready = completed.wait_for (5s) == std::future_status::ready;
    const auto drained =
      service.wait_for_accepted_callbacks_until (std::chrono::steady_clock::now () + 5s);
    const auto observer_failure_delta =
      dispatch_error_reporter_t::observer_failures () - observer_failures_before;
    test::acknowledge_delivery (std::chrono::steady_clock::now () + 5s);
    const auto errors = error_observations->snapshot ();
    const auto flows = flow_observations->snapshot ();
    service.stop ();
    location.stop ();
    worker->drain ();
    node->worker_executor.reset ();
    unsigned reported = 0;
    unsigned opposite_cause = 0;
    for (const auto &event : errors) {
        if (event.reason
            == (stale ? dispatch_error_reason_t::handler_missing
                      : dispatch_error_reason_t::stale_target))
            ++opposite_cause;
        if (event.reason
            == (stale ? dispatch_error_reason_t::stale_target
                      : dispatch_error_reason_t::handler_missing)) {
            ++reported;
            std::cout << "dispatch_actor_id=" << event.actor_id.value_or ("<none>")
                      << " expected_actor_id=" << expected_actor_id << std::endl;
            assert (event.actor_id == expected_actor_id);
            assert (event.mesh_name == "receiver-mesh");
            const bool tracing = rt::flow_context_t::tracing (mode);
            assert (event.flow_id.has_value () == tracing);
            assert (event.flow_origin.has_value () == tracing);
            if (tracing) {
                assert (event.flow_id == header.flow_id);
                assert (event.flow_origin == header.flow_origin);
            }
        }
    }
    unsigned received_events = 0;
    for (const auto &event : flows)
        if (event.detail_stage == "invoke_erased.enter") {
            ++received_events;
            std::cout << "received_actor_id=" << event.actor_id.value_or ("<none>")
                      << " expected_actor_id=" << expected_actor_id << std::endl;
            assert (event.actor_id == expected_actor_id);
        }
    if (application_failure && mode == message_flow_log_mode_t::detailed)
        assert (received_events == 1);
    double drops = 0;
    for (const auto &fields : metrics.collect_fields ())
        if (fields.at ("name") == "zlink.mesh_node.messages.dropped") {
            assert (fields.at ("mesh_name") == "receiver-mesh");
            assert (fields.at ("reason") == reason);
            assert (fields.at ("surface") == "actor");
            assert (fields.at ("message_kind") == "send");
            assert (fields.at ("unit") == "{message}");
            assert (fields.at ("instrument_kind") == "counter");
            drops += std::stod (fields.at ("value"));
        }
    std::cout << "cause=" << cause << " mode=" << static_cast<int> (mode) << " meter=" << meter
              << " accepted=" << accepted << " terminal_ready=" << ready << " drained=" << drained
              << " takes=" << takes << " retained=" << retained << " released=" << released
              << " terminals=" << terminals << " handler=" << handler_calls
              << " reported=" << reported << " opposite_cause=" << opposite_cause
              << " total_errors=" << errors.size () << " drops=" << drops << std::endl;
    if (!accepted || !ready || !drained || (!replay && takes != 1) || retained != 1 || released != 1
        || terminals != 1)
        return 2;
    const unsigned expected_reports = mode != message_flow_log_mode_t::off && !application_failure
                                      && !request && !forwarded && !parked && !malformed;
    const double expected_drops =
      meter && !application_failure && !request && !forwarded && !parked && (!malformed || stale)
        ? 1
        : 0;
    if (malformed)
        assert (observer_failure_delta == (stale && mode != message_flow_log_mode_t::off ? 1 : 0));
    return handler_calls == (application_failure ? 1 : 0) && reported == expected_reports
               && opposite_cause == 0 && errors.size () == expected_reports
               && drops == expected_drops
             ? 0
             : 1;
}
} // namespace zlink::framework::test::actor_gateway
