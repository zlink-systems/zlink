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
#include "../support/instance_spot_activation_messages.hpp"

using namespace framework_instance_spot_activation_fixture;

namespace
{
TEST (ZLinkFrameworkInstanceSpotActivation,
      ColdActivationDispatchEmitsReceivedAndTerminalFlowEvents)
{
    zlink::framework::serializer_registry_t serializers;

    // Worker threads may emit after this scope ends, so the observer owns the state.
    struct events_state_t
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<zlink::framework::message_flow_event_t> events;
    };
    const auto events_state = std::make_shared<events_state_t> ();
    auto &events_mutex = events_state->mutex;
    auto &events_changed = events_state->changed;
    auto &events = events_state->events;
    zlink::framework::dispatch_options_t dispatch;
    dispatch.message_flow (zlink::framework::message_flow_log_mode_t::normal);
    zlink::framework::detail::dispatch_options_access_t::set_observer_for_tests (
      dispatch, [events_state] (const zlink::framework::message_flow_event_t &event) {
          {
              const std::lock_guard lock (events_state->mutex);
              events_state->events.push_back (event);
          }
          events_state->changed.notify_all ();
      });

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    zlink::framework::detail::apply_dispatch_options (builder, dispatch);
    auto mesh = builder.add_route_mesh ("instance-trace");
    mesh.add_instance_spot_factory<traced_instance_spot_t> (
      "traced-player",
      [] (zlink::framework::instance_spot_context_t context) {
          return std::make_shared<traced_instance_spot_t> (std::move (context));
      },
      [] (auto &factory) { factory.disable_relocation (); });
    auto runtime = zlink::framework::detail::spot_node_runtime_t::from (builder, "instance-trace");
    ASSERT_TRUE (runtime);
    auto channel_runtime =
      zlink::framework::detail::channel_runtime_t::from (builder.message_bus ());
    channel_runtime.bind_serializers (serializers);

    const auto created = runtime->get_or_create_spot (
      "traced-player", zlink::framework::spot_id_t ("traced-player-1"));
    ASSERT_EQ (zlink::framework::spot_create_state_t::created, created.state);

    zlink::framework::service_collection_t services;
    auto provider = services.build_provider ();
    const std::string activation_flow_id = "019fc5b9-9df3-786b-bb69-d55358f6d48b";
    const auto event_payload = zlink::framework::detail::encoded_payload_to_raw (
      serializers.get<traced_event_t> ().serialize (traced_event_t{7}));
    const auto event_result =
      runtime
        ->dispatch_instance_activation (
          zlink::framework::spot_id_t ("traced-player-1"), traced_event_t::packet_name,
          serializers.get<traced_event_t> ().content_type (), event_payload.to_bytes (), {}, false,
          "operation-send", provider, serializers, [] {}, activation_flow_id,
          zlink::framework::flow_origin_t::application)
        .result ();
    ASSERT_TRUE (event_result);

    const auto request_payload = zlink::framework::detail::encoded_payload_to_raw (
      serializers.get<traced_request_t> ().serialize (traced_request_t{9}));
    const auto request_result =
      runtime
        ->dispatch_instance_activation (
          zlink::framework::spot_id_t ("traced-player-1"), traced_request_t::packet_name,
          serializers.get<traced_request_t> ().content_type (), request_payload.to_bytes (), {},
          true, "operation-request", provider, serializers, [] {}, activation_flow_id,
          zlink::framework::flow_origin_t::application)
        .result ();
    ASSERT_TRUE (request_result);
    const auto decoded_reply = serializers.get<traced_reply_t> ().deserialize (
      zlink::framework::detail::encoded_payload_from_raw (request_result.value ()));
    EXPECT_EQ (10, decoded_reply.value);

    const auto has_event_transition = [&] (std::string_view packet,
                                           zlink::framework::message_flow_outcome_t outcome,
                                           std::string_view correlation) {
        return std::any_of (events.begin (), events.end (), [&] (const auto &event) {
            return event.packet_name && *event.packet_name == packet && event.outcome == outcome
                   && event.surface == zlink::framework::dispatch_error_surface_t::spot_route
                   && event.spot_id && *event.spot_id == "traced-player-1" && event.correlation_id
                   && *event.correlation_id == correlation && event.flow_id
                   && *event.flow_id == activation_flow_id && event.flow_origin
                   && *event.flow_origin == zlink::framework::flow_origin_t::application;
        });
    };
    {
        std::unique_lock lock (events_mutex);
        ASSERT_TRUE (events_changed.wait_for (lock, std::chrono::seconds (2), [&] {
            return has_event_transition (traced_request_t::packet_name,
                                         zlink::framework::message_flow_outcome_t::replied,
                                         "operation-request");
        }));
        EXPECT_TRUE (has_event_transition (traced_event_t::packet_name,
                                           zlink::framework::message_flow_outcome_t::received,
                                           "operation-send"));
        EXPECT_TRUE (has_event_transition (traced_event_t::packet_name,
                                           zlink::framework::message_flow_outcome_t::dispatched,
                                           "operation-send"));
        EXPECT_TRUE (has_event_transition (traced_request_t::packet_name,
                                           zlink::framework::message_flow_outcome_t::received,
                                           "operation-request"));
    }
    runtime->release_native_handles ();
}
} // namespace

static void verify_ended_instance_intent (bool provider, bool disabled)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    auto opaque = std::make_shared<rt::in_memory_location_store_t> ();
    std::shared_ptr<fw::location_repository_t> store =
      provider ? std::static_pointer_cast<fw::location_repository_t> (
                   std::make_shared<rt::provider_location_repository_t> (*opaque))
               : std::static_pointer_cast<fw::location_repository_t> (
                   std::make_shared<rt::in_memory_location_repository_t> ());
    store->configure_reclaim_margin (std::chrono::milliseconds::zero ());
    fw::location_reclaim_test_access_t clock;
    clock.bind (*store);
    const auto owner = std::get<fw::owner_lease_claimed_t> (
                         store->claim_owner_lease ("ended-instance-owner", 60s).result ().value ())
                         .token;
    fw::mesh_node_descriptor_t old;
    old.mesh_name = "ended-instance";
    old.rid = zlink::routing_id_t::from ("old-instance-target");
    old.lifecycle_generation = 1;
    old.descriptor_revision = 1;
    old.endpoint = "tcp://127.0.0.1:7101";
    old.security_identity = "old-instance-target";
    old.owner_id = owner.owner_id;
    old.lease_generation = owner.lease_generation;
    old.object_role = fw::object_role_t::server;
    old.state = fw::framework_runtime_state_t::serving;
    // The old owner's advertised policy must not authorize the new target.
    old.object_capabilities.push_back (
      {fw::placement_object_kind_t::instance_spot, "traced-player",
       disabled ? fw::maintenance_policy_kind_t::recreate : fw::maintenance_policy_kind_t::disabled,
       false, 1});
    old.capacity.spot_types.push_back (
      {fw::placement_object_kind_t::instance_spot, "traced-player", {0, 0, 0}});
    ASSERT_EQ (fw::location_write_status_t::stored,
               store->update_mesh_node (old, fw::location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
    fw::object_reserve_request_t initial;
    initial.key = {fw::placement_object_kind_t::instance_spot, "ended-room"};
    initial.intent.stable_type = "traced-player";
    initial.target = {old.mesh_name, fw::node_rid_t::from_string (old.rid.to_string ()), 1, owner};
    initial.capacity_bundle = {0, 1,
                               fw::spot_type_capacity_delta_t{
                                 fw::placement_object_kind_t::instance_spot, "traced-player", 1}};
    const auto initial_result = store->reserve (initial).result ().value ();
    ASSERT_TRUE (std::holds_alternative<fw::object_reserved_t> (initial_result))
      << initial_result.index ();
    const auto reserved = std::get<fw::object_reserved_t> (initial_result);
    rt::instance_spot_authority_payload_t ready{
      .stable_type = "traced-player",
      .spot_id = "ended-room",
      .owner_id = owner.owner_id,
      .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
      .mesh_name = old.mesh_name,
      .node_rid = initial.target.node_rid,
      .node_generation = 1};
    ASSERT_TRUE (std::holds_alternative<fw::object_committed_t> (
      store
        ->commit ({initial.key, reserved.fence, rt::encode_instance_spot_authority_payload (ready)})
        .result ()
        .value ()));
    const auto before = std::get<fw::authority_snapshot_t> (
      store->read_authority (rt::spot_authority_key ("ended-room")).result ().value ());
    ASSERT_TRUE (std::holds_alternative<fw::owner_lease_released_t> (
      store->release_owner_lease (owner).result ().value ()));
    auto app = fw::app_t::create ();
    auto &options = app.add_zlink_framework ();
    options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
    options.configure_locations ().owner_lease_fencing_margin = 5s;
    options.add_location_store (opaque);
    options.add_relocation_store (std::make_shared<rt::in_memory_relocation_store_t> ());
    app.advanced ().services ().add_factory<fw::location_repository_t> (
      [store] (fw::service_provider_t &) { return store; }, fw::service_lifetime_t::singleton);
    options.configure_locations ().polling_interval = 10ms;
    auto mesh = options.add_route_mesh ("ended-instance");
    mesh.set_object_role (fw::object_role_t::server)
      .set_routing_id (zlink::routing_id_t::from ("new-instance-target"))
      .listen ("tcp://127.0.0.1:0");
    std::atomic_int factories{0};
    mesh.add_instance_spot_factory<traced_instance_spot_t> (
      "traced-player",
      [&] (fw::instance_spot_context_t context) {
          ++factories;
          return std::make_shared<traced_instance_spot_t> (std::move (context));
      },
      [disabled] (auto &factory) {
          if (disabled)
              factory.disable_relocation ();
          else
              factory.recreate_on_relocation ();
      });
    auto startup_observed = std::make_shared<fw::task_completion_source_t<void>> ();
    clock.bind (
      *store, [startup_observed] { startup_observed->complete (fw::result_t<void>::success ()); });
    std::thread host ([&] { EXPECT_EQ (0, app.run (0, nullptr)); });
    auto cleanup =
      std::unique_ptr<fw::app_t, std::function<void (fw::app_t *)>> (&app, [&] (auto *) {
          app.stop ();
          host.join ();
      });
    ASSERT_TRUE (startup_observed->task ().result_for (5s));
    clock.advance (15s);
    const auto until = std::chrono::steady_clock::now () + 5s;
    while (!app.is_ready () && std::chrono::steady_clock::now () < until)
        std::this_thread::yield ();
    ASSERT_TRUE (app.is_ready ());
    clock.bind (*store);
    auto services = app.advanced ().services ().build_provider ();
    auto &client = services.get_required<fw::route_client_t> ();
    (void) services.get_required<fw::spot_manager_t> ().find ("ended-room").result ();
    const auto ordinary = client.request_to_spot ("ended-room", traced_request_t{1})
                            .timeout (3s)
                            .async<traced_reply_t> ()
                            .result ();
    EXPECT_FALSE (ordinary);
    EXPECT_EQ (fw::framework_error_kind_t::unavailable, ordinary.error_kind ());
    const auto retained = std::get<fw::authority_snapshot_t> (
      store->read_authority (rt::spot_authority_key ("ended-room")).result ().value ());
    EXPECT_EQ (before.store_version, retained.store_version);
    EXPECT_EQ (before.payload, retained.payload);
    EXPECT_EQ (0, factories);
    const auto wrong = client.request_to_spot ("ended-room", traced_request_t{2})
                         .instance_spot ("other-type")
                         .timeout (3s)
                         .async<traced_reply_t> ()
                         .result ();
    EXPECT_FALSE (wrong);
    EXPECT_EQ (fw::framework_error_kind_t::type_mismatch, wrong.error_kind ());
    const auto waiting = client.request_to_spot ("ended-room", traced_request_t{3})
                           .instance_spot ()
                           .in_mesh ("ignored-on-existing-id")
                           .timeout (3s)
                           .async<traced_reply_t> ()
                           .result ();
    EXPECT_FALSE (waiting);
    EXPECT_EQ (fw::framework_error_kind_t::unavailable, waiting.error_kind ());
    const auto during_wait = std::get<fw::authority_snapshot_t> (
      store->read_authority (rt::spot_authority_key ("ended-room")).result ().value ());
    EXPECT_EQ (before.store_version, during_wait.store_version);
    EXPECT_EQ (0, factories);
    clock.advance (15s);
    const auto result = client.request_to_spot ("ended-room", traced_request_t{3})
                          .instance_spot ()
                          .in_mesh ("ignored-on-existing-id")
                          .timeout (3s)
                          .async<traced_reply_t> ()
                          .result ();
    const auto after = std::get<fw::authority_snapshot_t> (
      store->read_authority (rt::spot_authority_key ("ended-room")).result ().value ());
    if (disabled) {
        ASSERT_TRUE (result) << (result.error () ? result.error ()->what () : "");
        EXPECT_EQ (4, result.value ().value);
        EXPECT_GT (after.object_generation, before.object_generation);
        EXPECT_EQ (1, factories);
        const auto joined = client.request_to_spot ("ended-room", traced_request_t{4})
                              .instance_spot ()
                              .timeout (3s)
                              .async<traced_reply_t> ()
                              .result ();
        ASSERT_TRUE (joined);
        EXPECT_EQ (5, joined.value ().value);
        EXPECT_EQ (1, factories);
        EXPECT_EQ (
          after.object_generation,
          std::get<fw::authority_snapshot_t> (
            store->read_authority (rt::spot_authority_key ("ended-room")).result ().value ())
            .object_generation);
    } else {
        EXPECT_FALSE (result);
        EXPECT_EQ (fw::framework_error_kind_t::unavailable, result.error_kind ());
        EXPECT_EQ (before.store_version, after.store_version);
        EXPECT_EQ (before.payload, after.payload);
        EXPECT_EQ (0, factories);
    }
}

TEST (ZLinkFrameworkInstanceSpotActivation, EndedReadyProviderUsesTargetPolicy)
{
    verify_ended_instance_intent (true, true);
    verify_ended_instance_intent (true, false);
}

TEST (ZLinkFrameworkInstanceSpotActivation, EndedReadyInMemoryUsesTargetPolicy)
{
    verify_ended_instance_intent (false, true);
    verify_ended_instance_intent (false, false);
}

TEST (ZLinkFrameworkInstanceSpotActivation, StoredActivationMismatchPreservesAuthority)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    namespace host = rt::host;
    for (bool provider : {false, true}) {
        for (bool active : {false, true}) {
            for (unsigned input = 0; input != 8; ++input) {
                SCOPED_TRACE (active);
                SCOPED_TRACE (provider);
                SCOPED_TRACE (input);
                auto opaque = std::make_shared<rt::in_memory_location_store_t> ();
                std::shared_ptr<fw::location_repository_t> store =
                  provider ? std::static_pointer_cast<fw::location_repository_t> (
                               std::make_shared<rt::provider_location_repository_t> (*opaque))
                           : std::static_pointer_cast<fw::location_repository_t> (
                               std::make_shared<rt::in_memory_location_repository_t> ());
                store->configure_reclaim_margin (std::chrono::milliseconds::zero ());
                const auto owner =
                  std::get<fw::owner_lease_claimed_t> (
                    store->claim_owner_lease ("stored-owner", 60s).result ().value ())
                    .token;
                fw::mesh_node_descriptor_t descriptor;
                descriptor.mesh_name = "stored-activation";
                descriptor.rid = zlink::routing_id_t::from ("stored-target");
                descriptor.lifecycle_generation = 1;
                descriptor.descriptor_revision = 1;
                descriptor.endpoint = "tcp://127.0.0.1:7101";
                descriptor.security_identity = "stored-target";
                descriptor.owner_id = owner.owner_id;
                descriptor.lease_generation = owner.lease_generation;
                descriptor.object_role = fw::object_role_t::server;
                descriptor.state = fw::framework_runtime_state_t::serving;
                descriptor.object_capabilities.push_back (
                  {fw::placement_object_kind_t::instance_spot, "room",
                   fw::maintenance_policy_kind_t::disabled, false, 1});
                descriptor.capacity.spot_types.push_back (
                  {fw::placement_object_kind_t::instance_spot, "room", {0, 0, 0}});
                ASSERT_EQ (
                  fw::location_write_status_t::stored,
                  store->update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                    .result ()
                    .value ()
                    .status);
                auto blob_store = std::make_shared<rt::in_memory_relocation_store_t> ();
                auto blobs = std::make_shared<rt::provider_relocation_repository_t> (*blob_store);
                auto relocations =
                  std::make_shared<rt::stateful::public_relocation_store_adapter_t> (blobs);
                auto target = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
                  .mesh = {.descriptor = {.mesh_name = descriptor.mesh_name,
                                          .node_routing_id = descriptor.rid.to_bytes (),
                                          .lifecycle_generation = 1,
                                          .descriptor_revision = 1,
                                          .advertised_endpoint = "tcp://127.0.0.1:0"},
                           .runtime_failures =
                             std::make_shared<rt::runtime_failure_collector_t> ()}});
                int factories = 0;
                target->configure_instance_spot_operations (
                  store, relocations, [owner] { return owner; },
                  host::instance_spot_activation_materializer_t{
                    [&] (const auto &, const auto &, auto &) -> zlink::framework::task_t<bool> {
                        ++factories;
                        co_return true;
                    },
                    [] (auto, auto, auto accept_activation)
                      -> fw::task_t<host::instance_spot_activation_result_t> {
                        accept_activation ();
                        co_return host::instance_spot_activation_result_t{};
                    }});
                target->start ();
                auto cleanup =
                  std::unique_ptr<host::public_host_runtime_t,
                                  std::function<void (host::public_host_runtime_t *)>> (
                    target.get (), [&] (auto *) { target->close (); });
                const auto deadline = std::chrono::system_clock::now () + 3s;
                rt::protocol::instance_spot_activation_header_t original{
                  {descriptor.rid.to_bytes (), 1, "stored-room", descriptor.mesh_name, "room", "1",
                   static_cast<std::uint64_t> (
                     std::chrono::duration_cast<std::chrono::milliseconds> (
                       deadline.time_since_epoch ())
                       .count ())},
                  1,
                  descriptor.rid.to_bytes (),
                  std::nullopt,
                  true,
                  {11, 13},
                  17,
                  true};
                rt::protocol::application_payload_t payload{"probe", "application/json", {1}};
                const auto metadata =
                  fw::detail::mesh_metadata_codec_t::encode ({{"key", "value"}});
                const auto encoded =
                  rt::protocol::encode_instance_activation_recovery ({original, metadata, payload});
                const auto receipt =
                  relocations->put (encoded, 24h, std::chrono::steady_clock::now () + 3s);
                fw::object_reserve_request_t reserve;
                reserve.key = {fw::placement_object_kind_t::instance_spot, "stored-room"};
                reserve.intent.stable_type = "room";
                reserve.intent.request_content_reference = receipt.reference;
                std::vector<std::byte> public_bytes;
                for (auto value : encoded)
                    public_bytes.push_back (static_cast<std::byte> (value));
                reserve.intent.request_sha256 = rt::sha256 (public_bytes);
                reserve.intent.request_encoded_size = public_bytes.size ();
                reserve.target = {descriptor.mesh_name,
                                  fw::node_rid_t::from_string (descriptor.rid.to_string ()), 1,
                                  owner};
                reserve.capacity_bundle = {
                  0, 1,
                  fw::spot_type_capacity_delta_t{fw::placement_object_kind_t::instance_spot, "room",
                                                 1}};
                const auto reserved_result = store->reserve (reserve).result ().value ();
                ASSERT_TRUE (std::holds_alternative<fw::object_reserved_t> (reserved_result));
                if (active) {
                    rt::instance_spot_authority_payload_t ready{
                      .stable_type = "room",
                      .spot_id = "stored-room",
                      .owner_id = owner.owner_id,
                      .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
                      .mesh_name = descriptor.mesh_name,
                      .node_rid = reserve.target.node_rid,
                      .node_generation = 1,
                      .activation_recovery = rt::activation_recovery_pointer_t{
                        receipt.reference, reserve.intent.request_sha256,
                        static_cast<std::uint32_t> (reserve.intent.request_encoded_size), 1, 0}};
                    ASSERT_TRUE (std::holds_alternative<fw::object_committed_t> (
                      store
                        ->commit ({reserve.key,
                                   std::get<fw::object_reserved_t> (reserved_result).fence,
                                   rt::encode_instance_spot_authority_payload (ready)})
                        .result ()
                        .value ()));
                }
                const auto capacity_before = store->list_mesh_nodes (descriptor.mesh_name)
                                               .result ()
                                               .value ()
                                               .items.front ()
                                               .capacity;
                const auto before = std::get<fw::authority_snapshot_t> (
                  store->read_authority (rt::spot_authority_key ("stored-room"))
                    .result ()
                    .value ());
                auto wrong = original;
                auto changed_metadata = std::optional<std::vector<std::uint8_t>> (metadata);
                switch (input) {
                    case 0:
                        wrong.target.mesh_name = "other-mesh";
                        break;
                    case 1:
                        wrong.target.stable_type = "other-type";
                        break;
                    case 2:
                        wrong.target.descriptor_version = "2";
                        break;
                    case 3:
                        ++wrong.target.deadline_unix_ms;
                        break;
                    case 4:
                        ++wrong.operation.low;
                        ++wrong.target.deadline_unix_ms;
                        break;
                    case 5:
                        wrong.has_metadata = false;
                        changed_metadata.reset ();
                        break;
                    case 6:
                        changed_metadata =
                          fw::detail::mesh_metadata_codec_t::encode ({{"key", "other"}});
                        break;
                    case 7:
                        break; // Matching retransmission uses the current authority.
                }
                std::promise<rt::protocol::reply_header_t> reply;
                auto result = reply.get_future ();
                ASSERT_TRUE (target
                               ->activate_instance_spot_remote (
                                 descriptor.rid, wrong, changed_metadata, payload, 3s,
                                 [&] (auto, auto header, auto) { reply.set_value (header); })
                               .result ()
                               .value ());
                const auto until = std::chrono::steady_clock::now () + 3s;
                while (result.wait_for (0ms) != std::future_status::ready
                       && std::chrono::steady_clock::now () < until) {
                    ASSERT_TRUE (
                      target->dispatch_ready ([] (const auto &, const auto &, auto, auto) {}, false)
                        .result ());
                    std::this_thread::yield ();
                }
                ASSERT_EQ (std::future_status::ready, result.wait_for (0ms));
                const auto terminal = result.get ();
                const auto expected =
                  input != 7 && input != 4 ? rt::protocol::request_terminal_result::protocolError
                  : active                 ? rt::protocol::request_terminal_result::ok
                                           : rt::protocol::request_terminal_result::internalError;
                EXPECT_EQ (static_cast<std::uint32_t> (expected), terminal.terminal_result);
                const auto after = std::get<fw::authority_snapshot_t> (
                  store->read_authority (rt::spot_authority_key ("stored-room"))
                    .result ()
                    .value ());
                EXPECT_EQ (before.store_version, after.store_version);
                EXPECT_EQ (before.payload, after.payload);
                EXPECT_EQ (before.object_generation, after.object_generation);
                if (!active) {
                    ASSERT_TRUE (after.pending_creation);
                    EXPECT_EQ (before.pending_creation->request_content_reference,
                               after.pending_creation->request_content_reference);
                }
                const auto capacity_after = store->list_mesh_nodes (descriptor.mesh_name)
                                              .result ()
                                              .value ()
                                              .items.front ()
                                              .capacity;
                EXPECT_EQ (capacity_before.spots.active, capacity_after.spots.active);
                EXPECT_EQ (capacity_before.spots.reserved, capacity_after.spots.reserved);
                EXPECT_EQ (capacity_before.spot_types.front ().usage.active,
                           capacity_after.spot_types.front ().usage.active);
                EXPECT_EQ (capacity_before.spot_types.front ().usage.reserved,
                           capacity_after.spot_types.front ().usage.reserved);
                EXPECT_EQ (encoded, *relocations->get (receipt.reference));
                EXPECT_EQ ((input == 7 || input == 4) && active ? 1 : 0, factories);
            }
        }
    }
}
