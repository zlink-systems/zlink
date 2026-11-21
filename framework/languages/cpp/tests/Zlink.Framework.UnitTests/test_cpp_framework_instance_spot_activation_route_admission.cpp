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
#include "../support/instance_spot_activation_messages.hpp"

using namespace framework_instance_spot_activation_fixture;

namespace
{
TEST (ZLinkFrameworkInstanceSpotActivation,
      MissingIntentActivatesOnceAndReadyOwnerIgnoresPlacementHints)
{
    zlink::framework::serializer_registry_t serializers;

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    const auto resolver_owner = std::make_shared<resolver_t> ();
    auto &resolver = *resolver_owner;
    runtime.bind_spot_address_resolver (resolver_owner);

    std::atomic_int activations{0};
    std::atomic_int ready_requests{0};
    runtime.bind_instance_spot_activator (
      [&] (const zlink::framework::spot_id_t &spot_id,
           const zlink::framework::detail::spot_activation_intent_t &intent,
           const std::optional<zlink::framework::runtime::spot_address_t> &cached_route,
           const std::string &, std::type_index,
           std::function<zlink::framework::serialized_payload_t (
             zlink::framework::serializer_registry_t &)>,
           const std::map<std::string, std::string> &, std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          EXPECT_EQ ("cart-17", std::string (spot_id));
          EXPECT_FALSE (cached_route);
          EXPECT_EQ (std::optional<std::string> ("commerce"), intent.mesh_name);
          EXPECT_EQ (std::optional<std::string> ("shopping-cart"), intent.stable_type);
          ++activations;
          auto address = zlink::framework::runtime::spot_address_t{
            "commerce", zlink::routing_id_t::from ("cart-node"), "cart-17", 1};
          resolver.addresses.insert_or_assign ("cart-17", address);
          co_return zlink::framework::result_t<void>::success ();
      },
      [&] (const auto &, const auto &, const auto &cached_route, std::string, std::type_index, auto,
           std::chrono::milliseconds, auto) {
          EXPECT_TRUE (cached_route);
          EXPECT_EQ ("commerce", cached_route->mesh_name);
          EXPECT_EQ ("cart-node", cached_route->node_rid.to_string ());
          ++ready_requests;
          return zlink::framework::request_task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::success (
              zlink::framework::detail::encoded_payload_to_raw (
                serializers.get<reply_t> ().serialize (reply_t{71}))));
      });

    std::atomic_int sends{0};
    std::atomic_int requests{0};
    zlink::framework::runtime::messaging::envelope_codec_t envelopes;
    runtime.bind_spot_mesh_transport (
      "commerce",
      [&] (const zlink::routing_id_t &node, const std::string &spot, std::uint64_t generation,
           zlink::framework::runtime::messaging::message_parts_t)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          EXPECT_EQ ("cart-node", node.to_string ());
          EXPECT_EQ ("cart-17", spot);
          EXPECT_EQ (1u, generation);
          ++sends;
          co_return zlink::framework::result_t<void>::success ();
      },
      [&] (const zlink::routing_id_t &, const std::string &, std::uint64_t,
           zlink::framework::runtime::messaging::message_parts_t parts, std::chrono::milliseconds)
        -> zlink::framework::request_task_t<
          zlink::framework::result_t<zlink::framework::runtime::messaging::message_parts_t>> {
          ++requests;
          auto header = envelopes.decode_header (parts).value ();
          header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
          reply_t reply{71};
          co_await zlink::framework::detail::request_admitted_t{};
          co_return zlink::framework::
            result_t<zlink::framework::runtime::messaging::message_parts_t>::success (
              envelopes.encode_parts (header, reply, serializers));
      });

    auto client = builder.route_client (serializers);
    const auto sent = client.send_to_spot ("cart-17", event_t{1})
                        .instance_spot ("shopping-cart")
                        .in_mesh ("commerce")
                        .async ()
                        .result ();
    ASSERT_TRUE (sent);

    const auto reply = client.request_to_spot ("cart-17", request_t{2})
                         .instance_spot ("different-type")
                         .in_mesh ("different-mesh")
                         .async<reply_t> ()
                         .result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (71, reply.value ().value);
    EXPECT_EQ (1, activations.load ());
    EXPECT_EQ (0, sends.load ());
    EXPECT_EQ (0, requests.load ());
    EXPECT_EQ (1, ready_requests.load ());
}

TEST (ZLinkFrameworkInstanceSpotActivation, NotReadyTargetFailureRetainsSelectedRidOnce)
{
    namespace fw = zlink::framework;
    using namespace std::chrono_literals;
    for (const auto level :
         {fw::message_flow_log_mode_t::off, fw::message_flow_log_mode_t::errors}) {
        auto app = fw::app_t::create ();
        auto &options = app.add_zlink_framework ();
        options.configure_locations ().owner_lease_renew_interval = 10ms;
        options.configure_locations ().owner_lease_fencing_margin = 1ms;
        options.configure_locations ().polling_interval = 10ms;
        auto events =
          std::make_shared<fw::test::telemetry_observations_t<fw::message_flow_event_t>> ();
        auto &dispatch = options.configure_dispatch ();
        dispatch.message_flow (level);
        fw::detail::dispatch_options_access_t::set_observer_for_tests (
          dispatch, [events] (const auto &event) {
              if (event.surface == fw::dispatch_error_surface_t::instance_spot
                  && event.result == fw::message_flow_result_t::failed)
                  events->record (event);
          });
        options.add_location_store (std::make_shared<fw::runtime::in_memory_location_store_t> ());
        options.add_relocation_store (
          std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
        options.add_route_mesh ("not-ready-flow")
          .set_object_role (fw::object_role_t::client)
          .set_routing_id (zlink::routing_id_t::from ("flow-source"))
          .listen ("tcp://127.0.0.1:0");
        std::thread host ([&] { EXPECT_EQ (0, app.run (0, nullptr)); });
        auto cleanup =
          std::unique_ptr<fw::app_t, std::function<void (fw::app_t *)>> (&app, [&] (auto *) {
              app.stop ();
              host.join ();
          });
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        while (!app.is_ready () && std::chrono::steady_clock::now () < deadline)
            std::this_thread::yield ();
        ASSERT_TRUE (app.is_ready ());
        auto resolver = std::make_shared<resolver_t> ();
        fw::runtime::spot_address_t route;
        route.mesh_name = "not-ready-flow";
        route.node_rid = zlink::routing_id_t::from ("dead-target");
        route.node_generation = 7;
        route.spot_id = "not-ready-room";
        route.spot_generation = route.object_generation = route.authority_owner_generation = 1;
        route.owner.owner_id = "dead-owner";
        route.owner.lease_generation = 1;
        route.store_version = "1";
        resolver->addresses.emplace (route.spot_id, route);
        fw::detail::channel_runtime_t::from (app.advanced ().zlink ().message_bus ())
          .bind_spot_address_resolver (resolver);
        auto provider = app.advanced ().services ().build_provider ();
        auto &client = provider.get_required<fw::route_client_t> ();
        const auto send = client.send_to_spot (route.spot_id, traced_event_t{1})
                            .instance_spot ("Room")
                            .in_mesh (route.mesh_name)
                            .async ()
                            .result ();
        EXPECT_FALSE (send);
        // Restore the same stale authority for the independent request.
        resolver->addresses.insert_or_assign (route.spot_id, route);
        const auto request = client.request_to_spot (route.spot_id, traced_request_t{1})
                               .instance_spot ("Room")
                               .in_mesh (route.mesh_name)
                               .timeout (100ms)
                               .async<traced_reply_t> ()
                               .result ();
        EXPECT_FALSE (request);
        if (level != fw::message_flow_log_mode_t::off) {
            std::unique_lock lock (events->mutex);
            ASSERT_TRUE (
              events->changed.wait_for (lock, 2s, [&] { return events->events.size () >= 2; }));
        }
        fw::test::acknowledge_delivery ();
        const auto failed = events->snapshot ();
        if (level == fw::message_flow_log_mode_t::off) {
            EXPECT_TRUE (failed.empty ());
        } else {
            ASSERT_EQ (2u, failed.size ());
            for (const auto &event : failed) {
                EXPECT_EQ ("dead-target", event.target_rid);
                EXPECT_EQ (fw::message_flow_reason_t::stale_target, event.reason);
                EXPECT_TRUE (event.exception);
            }
        }
    }
}

TEST (ZLinkFrameworkInstanceSpotActivation, MissingWithoutIntentDoesNotActivate)
{
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    auto runtime = zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    const auto resolver_owner = std::make_shared<resolver_t> ();
    auto &resolver = *resolver_owner;
    runtime.bind_spot_address_resolver (resolver_owner);
    std::atomic_int activations{0};
    runtime.bind_instance_spot_activator (
      [&] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
           std::chrono::system_clock::time_point)
        -> zlink::framework::task_t<zlink::framework::result_t<void>> {
          ++activations;
          co_return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure, "must not activate");
      },
      [&] (const auto &, const auto &, const auto &, auto, auto, auto, auto, auto) {
          ++activations;
          return zlink::framework::request_task_t<zlink::message_t> (
            zlink::framework::result_t<zlink::message_t>::failure (
              zlink::framework::framework_error_kind_t::internal_failure, "must not activate"));
      });

    const auto result =
      builder.route_client (serializers).send_to_spot ("missing", event_t{1}).async ().result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (zlink::framework::framework_error_kind_t::not_found, result.error_kind ());
    EXPECT_EQ (0, activations.load ());
}

TEST (ZLinkFrameworkInstanceSpotActivation, ActivationKeepsSubmissionStartAcrossRouteLookup)
{
    namespace fw = zlink::framework;
    fw::serializer_registry_t serializers;
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    const auto resolver_owner = std::make_shared<resolver_t> ();
    auto &resolver = *resolver_owner;
    resolver.lookup_gate = std::make_shared<fw::task_completion_source_t<void>> ();
    runtime.bind_spot_address_resolver (resolver_owner);
    std::optional<std::chrono::system_clock::time_point> observed_start;
    runtime.bind_instance_spot_activator (
      [&] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &,
           std::chrono::system_clock::time_point started_at) -> fw::task_t<fw::result_t<void>> {
          observed_start = started_at;
          co_return fw::result_t<void>::success ();
      },
      [] (const auto &, const auto &, const auto &, auto, auto, auto, auto, auto) {
          return fw::request_task_t<zlink::message_t> (fw::result_t<zlink::message_t>::failure (
            fw::framework_error_kind_t::internal_failure, "unused request activation"));
      });
    const auto before = std::chrono::system_clock::now ();
    auto send = builder.route_client (serializers)
                  .send_to_spot ("cold", event_t{1})
                  .instance_spot ("traced-player")
                  .async ();
    const auto lookup_started = std::chrono::system_clock::now ();
    EXPECT_EQ (1, resolver.reads.load ());
    EXPECT_FALSE (send.await_ready ());
    EXPECT_FALSE (observed_start);
    resolver.lookup_gate->complete (fw::result_t<void>::success ());
    ASSERT_TRUE (send.result ());
    ASSERT_TRUE (observed_start);
    EXPECT_GE (*observed_start, before);
    EXPECT_LE (*observed_start, lookup_started);
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      OneWayColdActivationUsesSourceRequestBudgetWithoutEndingSend)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    auto app = fw::app_t::create ();
    auto &options = app.add_zlink_framework ();
    auto flows = std::make_shared<fw::test::telemetry_observations_t<fw::message_flow_event_t>> ();
    auto errors =
      std::make_shared<fw::test::telemetry_observations_t<fw::message_dispatch_error_event_t>> ();
    constexpr auto source_budget = 80ms;
    auto &dispatch = options.configure_dispatch ();
    dispatch.message_flow (fw::message_flow_log_mode_t::normal);
    fw::detail::dispatch_options_access_t::set_observer_for_tests (
      dispatch, [flows] (const fw::message_flow_event_t &event) {
          if (event.surface == fw::dispatch_error_surface_t::instance_spot
              && event.outcome == fw::message_flow_outcome_t::dropped)
              flows->record (event);
      });
    fw::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      dispatch, [errors] (const fw::message_dispatch_error_event_t &event) {
          if (event.surface == fw::dispatch_error_surface_t::instance_spot)
              errors->record (event);
      });
    class lookup_gate_store_t final : public fw::location_store_t
    {
      public:
        fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
        {
            if (key.value.find ("budget-player-1") != std::string::npos && !held.exchange (true)) {
                entered.set_value ();
                co_await release.task ();
            }
            co_return co_await inner.read (std::move (key));
        }
        fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
        {
            return inner.scan (std::move (request));
        }
        fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
        {
            return inner.write (std::move (request));
        }
        fw::runtime::in_memory_location_store_t inner;
        std::atomic_bool held{false};
        std::promise<void> entered;
        fw::task_completion_source_t<void> release;
    };
    class recovery_gate_t final : public fw::runtime::stateful::relocation_store_port_t
    {
      public:
        fw::runtime::stateful::relocation_stored_t
        put (const std::vector<std::uint8_t> &payload,
             std::chrono::hours,
             std::chrono::steady_clock::time_point) override
        {
            entered.set_value (
              fw::runtime::protocol::decode_instance_activation_recovery (payload, false)
                .activation.target.deadline_unix_ms);
            return fw::runtime::infrastructure_result ([&] { return release.task (); }).value ();
        }
        std::optional<std::vector<std::uint8_t>> get (const std::string &) override
        {
            ADD_FAILURE () << "Unexpected recovery read";
            return std::nullopt;
        }
        void remove (const std::string &) override
        {
            ADD_FAILURE () << "Unexpected recovery removal";
        }
        std::promise<std::uint64_t> entered;
        fw::task_completion_source_t<fw::runtime::stateful::relocation_stored_t> release;
    };
    auto recovery = std::make_shared<recovery_gate_t> ();
    auto recovery_entered = recovery->entered.get_future ();
    app.advanced ().services ().add_factory<fw::runtime::stateful::relocation_store_port_t> (
      [recovery] (auto &) {
          return std::static_pointer_cast<fw::runtime::stateful::relocation_store_port_t> (
            recovery);
      },
      fw::service_lifetime_t::singleton);
    auto store = std::make_shared<lookup_gate_store_t> ();
    auto lookup_entered = store->entered.get_future ();
    options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
    options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
    options.add_location_store (store);
    options.add_relocation_store (std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    options.configure_locations ().polling_interval = 10ms;
    std::shared_ptr<traced_instance_spot_t> instance;
    options.add_route_mesh ("activation-budget")
      .set_object_role (fw::object_role_t::server)
      .set_routing_id (zlink::routing_id_t::from ("activation-budget-node"))
      .listen ("tcp://127.0.0.1:0")
      .set_default_request_timeout (source_budget)
      .add_instance_spot_factory<traced_instance_spot_t> (
        "traced-player",
        [&] (fw::instance_spot_context_t context) {
            instance = std::make_shared<traced_instance_spot_t> (std::move (context));
            return instance;
        },
        [] (auto &factory) { factory.disable_relocation (); });
    char command[] = "instance-activation-budget";
    char *argv[] = {command};
    int exit_code = -1;
    std::thread host ([&] { exit_code = app.run (1, argv); });
    auto cleanup =
      std::unique_ptr<fw::app_t, std::function<void (fw::app_t *)>> (&app, [&] (auto *) {
          recovery->release.complete (
            fw::result_t<fw::runtime::stateful::relocation_stored_t>::failure (
              fw::framework_error_kind_t::unavailable, "Controlled recovery Store failure"));
          store->release.complete (fw::result_t<void>::success ());
          app.stop ();
          host.join ();
      });
    const auto ready_deadline = std::chrono::steady_clock::now () + 5s;
    while (!app.is_ready () && std::chrono::steady_clock::now () < ready_deadline)
        std::this_thread::yield ();
    ASSERT_TRUE (app.is_ready ());
    auto provider = app.advanced ().services ().build_provider ();
    auto &client = provider.get_required<fw::route_client_t> ();
    const auto unix_ms = [] (auto at) {
        return static_cast<std::uint64_t> (
          std::chrono::duration_cast<std::chrono::milliseconds> (at.time_since_epoch ()).count ());
    };
    const auto before_send = std::chrono::system_clock::now ();
    auto send = client.send_to_spot ("budget-player-1", traced_event_t{11})
                  .instance_spot ("traced-player")
                  .in_mesh ("activation-budget")
                  .async ();
    ASSERT_EQ (std::future_status::ready, lookup_entered.wait_for (3s));
    const auto lookup_started = std::chrono::system_clock::now ();
    // Crossing the source budget must not finish a send still waiting for resolve.
    auto source_expired = advance_activation_deadline (unix_ms (lookup_started + source_budget));
    ASSERT_EQ (std::future_status::ready, source_expired.wait_for (1s));
    EXPECT_TRUE (source_expired.get ());
    EXPECT_FALSE (send.await_ready ());
    store->release.complete (fw::result_t<void>::success ());
    // Outbound admission succeeds while target recovery is still pending.
    const auto admitted = send.result_for (1s);
    ASSERT_TRUE (admitted.has_value ());
    ASSERT_TRUE (*admitted);
    ASSERT_EQ (std::future_status::ready, recovery_entered.wait_for (3s));
    const auto activation_deadline = recovery_entered.get ();
    EXPECT_GE (activation_deadline, unix_ms (before_send + source_budget));
    EXPECT_LE (activation_deadline, unix_ms (lookup_started + source_budget));
    // Spot address messaging §5: expire the captured source budget while recovery
    // is pending. Advancing the scheduler does not assume the two clocks advance equally.
    auto expired = advance_activation_deadline (activation_deadline);
    ASSERT_EQ (std::future_status::ready, expired.wait_for (1s));
    EXPECT_TRUE (expired.get ());
    const auto observation_deadline = std::chrono::steady_clock::now () + 3s;
    {
        std::unique_lock lock (errors->mutex);
        ASSERT_TRUE (errors->changed.wait_until (lock, observation_deadline,
                                                 [&] { return !errors->events.empty (); }));
    }
    // Activation records its dropped flow before reporting this terminal error.
    fw::test::acknowledge_delivery (observation_deadline);
    const auto dropped = flows->snapshot ();
    const auto diagnostics = errors->snapshot ();
    ASSERT_EQ (1u, dropped.size ());
    const auto &terminal = dropped.front ();
    EXPECT_EQ (fw::message_flow_reason_t::activation_timeout, terminal.reason);
    ASSERT_TRUE (terminal.exception);
    try {
        std::rethrow_exception (terminal.exception);
    }
    catch (const fw::framework_exception_t &error) {
        EXPECT_EQ (fw::framework_error_kind_t::deadline_exceeded, error.kind ());
    }
    EXPECT_EQ (fw::dispatch_error_action_t::drop, terminal.error_action);
    ASSERT_EQ (1u, diagnostics.size ());
    EXPECT_EQ (typeid (fw::framework_exception_t).name (), diagnostics.front ().error_type);
    EXPECT_EQ ("Instance Spot activation deadline expired", diagnostics.front ().error_message);
    EXPECT_EQ (terminal.exception, diagnostics.front ().exception);
    EXPECT_FALSE (instance);
    EXPECT_TRUE (send.result ());
    cleanup.reset ();
    EXPECT_EQ (1u, flows->snapshot ().size ());
    EXPECT_EQ (0, exit_code);
}

TEST (ZLinkFrameworkInstanceSpotActivation, ColdSendUsesServiceSendAdmissionBeforePeerIsAdmitted)
{
    using namespace std::chrono_literals;
    namespace rt = zlink::framework::runtime;
    rt::mesh::raw_mesh_node_owner_t target (
      {.descriptor = {.mesh_name = "cold-admission",
                      .node_routing_id = {1},
                      .lifecycle_generation = 1,
                      .descriptor_revision = 1,
                      .advertised_endpoint = "tcp://127.0.0.1:0"},
       .runtime_failures = std::make_shared<rt::runtime_failure_collector_t> ()});
    rt::mesh::raw_mesh_node_owner_t source (
      {.descriptor = {.mesh_name = "cold-admission",
                      .node_routing_id = {2},
                      .lifecycle_generation = 2,
                      .descriptor_revision = 1,
                      .advertised_endpoint = "tcp://127.0.0.1:0"},
       .runtime_failures = std::make_shared<rt::runtime_failure_collector_t> ()});
    target.start ();
    source.start ();
    ASSERT_TRUE (source.connect_peer (target.endpoint ()));
    ASSERT_FALSE (source.topology ().peer (std::array<std::uint8_t, 1>{1}));
    rt::protocol::application_payload_t payload{"probe", "application/json", {1}};
    EXPECT_FALSE (source.send_to_node ({1}, payload).result ().value ());
    rt::protocol::instance_spot_activation_header_t command{
      {{1},
       1,
       "contested",
       "cold-admission",
       "room",
       "descriptor-1",
       static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                     std::chrono::system_clock::now ().time_since_epoch () + 2s)
                                     .count ())},
      2,
      {2},
      std::nullopt,
      false,
      {11, 13},
      0,
      false};
    auto cold = source.send_instance_spot_activation ({1}, command, std::nullopt, payload);
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!cold.await_ready () && std::chrono::steady_clock::now () < deadline) {
        ASSERT_TRUE (source.pump_one (rt::mesh::service_liveness_registry_t::clock_t::now (), false)
                       .result ());
        std::this_thread::yield ();
    }
    const auto admitted = cold.result_for (0ms);
    ASSERT_TRUE (admitted);
    ASSERT_TRUE (*admitted);
    EXPECT_FALSE (admitted->value ());
    EXPECT_FALSE (source.topology ().peer (std::array<std::uint8_t, 1>{1}));
    source.close ();
    target.close ();
}

TEST (ZLinkFrameworkInstanceSpotActivation, ColdTypeClassificationUsesServingCapabilities)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    struct case_t
    {
        const char *name;
        int weight;
        int spot_limit;
        int type_limit;
        bool second_type;
        bool fill_first;
        std::optional<std::string> requested_type;
        fw::framework_error_kind_t expected;
    };
    const std::vector<case_t> cases{
      {"absent", 100, 0, 0, false, false, "absent", fw::framework_error_kind_t::not_found},
      {"weight-zero", 0, 0, 0, false, false, "traced-player",
       fw::framework_error_kind_t::unavailable},
      {"aggregate-full", 100, 1, 0, false, true, "traced-player",
       fw::framework_error_kind_t::unavailable},
      {"per-type-full", 100, 0, 1, false, true, "traced-player",
       fw::framework_error_kind_t::unavailable},
      {"multiple-types-after-full", 100, 1, 0, true, true, std::nullopt,
       fw::framework_error_kind_t::invalid_operation}};

    for (const auto &test_case : cases) {
        SCOPED_TRACE (test_case.name);
        auto app = fw::app_t::create ();
        auto &options = app.add_zlink_framework ();
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (std::make_shared<fw::runtime::in_memory_location_store_t> ());
        options.add_relocation_store (
          std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
        options.configure_locations ().polling_interval = 10ms;
        auto mesh = options.add_route_mesh ("cold-type-classification");
        mesh.set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("cold-type-node"))
          .listen ("tcp://127.0.0.1:0")
          .set_placement_weight (test_case.weight);
        if (test_case.spot_limit != 0)
            mesh.set_spot_limit (test_case.spot_limit);
        mesh.add_instance_spot_factory<traced_instance_spot_t> (
          "traced-player",
          [] (fw::instance_spot_context_t context) {
              return std::make_shared<traced_instance_spot_t> (std::move (context));
          },
          [limit = test_case.type_limit] (auto &factory) {
              if (limit)
                  factory.set_stable_type_limit (limit);
              factory.disable_relocation ();
          });
        if (test_case.second_type) {
            mesh.add_instance_spot_factory<traced_instance_spot_t> (
              "other-player",
              [] (fw::instance_spot_context_t context) {
                  return std::make_shared<traced_instance_spot_t> (std::move (context));
              },
              [] (auto &factory) { factory.disable_relocation (); });
        }
        char command[] = "cold-type-classification";
        char *argv[] = {command};
        int exit_code = -1;
        std::thread host ([&] { exit_code = app.run (1, argv); });
        auto cleanup =
          std::unique_ptr<fw::app_t, std::function<void (fw::app_t *)>> (&app, [&] (auto *) {
              app.stop ();
              host.join ();
          });
        const auto ready_deadline = std::chrono::steady_clock::now () + 5s;
        while (!app.is_ready () && std::chrono::steady_clock::now () < ready_deadline)
            std::this_thread::yield ();
        ASSERT_TRUE (app.is_ready ());
        auto provider = app.advanced ().services ().build_provider ();
        auto &client = provider.get_required<fw::route_client_t> ();
        if (test_case.fill_first) {
            const auto first =
              client.request_to_spot ("filled-spot", traced_request_t{1})
                .instance_spot (test_case.second_type ? "other-player" : "traced-player")
                .in_mesh ("cold-type-classification")
                .timeout (3s)
                .async<traced_reply_t> ()
                .result_for (4s);
            ASSERT_TRUE (first.has_value ());
            ASSERT_TRUE (*first) << (first->error () ? first->error ()->what () : "");
        }
        const auto reply = test_case.requested_type
                             ? client.request_to_spot ("probe-spot", traced_request_t{2})
                                 .instance_spot (*test_case.requested_type)
                                 .in_mesh ("cold-type-classification")
                                 .timeout (3s)
                                 .async<traced_reply_t> ()
                                 .result_for (4s)
                             : client.request_to_spot ("probe-spot", traced_request_t{2})
                                 .instance_spot ()
                                 .in_mesh ("cold-type-classification")
                                 .timeout (3s)
                                 .async<traced_reply_t> ()
                                 .result_for (4s);
        ASSERT_TRUE (reply.has_value ());
        ASSERT_FALSE (*reply);
        EXPECT_EQ (test_case.expected, reply->error_kind ());
        cleanup.reset ();
        EXPECT_EQ (0, exit_code);
    }
}
} // namespace
