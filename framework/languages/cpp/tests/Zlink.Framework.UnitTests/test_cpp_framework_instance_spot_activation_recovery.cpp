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
TEST (ZLinkFrameworkInstanceSpotActivation, RecoveredRequestWithoutNativeTokenDoesNotSubmitReply)
{
    namespace rt = zlink::framework::runtime;
    rt::mesh::raw_mesh_node_owner_t target (
      {.descriptor = {.mesh_name = "recovered-cold",
                      .node_routing_id = {1},
                      .lifecycle_generation = 1,
                      .descriptor_revision = 1,
                      .advertised_endpoint = "tcp://127.0.0.1:0"},
       .runtime_failures = std::make_shared<rt::runtime_failure_collector_t> ()});
    target.start ();
    auto recovered = rt::mesh::service_mailbox_record_t::from_parts (
      "source", rt::mesh::service_mailbox_domain_t::infrastructure, {}, {2}, 17);
    EXPECT_NO_THROW (EXPECT_FALSE (target.reply_instance_spot_activation (recovered, 0, 0)));
    target.close ();
}
} // namespace

TEST (CppFrameworkInstanceSpotActivation, StartupReleasesPreviousLifecycleBeforeRecoveryRoot)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    namespace host = rt::host;
    auto provider = std::make_shared<rt::in_memory_location_store_t> ();
    auto store = std::make_shared<rt::provider_location_repository_t> (*provider);
    store->configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto owner = std::get<fw::owner_lease_claimed_t> (
                         store->claim_owner_lease ("startup-owner", 60s).result ().value ())
                         .token;
    fw::mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "startup-mesh";
    descriptor.rid = zlink::routing_id_t::from ("startup-target");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7100";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.object_role = fw::object_role_t::server;
    descriptor.state = fw::framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back ({fw::placement_object_kind_t::instance_spot, "room",
                                               fw::maintenance_policy_kind_t::disabled, false, 1});
    descriptor.capacity.spots.limit = 1;
    descriptor.capacity.spot_types.push_back (
      {fw::placement_object_kind_t::instance_spot, "room", {0, 0, 1}});
    ASSERT_EQ (store->update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               fw::location_write_status_t::stored);
    class roots_t final : public rt::stateful::relocation_store_port_t
    {
      public:
        std::function<void ()> before_remove;
        bool exists = true;
        rt::stateful::relocation_stored_t put (const std::vector<std::uint8_t> &,
                                               std::chrono::hours,
                                               std::chrono::steady_clock::time_point) override
        {
            return {"startup-root", 0};
        }
        std::optional<std::vector<std::uint8_t>> get (const std::string &) override
        {
            ADD_FAILURE () << "Old lifecycle must not replay";
            return std::nullopt;
        }
        void remove (const std::string &reference) override
        {
            EXPECT_EQ (reference, "startup-root");
            before_remove ();
            EXPECT_TRUE (exists);
            exists = false;
        }
    };
    auto roots = std::make_shared<roots_t> ();
    fw::object_reserve_request_t request;
    request.key = {fw::placement_object_kind_t::instance_spot, "startup-spot"};
    request.intent = {"room", "startup-root", {}, 4};
    request.target = {descriptor.mesh_name, fw::node_rid_t::from_string ("startup-target"), 1,
                      owner};
    const std::string marker = "zlink:instance-spot:creating:v1";
    for (const auto ch : marker)
        request.creating_payload.push_back (static_cast<std::byte> (ch));
    request.capacity_bundle.spot_slots = 1;
    request.capacity_bundle.spot_type =
      fw::spot_type_capacity_delta_t{fw::placement_object_kind_t::instance_spot, "room", 1};
    const auto reserved = store->reserve (request).result ().value ();
    ASSERT_TRUE (std::holds_alternative<fw::object_reserved_t> (reserved));
    const auto key = rt::spot_authority_key ("startup-spot");
    roots->before_remove = [&] {
        EXPECT_TRUE (std::holds_alternative<fw::authority_missing_t> (
          store->read_authority (key).result ().value ()));
        const auto capacity = std::get<fw::store_found_t> (
          provider
            ->read ({std::string ("zlink:v12:capacity") + '\0' + "startup-mesh" + '\0'
                     + descriptor.rid.to_hex () + '\0' + "spots" + '\0' + "pending"})
            .result ()
            .value ());
        const auto counts =
          nlohmann::json::parse (capacity.value.bytes.begin (), capacity.value.bytes.end ());
        EXPECT_EQ (counts, 0);
    };
    store->remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, owner).result ().value ();
    descriptor.lifecycle_generation = 2;
    ASSERT_EQ (store->update_mesh_node (descriptor, fw::location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               fw::location_write_status_t::stored);
    auto target = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "startup-mesh",
                              .node_routing_id = descriptor.rid.to_bytes (),
                              .lifecycle_generation = 2,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<rt::runtime_failure_collector_t> ()}});
    target->configure_instance_spot_operations (
      store, roots, [owner] { return owner; },
      host::instance_spot_activation_materializer_t{
        [] (const auto &, const auto &, auto &) -> zlink::framework::task_t<bool> {
            ADD_FAILURE () << "Old lifecycle must not prepare";
            co_return false;
        },
        [] (auto, auto,
            auto accept_activation) -> fw::task_t<host::instance_spot_activation_result_t> {
            ADD_FAILURE () << "Old lifecycle must not activate";
            accept_activation ();
            co_return host::instance_spot_activation_result_t{};
        }});
    ASSERT_TRUE (roots->exists);
    target->start ();
    target->recover_instance_spot_activations ().result ().value ();
    EXPECT_FALSE (roots->exists);
}

static void verify_ready_recovery_root_deletion (bool local_available)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    namespace host = rt::host;
    class held_read_t final : public rt::in_memory_location_repository_t
    {
      public:
        fw::task_t<fw::authority_read_result_t>
        read_authority_core (fw::authority_key_t key, std::stop_token stop = {}) override
        {
            auto result =
              co_await rt::in_memory_location_repository_t::read_authority_core (key, stop);
            if (hold.exchange (false)) {
                EXPECT_TRUE (std::holds_alternative<fw::authority_snapshot_t> (result));
                captured.store (true);
                co_await release.task ();
            }
            co_return result;
        }
        std::atomic_bool hold{false};
        std::atomic_bool captured{false};
        fw::task_completion_source_t<void> release;
    };
    auto store = std::make_shared<held_read_t> ();
    store->configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto owner = std::get<fw::owner_lease_claimed_t> (
                         store->claim_owner_lease ("owner", 60s).result ().value ())
                         .token;
    fw::mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "same-target";
    descriptor.rid = zlink::routing_id_t::from ("target");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7101";
    descriptor.security_identity = "test-owner";
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

    class observed_roots_t final : public rt::stateful::relocation_store_port_t
    {
      public:
        explicit observed_roots_t (std::shared_ptr<fw::relocation_repository_t> value) :
            inner (std::move (value))
        {
        }
        rt::stateful::relocation_stored_t
        put (const std::vector<std::uint8_t> &bytes,
             std::chrono::hours retention,
             std::chrono::steady_clock::time_point deadline) override
        {
            return inner.put (bytes, retention, deadline);
        }
        std::optional<std::vector<std::uint8_t>> get (const std::string &reference) override
        {
            return inner.get (reference);
        }
        void remove (const std::string &reference) override
        {
            inner.remove (reference);
            deleted.store (true);
        }
        rt::stateful::public_relocation_store_adapter_t inner;
        std::atomic_bool deleted{false};
    };
    auto relocations = std::make_shared<observed_roots_t> (blobs);

    auto target = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "same-target",
                              .node_routing_id = zlink::routing_id_t::from ("target").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<rt::runtime_failure_collector_t> ()}});

    fw::task_completion_source_t<host::instance_spot_activation_result_t> first_handler;
    std::atomic_int entered{0};
    fw::zlink_builder_t builder = fw::test::runtime_failure_builder ();
    auto mesh = builder.add_route_mesh ("same-target");
    mesh.add_instance_spot_factory<traced_instance_spot_t> (
      "room-type",
      [] (fw::instance_spot_context_t context) {
          return std::make_shared<traced_instance_spot_t> (std::move (context));
      },
      [] (auto &factory) { factory.disable_relocation (); });
    auto local_spots = fw::detail::spot_node_runtime_t::from (builder, "same-target");
    ASSERT_TRUE (local_spots);
    target->configure_instance_spot_operations (
      store, relocations, [owner] { return owner; },
      host::instance_spot_activation_materializer_t{
        [&] (const auto &request, const auto &snapshot, auto &) -> zlink::framework::task_t<bool> {
            const auto created = co_await local_spots->get_or_create_spot_async (
              request.target.stable_type, fw::spot_id_t (request.target.spot_id),
              zlink::message_t{}, snapshot.object_generation, request.target.mesh_name,
              snapshot.authority_owner_generation);
            co_return created->state == fw::spot_create_state_t::created
              || created->state == fw::spot_create_state_t::existing;
        },
        [&] (auto command, auto,
             auto accept_activation) -> fw::task_t<host::instance_spot_activation_result_t> {
            accept_activation ();
            ++entered;
            if (command->activation.operation.low == 1)
                co_return co_await first_handler.task ();
            co_return host::instance_spot_activation_result_t{
              0, 0, rt::protocol::application_payload_t{"reply", "application/json", {'2'}}};
        },
        {},
        {},
        [&] (const auto &request, const auto &snapshot) {
            return local_available
                   && local_spots->matches_instance_activation (
                     request.target.spot_id, request.target.stable_type, request.target.mesh_name,
                     snapshot.object_generation, snapshot.authority_owner_generation);
        }});

    target->start ();
    struct cleanup_t
    {
        std::function<void ()> finish;
        ~cleanup_t () { finish (); }
    } cleanup{[&] {
        first_handler.complete (
          fw::result_t<host::instance_spot_activation_result_t>::success ({}));
        store->release.complete (fw::result_t<void>::success ());
        target->close ();
    }};
    const auto deadline =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    (std::chrono::system_clock::now () + 3s).time_since_epoch ())
                                    .count ());
    rt::protocol::instance_spot_activation_header_t request{
      {target->status ().routing_id ().to_bytes (), target->status ().lifecycle_generation (),
       "room", "same-target", "room-type", "1", deadline},
      target->status ().lifecycle_generation (),
      target->status ().routing_id ().to_bytes (),
      std::nullopt,
      true,
      {1575, 1},
      1,
      false};
    const rt::protocol::application_payload_t payload{"probe", "application/json", {1}};
    std::promise<
      std::pair<rt::protocol::reply_header_t, std::optional<rt::protocol::application_payload_t>>>
      first_reply, second_reply;
    auto first = first_reply.get_future ();
    auto second = second_reply.get_future ();
    ASSERT_TRUE (target
                   ->activate_instance_spot_remote (target->status ().routing_id (), request,
                                                    std::nullopt, payload, 3s,
                                                    [&] (auto, auto header, auto reply) {
                                                        first_reply.set_value ({header, reply});
                                                    })
                   .result ());
    const auto until = std::chrono::steady_clock::now () + 3s;
    auto pump = [&] {
        ASSERT_TRUE (
          target->dispatch_ready ([] (const auto &, const auto &, auto, auto) {}, false).result ());
    };
    while (entered.load () == 0 && std::chrono::steady_clock::now () < until)
        pump ();
    ASSERT_EQ (1, entered.load ());
    const auto active = std::get<fw::authority_snapshot_t> (
      store->read_authority (rt::spot_authority_key ("room")).result ().value ());
    EXPECT_TRUE (local_spots->matches_instance_activation ("room", "room-type", "same-target",
                                                           active.object_generation,
                                                           active.authority_owner_generation));
    EXPECT_FALSE (local_spots->matches_instance_activation ("room", "room-type", "same-target",
                                                            active.object_generation + 1,
                                                            active.authority_owner_generation));
    EXPECT_FALSE (local_spots->matches_instance_activation ("room", "room-type", "same-target",
                                                            active.object_generation,
                                                            active.authority_owner_generation + 1));
    EXPECT_FALSE (local_spots->matches_instance_activation ("room", "other-type", "same-target",
                                                            active.object_generation,
                                                            active.authority_owner_generation));
    EXPECT_FALSE (local_spots->matches_instance_activation ("room", "room-type", "other-mesh",
                                                            active.object_generation,
                                                            active.authority_owner_generation));
    store->hold.store (true);
    request.operation.low = 2;
    request.reply_route_id = 2;
    ASSERT_TRUE (target
                   ->activate_instance_spot_remote (target->status ().routing_id (), request,
                                                    std::nullopt, payload, 3s,
                                                    [&] (auto, auto header, auto reply) {
                                                        second_reply.set_value ({header, reply});
                                                    })
                   .result ());
    while (!store->captured.load () && std::chrono::steady_clock::now () < until)
        pump ();
    ASSERT_TRUE (store->captured.load ());
    first_handler.complete (fw::result_t<host::instance_spot_activation_result_t>::success (
      {0, 0, rt::protocol::application_payload_t{"reply", "application/json", {'1'}}}));
    while (!relocations->deleted.load () && std::chrono::steady_clock::now () < until)
        pump ();
    ASSERT_TRUE (relocations->deleted.load ());
    store->release.complete (fw::result_t<void>::success ());
    while ((first.wait_for (0ms) != std::future_status::ready
            || second.wait_for (0ms) != std::future_status::ready)
           && std::chrono::steady_clock::now () < until)
        pump ();
    ASSERT_EQ (std::future_status::ready, first.wait_for (0ms));
    ASSERT_EQ (std::future_status::ready, second.wait_for (0ms));
    const auto [first_header, first_payload] = first.get ();
    const auto [second_header, second_payload] = second.get ();
    EXPECT_EQ (0u, first_header.terminal_result);
    ASSERT_TRUE (first_payload);
    EXPECT_EQ (std::vector<std::uint8_t>{'1'}, first_payload->payload_bytes ());
    if (local_available) {
        EXPECT_EQ (0u, second_header.terminal_result);
        ASSERT_TRUE (second_payload);
        EXPECT_EQ (std::vector<std::uint8_t>{'2'}, second_payload->payload_bytes ());
    } else {
        const auto failure = rt::messaging::request_failure_mapper_t{}.target_failure_reply (
          fw::framework_error_kind_t::data_lost);
        ASSERT_TRUE (failure);
        EXPECT_EQ (failure->terminal_result, second_header.terminal_result);
        EXPECT_EQ (failure->failure_code, second_header.failure_code);
        EXPECT_FALSE (second_payload);
        EXPECT_EQ (1, entered.load ());
    }
    local_spots->release_native_handles ();
}

TEST (CppFrameworkInstanceSpotActivation, ReadyLocalAdmissionSurvivesRecoveryRootDeletion)
{
    verify_ready_recovery_root_deletion (true);
}

TEST (CppFrameworkInstanceSpotActivation, RecoveryWithoutLocalAdmissionRejectsMissingRoot)
{
    verify_ready_recovery_root_deletion (false);
}
