/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "../support/location_reclaim_clock.hpp"
#include "../support/manual_deadlines.hpp"
#include "../support/telemetry_observation.hpp"
#include "metric_test_reader.hpp"
#include <zlink/framework.hpp>
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/failure_origin_wire.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <gtest/gtest.h>
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime_context_hooks_environment.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <future>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <type_traits>
#include <typeinfo>
#include <vector>
#include "../support/instance_spot_activation_deadline_fixture.hpp"

using namespace framework_instance_spot_activation_fixture;

namespace zlink::framework::runtime::host
{
struct public_host_runtime_test_access_t
{
    static bool expire_activation (public_host_runtime_t &host, const std::string &spot_id)
    {
        const auto operation =
          host._lifecycle_configuration_lane
            .run ([&] { return host._instance_spot_activation_admissions.at (spot_id); })
            .get ();
        return operation->admission.complete (
          result_t<public_host_runtime_t::instance_activation_admission_t>::failure (
            framework_error_kind_t::deadline_exceeded, "Activation admission expired"));
    }

    static task_t<void> activate (public_host_runtime_t &host,
                                  protocol::instance_activation_recovery_t command,
                                  std::function<void (instance_spot_activation_result_t)> done)
    {
        return host.dispatch_instance_spot_activation (std::move (command), {}, std::move (done));
    }
};
} // namespace zlink::framework::runtime::host

enum class activation_failure_t
{
    deadline,
    factory,
    shutdown,
    accepted_handler,
    initialization,
    conflict,
    store_failure,
    fenced,
    cancelled,
    non_std_factory
};

static void verify_activation_failure_terminal (bool local,
                                                bool provider,
                                                activation_failure_t failure,
                                                bool registered_type = true,
                                                bool hold_delivery = false,
                                                bool expire_admission = false)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    namespace host = rt::host;
    metric_test::provider_t metrics;
    const bool shutdown_failure =
      failure == activation_failure_t::shutdown || failure == activation_failure_t::cancelled;
    const auto expected_kind =
      failure == activation_failure_t::deadline || failure == activation_failure_t::initialization
        ? fw::framework_error_kind_t::deadline_exceeded
      : shutdown_failure                          ? fw::framework_error_kind_t::shutting_down
      : failure == activation_failure_t::conflict ? fw::framework_error_kind_t::already_exists
      : failure == activation_failure_t::fenced   ? fw::framework_error_kind_t::invalid_operation
                                                  : fw::framework_error_kind_t::internal_failure;
    auto opaque = std::make_shared<rt::in_memory_location_store_t> ();
    std::shared_ptr<fw::location_repository_t> store =
      provider ? std::static_pointer_cast<fw::location_repository_t> (
                   std::make_shared<rt::provider_location_repository_t> (*opaque))
               : std::static_pointer_cast<fw::location_repository_t> (
                   std::make_shared<rt::in_memory_location_repository_t> ());
    store->configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto owner = std::get<fw::owner_lease_claimed_t> (
                         store->claim_owner_lease ("terminal-owner", 60s).result ().value ())
                         .token;
    fw::mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "terminal";
    descriptor.rid = zlink::routing_id_t::from ("terminal-target");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7101";
    descriptor.security_identity = "terminal-owner";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.object_role = fw::object_role_t::server;
    descriptor.state = fw::framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back ({fw::placement_object_kind_t::instance_spot,
                                               "room-type", fw::maintenance_policy_kind_t::disabled,
                                               false, 1});
    descriptor.capacity.spots.limit = 1;
    descriptor.capacity.spot_types.push_back (
      {fw::placement_object_kind_t::instance_spot, "room-type", {0, 0, 1}});
    ASSERT_EQ (fw::location_write_status_t::stored,
               store->update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
    auto blob_store = std::make_shared<rt::in_memory_relocation_store_t> ();
    auto blobs = std::make_shared<rt::provider_relocation_repository_t> (*blob_store);
    auto relocations = std::make_shared<rt::stateful::public_relocation_store_adapter_t> (blobs);
    auto options = host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "terminal",
                              .node_routing_id = descriptor.rid.to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<rt::runtime_failure_collector_t> ()}};
    options.mesh.metric_provider = metrics.api ();
    options.mesh.dispatch.message_flow (fw::message_flow_log_mode_t::normal);
    auto flows = std::make_shared<fw::test::telemetry_observations_t<fw::message_flow_event_t>> ();
    auto diagnostics =
      std::make_shared<fw::test::telemetry_observations_t<fw::message_dispatch_error_event_t>> ();
    fw::detail::dispatch_options_access_t::set_observer_for_tests (
      options.mesh.dispatch, [flows] (const auto &event) {
          if (event.surface == fw::dispatch_error_surface_t::instance_spot
              && event.outcome == fw::message_flow_outcome_t::dropped)
              flows->record (event);
      });
    fw::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      options.mesh.dispatch, [diagnostics] (const auto &event) { diagnostics->record (event); });
    auto target = std::make_shared<host::public_host_runtime_t> (std::move (options));
    int dispatches = 0;
    int cleanups = 0;
    std::exception_ptr terminal_exception;
    std::promise<void> preparation_cleaned;
    auto delivery_release = std::make_shared<fw::task_completion_source_t<void>> ();
    auto delivery_completed = std::make_shared<std::promise<void>> ();
    std::promise<void> initialization_entered;
    auto initialization_release = std::make_shared<fw::task_completion_source_t<void>> ();
    const bool deadline_failure =
      failure == activation_failure_t::deadline || failure == activation_failure_t::initialization;
    target->configure_instance_spot_operations (
      store, relocations, [owner] { return owner; },
      host::instance_spot_activation_materializer_t{
        [&] (const auto &request, const auto &, auto &cleanup) -> zlink::framework::task_t<bool> {
            cleanup = [&] (bool) -> fw::task_t<void> {
                ++cleanups;
                preparation_cleaned.set_value ();
                co_return;
            };
            if (failure == activation_failure_t::factory)
                throw std::runtime_error ("factory terminal failure");
            if (failure == activation_failure_t::non_std_factory)
                throw 7;
            if (failure == activation_failure_t::cancelled)
                throw std::system_error (std::make_error_code (std::errc::operation_canceled));
            if (failure == activation_failure_t::conflict
                || failure == activation_failure_t::store_failure
                || failure == activation_failure_t::fenced)
                throw fw::framework_exception_t (expected_kind, "typed activation failure");
            if (failure == activation_failure_t::initialization) {
                initialization_entered.set_value ();
                co_await initialization_release->task ();
            }
            if (failure == activation_failure_t::deadline)
                advance_activation_deadline (request.target.deadline_unix_ms).get ();
            co_return true;
        },
        [&] (auto, auto,
             auto accept_activation) -> fw::task_t<host::instance_spot_activation_result_t> {
            accept_activation ();
            ++dispatches;
            co_return host::instance_spot_activation_result_t{
              static_cast<std::uint32_t> (rt::protocol::request_terminal_result::internalError),
              static_cast<std::uint32_t> (rt::protocol::framework_error_code::requestFailed),
              std::nullopt, [] {}};
        },
        {},
        [registered_type] (
          const std::string &type) -> std::optional<fw::detail::factory_relocation_kind_t> {
            return registered_type && type == "room-type"
                     ? std::make_optional (fw::detail::factory_relocation_kind_t::disabled)
                     : std::nullopt;
        }});
    target->start ();
    auto cleanup = std::unique_ptr<host::public_host_runtime_t,
                                   std::function<void (host::public_host_runtime_t *)>> (
      target.get (), [target, initialization_release, delivery_release] (auto *) {
          initialization_release->complete (fw::result_t<void>::success ());
          delivery_release->complete (fw::result_t<void>::success ());
          target->close ();
      });
    if (failure == activation_failure_t::shutdown)
        target->close ();
    const auto deadline =
      std::chrono::system_clock::now () + (deadline_failure && !expire_admission ? 100ms : 3s);
    rt::protocol::instance_spot_activation_header_t header{
      {descriptor.rid.to_bytes (), 1, "terminal-room", "terminal", "room-type", "1",
       static_cast<std::uint64_t> (
         std::chrono::duration_cast<std::chrono::milliseconds> (deadline.time_since_epoch ())
           .count ())},
      1,
      descriptor.rid.to_bytes (),
      std::nullopt,
      false,
      {2242, 1},
      0,
      false};
    const std::string flow_id = "019fc5b9-9df3-786b-bb69-d55358f6d48b";
    rt::protocol::application_payload_t payload{"probe", "application/json", {1}};
    payload.flow_id = flow_id;
    payload.flow_origin = fw::flow_origin_t::application;
    std::chrono::steady_clock::time_point observation_deadline;
    if (local) {
        auto terminals = std::make_shared<int> (0);
        auto accepted_terminals = std::make_shared<int> (0);
        auto terminal_ready = std::make_shared<std::promise<void>> ();
        auto run = [&] {
            auto activation = host::public_host_runtime_test_access_t::activate (
              *target, {header, std::nullopt, payload},
              [terminals, accepted_terminals, terminal_ready, failure, expected_kind, hold_delivery,
               delivery_release, delivery_completed, &terminal_exception] (const auto &result) {
                  ++*terminals;
                  terminal_exception = result.exception;
                  terminal_ready->set_value ();
                  // This is the actual local operation result, before telemetry projection.
                  // Remote send rows independently cover admission and owned diagnostics.
                  if (failure != activation_failure_t::accepted_handler) {
                      EXPECT_TRUE (result.exception);
                      if (result.exception) {
                          try {
                              std::rethrow_exception (result.exception);
                          }
                          catch (const fw::framework_exception_t &cause) {
                              EXPECT_NE (activation_failure_t::factory, failure);
                              EXPECT_NE (activation_failure_t::cancelled, failure);
                              EXPECT_NE (activation_failure_t::non_std_factory, failure);
                              EXPECT_EQ (expected_kind, cause.kind ());
                          }
                          catch (const std::system_error &cause) {
                              EXPECT_EQ (activation_failure_t::cancelled, failure);
                              EXPECT_EQ (std::make_error_code (std::errc::operation_canceled),
                                         cause.code ());
                          }
                          catch (const std::runtime_error &cause) {
                              EXPECT_EQ (activation_failure_t::factory, failure);
                              EXPECT_TRUE (typeid (std::runtime_error) == typeid (cause));
                              EXPECT_STREQ ("factory terminal failure", cause.what ());
                          }
                          catch (int cause) {
                              EXPECT_EQ (activation_failure_t::non_std_factory, failure);
                              EXPECT_EQ (7, cause);
                          }
                          catch (...) {
                              ADD_FAILURE () << "Unexpected local activation cause type";
                          }
                      }
                  }
                  const auto error =
                    rt::messaging::request_failure_mapper_t{}.reply_header_exception (
                      result.terminal_result, result.failure_code, "Activation regression");
                  EXPECT_EQ (expected_kind, error.kind ());
                  EXPECT_EQ (failure == activation_failure_t::accepted_handler,
                             static_cast<bool> (result.accepted_turn_terminal));
                  if (result.accepted_turn_terminal) {
                      ++*accepted_terminals;
                      result.accepted_turn_terminal ();
                  }
                  if (hold_delivery) {
                      (void) delivery_release->task ().result ();
                      delivery_completed->set_value ();
                  }
              });
            EXPECT_TRUE (activation.result ());
        };
        if (failure == activation_failure_t::initialization) {
            auto running = std::async (std::launch::async, run);
            // Release stalled test stimuli before running's destructor can join on failure.
            auto release_on_exit = std::unique_ptr<void, std::function<void (void *)>> (
              target.get (), [initialization_release, delivery_release] (void *) {
                  initialization_release->complete (fw::result_t<void>::success ());
                  delivery_release->complete (fw::result_t<void>::success ());
              });
            EXPECT_EQ (std::future_status::ready,
                       initialization_entered.get_future ().wait_for (1s));
            std::future<bool> expired;
            if (expire_admission)
                expired = std::async (std::launch::async, [&] {
                    return host::public_host_runtime_test_access_t::expire_activation (
                      *target, "terminal-room");
                });
            else
                expired = advance_activation_deadline (header.target.deadline_unix_ms);
            EXPECT_EQ (std::future_status::ready, terminal_ready->get_future ().wait_for (1s));
            initialization_release->complete (fw::result_t<void>::success ());
            if (hold_delivery) {
                EXPECT_EQ (std::future_status::ready,
                           preparation_cleaned.get_future ().wait_for (1s));
                EXPECT_EQ (std::future_status::timeout, running.wait_for (100ms));
                delivery_release->complete (fw::result_t<void>::success ());
                EXPECT_EQ (std::future_status::ready,
                           delivery_completed->get_future ().wait_for (1s));
            }
            running.get ();
            if (expired.valid ())
                EXPECT_TRUE (expired.get ());
        } else
            run ();
        EXPECT_EQ (1, *terminals);
        EXPECT_EQ (failure == activation_failure_t::accepted_handler ? 1 : 0, *accepted_terminals);
        observation_deadline = std::chrono::steady_clock::now () + 3s;
    } else {
        ASSERT_TRUE (
          target->send_instance_spot_activation_remote (descriptor.rid, header, {}, payload)
            .result ()
            .value ());
        observation_deadline = std::chrono::steady_clock::now () + 3s;
        while (diagnostics->snapshot ().empty ()
               && std::chrono::steady_clock::now () < observation_deadline) {
            ASSERT_TRUE (
              target->dispatch_ready ([] (const auto &, const auto &, auto, auto) {}, false)
                .result ());
            std::this_thread::yield ();
        }
    }
    // Local activate().result() completes the producer. Remotely, dispatch diagnostics
    // follow record_activation, so their delivery also orders the preceding flow submission.
    {
        std::unique_lock lock (diagnostics->mutex);
        ASSERT_TRUE (diagnostics->changed.wait_until (
          lock, observation_deadline, [&] { return !diagnostics->events.empty (); }));
    }
    fw::test::acknowledge_delivery (observation_deadline);
    const auto events = flows->snapshot ();
    const auto errors = diagnostics->snapshot ();
    target->close ();
    int activation_count = 0;
    int duration_count = 0;
    const std::string expected_outcome = failure == activation_failure_t::accepted_handler ? "ready"
                                         : deadline_failure                          ? "timed_out"
                                         : shutdown_failure                          ? "shutdown"
                                         : failure == activation_failure_t::conflict ? "conflict"
                                         : failure == activation_failure_t::fenced   ? "fenced"
                                         : failure == activation_failure_t::store_failure
                                           ? "store_failure"
                                           : "rejected";
    for (const auto &metric : metrics.collect ()) {
        if (metric.instrument_descriptor.name_ != "zlink.instance_spot.activations"
            && metric.instrument_descriptor.name_ != "zlink.instance_spot.activation.duration")
            continue;
        for (const auto &point : metric.point_data_attr_) {
            EXPECT_EQ (3u, point.attributes.size ());
            EXPECT_EQ ("terminal",
                       opentelemetry::nostd::get<std::string> (point.attributes.at ("mesh_name")));
            EXPECT_EQ ("room-type", opentelemetry::nostd::get<std::string> (
                                      point.attributes.at ("instance_spot_type")));
            EXPECT_EQ (expected_outcome,
                       opentelemetry::nostd::get<std::string> (point.attributes.at ("outcome")));
            if (auto sum = opentelemetry::nostd::get_if<metric_test::sdk::SumPointData> (
                  &point.point_data)) {
                EXPECT_TRUE (sum->is_monotonic_);
                EXPECT_DOUBLE_EQ (1, opentelemetry::nostd::get<double> (sum->value_));
                EXPECT_EQ ("{activation}", metric.instrument_descriptor.unit_);
                ++activation_count;
            } else if (auto histogram =
                         opentelemetry::nostd::get_if<metric_test::sdk::HistogramPointData> (
                           &point.point_data)) {
                EXPECT_EQ (1u, histogram->count_);
                EXPECT_GE (opentelemetry::nostd::get<double> (histogram->sum_), 0);
                EXPECT_EQ ("s", metric.instrument_descriptor.unit_);
                ++duration_count;
            }
        }
    }
    EXPECT_EQ (registered_type ? 1 : 0, activation_count);
    EXPECT_EQ (registered_type ? 1 : 0, duration_count);
    const auto &dropped = events;
    if (failure == activation_failure_t::accepted_handler) {
        EXPECT_TRUE (dropped.empty ());
        EXPECT_EQ (1, dispatches);
        EXPECT_EQ (1, cleanups);
        EXPECT_EQ (1u, errors.size ());
        return;
    }
    ASSERT_EQ (1u, dropped.size ());
    const auto &event = dropped.front ();
    EXPECT_EQ (fw::dispatch_error_action_t::drop, event.error_action);
    EXPECT_EQ (flow_id, event.flow_id);
    EXPECT_EQ (fw::flow_origin_t::application, event.flow_origin);
    EXPECT_EQ (deadline_failure   ? fw::message_flow_reason_t::activation_timeout
               : shutdown_failure ? fw::message_flow_reason_t::shutdown
                                  : fw::message_flow_reason_t::activation_rejected,
               event.reason);
    EXPECT_EQ (fw::message_flow_result_t::dropped, event.result);
    ASSERT_TRUE (event.exception);
    if (local)
        EXPECT_EQ (terminal_exception, event.exception);
    try {
        std::rethrow_exception (event.exception);
    }
    catch (const std::exception &error) {
        if (failure == activation_failure_t::factory)
            EXPECT_STREQ ("factory terminal failure", error.what ());
        else if (failure == activation_failure_t::cancelled) {
            const auto *cancelled = dynamic_cast<const std::system_error *> (&error);
            ASSERT_NE (nullptr, cancelled);
            EXPECT_EQ (std::make_error_code (std::errc::operation_canceled), cancelled->code ());
        } else {
            const auto *typed = dynamic_cast<const fw::framework_exception_t *> (&error);
            ASSERT_NE (nullptr, typed);
            EXPECT_EQ (expected_kind, typed->kind ());
        }
    }
    catch (int error) {
        EXPECT_EQ (activation_failure_t::non_std_factory, failure);
        EXPECT_EQ (7, error);
    }
    EXPECT_EQ (0, dispatches);
    EXPECT_EQ (failure == activation_failure_t::shutdown ? 0 : 1, cleanups);
    ASSERT_EQ (1u, errors.size ());
    const auto &error = errors.front ();
    EXPECT_EQ (event.exception, error.exception);
    EXPECT_EQ (fw::dispatch_error_surface_t::instance_spot, error.surface);
    EXPECT_EQ (fw::dispatch_message_kind_t::send, error.message_kind);
    EXPECT_EQ (fw::dispatch_error_action_t::drop, error.action);
    EXPECT_EQ (flow_id, error.flow_id);
    EXPECT_EQ (fw::flow_origin_t::application, error.flow_origin);
    EXPECT_EQ (failure == activation_failure_t::factory     ? typeid (std::runtime_error).name ()
               : failure == activation_failure_t::cancelled ? typeid (std::system_error).name ()
               : failure == activation_failure_t::non_std_factory
                 ? "non-standard exception"
                 : typeid (fw::framework_exception_t).name (),
               error.error_type);
    if (failure == activation_failure_t::factory)
        EXPECT_EQ ("factory terminal failure", error.error_message);
    else if (failure == activation_failure_t::non_std_factory)
        EXPECT_EQ ("non-standard exception", error.error_message);
    else if (failure == activation_failure_t::conflict
             || failure == activation_failure_t::store_failure
             || failure == activation_failure_t::fenced)
        EXPECT_EQ ("typed activation failure", error.error_message);
    else if (failure == activation_failure_t::cancelled)
        EXPECT_EQ (std::make_error_code (std::errc::operation_canceled).message (),
                   error.error_message);
    else if (failure == activation_failure_t::shutdown)
        EXPECT_EQ ("Instance Spot activation target stopped", error.error_message);
    else if (deadline_failure)
        EXPECT_EQ (expire_admission ? "Activation admission expired"
                                    : "Instance Spot activation deadline expired",
                   error.error_message);
    if (failure != activation_failure_t::shutdown) {
        const auto authority =
          store->read_authority (rt::spot_authority_key ("terminal-room")).result ().value ();
        EXPECT_FALSE (std::holds_alternative<fw::authority_snapshot_t> (authority));
    }
}

TEST (ZLinkFrameworkInstanceSpotActivation, ActivationTaskWaitsForTerminalDelivery)
{
    verify_activation_failure_terminal (true, true, activation_failure_t::initialization, true,
                                        true);
}

TEST (ZLinkFrameworkInstanceSpotActivation, TerminalAdmissionPreventsLatePreparationDispatch)
{
    verify_activation_failure_terminal (true, true, activation_failure_t::initialization, true,
                                        true, true);
}

TEST (ZLinkFrameworkInstanceSpotActivation, NonStandardFactoryFailureRemainsRejectedAndObservable)
{
    verify_activation_failure_terminal (true, true, activation_failure_t::non_std_factory);
}

TEST (ZLinkFrameworkInstanceSpotActivation, UnregisteredTypeDoesNotCreateActivationMetricSeries)
{
    verify_activation_failure_terminal (true, true, activation_failure_t::factory, false);
}

TEST (ZLinkFrameworkInstanceSpotActivation, CancelledPreparationKeepsShutdownMetricAndDrop)
{
    verify_activation_failure_terminal (true, true, activation_failure_t::cancelled);
}

TEST (ZLinkFrameworkInstanceSpotActivation, ConflictActivationMetrics)
{
    verify_activation_failure_terminal (true, true, activation_failure_t::conflict);
}

TEST (ZLinkFrameworkInstanceSpotActivation, StoreFailureActivationMetrics)
{
    verify_activation_failure_terminal (true, true, activation_failure_t::store_failure);
}

TEST (ZLinkFrameworkInstanceSpotActivation, FencedActivationMetrics)
{
    verify_activation_failure_terminal (true, true, activation_failure_t::fenced);
}

TEST (ZLinkFrameworkInstanceSpotActivation, InitializationDeadlineEndsBeforePreparationCompletes)
{
    verify_activation_failure_terminal (true, true, activation_failure_t::initialization);
}

TEST (ZLinkFrameworkInstanceSpotActivation, LocalProviderDeadlineDropsOriginalFlowOnce)
{
    verify_activation_failure_terminal (true, true, activation_failure_t::deadline);
}

TEST (ZLinkFrameworkInstanceSpotActivation, RemoteProviderDeadlineDropsOriginalFlowOnce)
{
    verify_activation_failure_terminal (false, true, activation_failure_t::deadline);
}

TEST (ZLinkFrameworkInstanceSpotActivation, InMemoryDeadlineDropsOriginalFlowOnce)
{
    verify_activation_failure_terminal (true, false, activation_failure_t::deadline);
}

TEST (ZLinkFrameworkInstanceSpotActivation, LocalFactoryFailureDropsOriginalErrorOnce)
{
    verify_activation_failure_terminal (true, true, activation_failure_t::factory);
}

TEST (ZLinkFrameworkInstanceSpotActivation, RemoteFactoryFailureDropsOriginalErrorOnce)
{
    verify_activation_failure_terminal (false, true, activation_failure_t::factory);
}

TEST (ZLinkFrameworkInstanceSpotActivation, StoppedTargetKeepsShutdownTerminal)
{
    verify_activation_failure_terminal (true, true, activation_failure_t::shutdown);
}

TEST (ZLinkFrameworkInstanceSpotActivation, AcceptedHandlerFailureDoesNotBecomeActivationDrop)
{
    verify_activation_failure_terminal (true, true, activation_failure_t::accepted_handler);
    verify_activation_failure_terminal (false, true, activation_failure_t::accepted_handler);
}
