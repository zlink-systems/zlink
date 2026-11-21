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


namespace
{
enum class retransmission_input_t
{
    unchanged,
    metadata,
    payload
};

static void verify_same_target_activation_order (
  bool ready_request,
  bool retransmission_mismatch = false,
  bool close_during_terminal = false,
  bool read_after_delete = false,
  retransmission_input_t mismatch_input = retransmission_input_t::unchanged)
{
    using namespace std::chrono_literals;
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    namespace host = rt::host;
    retransmission_mismatch =
      retransmission_mismatch || mismatch_input != retransmission_input_t::unchanged;
    class held_reservation_t final : public rt::in_memory_location_repository_t
    {
      public:
        fw::task_t<fw::object_reserve_result_t> reserve_core (fw::object_reserve_request_t request,
                                                              std::stop_token stop = {}) override
        {
            ++reserves;
            if (!hold_commit)
                co_await release.task ();
            co_return co_await rt::in_memory_location_repository_t::reserve_core (
              std::move (request), stop);
        }
        fw::task_t<fw::object_commit_result_t>
        commit_core (fw::object_commit_request_t request,
                     std::stop_token stop = {},
                     std::chrono::system_clock::time_point deadline = {}) override
        {
            auto result = co_await rt::in_memory_location_repository_t::commit_core (
              std::move (request), stop, deadline);
            ++commits;
            if (hold_commit)
                co_await release.task ();
            co_return result;
        }
        fw::task_t<fw::authority_compare_exchange_result_t>
        compare_exchange_authority_core (fw::authority_key_t key,
                                         std::string version,
                                         fw::authority_mutation_t mutation,
                                         std::stop_token stop = {}) override
        {
            if (hold_terminal) {
                terminal_entered.set_value ();
                co_await terminal_release.task ();
                hold_terminal = false;
            }
            co_return co_await rt::in_memory_location_repository_t::
              compare_exchange_authority_core (std::move (key), std::move (version),
                                               std::move (mutation), stop);
        }
        bool hold_terminal = false;
        std::promise<void> terminal_entered;
        fw::task_completion_source_t<void> terminal_release;
        bool hold_commit = false;
        std::atomic_int reserves{0};
        std::atomic_int commits{0};
        fw::task_completion_source_t<void> release;
    };
    auto store = std::make_shared<held_reservation_t> ();
    store->configure_reclaim_margin (std::chrono::milliseconds::zero ());
    store->hold_commit = ready_request;
    store->hold_terminal = close_during_terminal;
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
    class delete_before_read_store_t final : public fw::relocation_store_t
    {
      public:
        fw::task_t<fw::blob_put_result_t> put (fw::blob_reference_t reference,
                                               std::span<const std::byte> payload,
                                               std::chrono::milliseconds retention) override
        {
            return inner.put (std::move (reference), payload, retention);
        }
        fw::task_t<fw::blob_read_result_t> read (fw::blob_reference_t reference) override
        {
            if (read_after_delete) {
                read_entered.store (true);
                co_await deleted.task ();
            }
            co_return co_await inner.read (std::move (reference));
        }
        fw::task_t<fw::blob_renew_result_t> renew (fw::blob_reference_t reference,
                                                   std::chrono::milliseconds retention) override
        {
            return inner.renew (std::move (reference), retention);
        }
        fw::task_t<void> erase (fw::blob_reference_t reference) override
        {
            co_await inner.erase (std::move (reference));
            deleted.complete (fw::result_t<void>::success ());
        }
        rt::in_memory_relocation_store_t inner;
        bool read_after_delete = false;
        std::atomic_bool read_entered{false};
        fw::task_completion_source_t<void> deleted;
    };
    auto blob_store = std::make_shared<delete_before_read_store_t> ();
    blob_store->read_after_delete = read_after_delete;
    auto blobs = std::make_shared<rt::provider_relocation_repository_t> (*blob_store);
    auto relocations = std::make_shared<rt::stateful::public_relocation_store_adapter_t> (blobs);
    auto target = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "same-target",
                              .node_routing_id = zlink::routing_id_t::from ("target").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<rt::runtime_failure_collector_t> ()}});
    std::vector<std::uint64_t> queued;
    fw::task_completion_source_t<host::instance_spot_activation_result_t> first_handler;
    target->configure_instance_spot_operations (
      store, relocations, [owner] { return owner; },
      host::instance_spot_activation_materializer_t{
        [] (const auto &, const auto &, auto &) -> zlink::framework::task_t<bool> {
            co_return true;
        },
        [&] (auto command, auto,
             auto accept_activation) -> fw::task_t<host::instance_spot_activation_result_t> {
            queued.push_back (command->activation.operation.low);
            accept_activation ();
            if (read_after_delete && command->activation.operation.low == 1)
                co_return co_await first_handler.task ();
            co_return host::instance_spot_activation_result_t{
              0, 0,
              command->activation.request
                ? std::optional<rt::protocol::application_payload_t> (
                    rt::protocol::application_payload_t{"reply", "application/json", {'1'}})
                : std::nullopt};
        }});
    target->start ();
    struct cleanup_t
    {
        std::function<void ()> finish;
        ~cleanup_t () { finish (); }
    };
    const cleanup_t cleanup{[&] {
        store->release.complete (fw::result_t<void>::success ());
        first_handler.complete (
          fw::result_t<host::instance_spot_activation_result_t>::success ({}));
        blob_store->deleted.complete (fw::result_t<void>::success ());
        target->close ();
    }};
    const auto deadline =
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    (std::chrono::system_clock::now () + 3s).time_since_epoch ())
                                    .count ());
    rt::protocol::instance_spot_activation_header_t event{
      {target->status ().routing_id ().to_bytes (), target->status ().lifecycle_generation (),
       "room", "same-target", "room-type", "1", deadline},
      target->status ().lifecycle_generation (),
      target->status ().routing_id ().to_bytes (),
      std::nullopt,
      false,
      {1571, 1},
      0,
      false};
    const rt::protocol::application_payload_t payload{"probe", "application/json", {1}};
    ASSERT_TRUE (target
                   ->send_instance_spot_activation_remote (target->status ().routing_id (), event,
                                                           std::nullopt, payload)
                   .result ());
    auto pump = [&] {
        ASSERT_TRUE (
          target->dispatch_ready ([] (const auto &, const auto &, auto, auto) {}, false).result ());
    };
    const auto until = std::chrono::steady_clock::now () + 3s;
    while ((store->reserves == 0 || (ready_request && store->commits == 0))
           && std::chrono::steady_clock::now () < until)
        pump ();
    ASSERT_EQ (1, store->reserves);
    if (ready_request)
        ASSERT_EQ (1, store->commits);
    auto request = event;
    request.request = true;
    request.operation.low = retransmission_mismatch ? 1 : 2;
    request.reply_route_id = 2;
    if (retransmission_mismatch && mismatch_input == retransmission_input_t::unchanged)
        ++request.target.deadline_unix_ms;
    auto request_payload = payload;
    std::optional<std::vector<std::uint8_t>> request_metadata;
    if (mismatch_input == retransmission_input_t::metadata) {
        request.has_metadata = true;
        request_metadata = std::vector<std::uint8_t>{2};
    }
    if (mismatch_input == retransmission_input_t::payload)
        request_payload.payload_bytes () = {2};
    if (ready_request) {
        const auto authority =
          store->read_authority (rt::spot_authority_key ("room")).result ().value ();
        const auto &snapshot = std::get<fw::authority_snapshot_t> (authority);
        request.target.object_generation = snapshot.object_generation;
        request.target.authority_owner_generation = snapshot.authority_owner_generation;
        request.target.owner_id = snapshot.owner.owner_id;
        request.target.owner_lease_generation = snapshot.owner.lease_generation;
        request.target.store_version = snapshot.store_version;
        request.target.instance_intent = true;
    }
    std::promise<rt::protocol::reply_header_t> reply;
    auto result = reply.get_future ();
    ASSERT_TRUE (target
                   ->activate_instance_spot_remote (
                     target->status ().routing_id (), request, request_metadata, request_payload,
                     3s, [&] (auto, auto header, auto) { reply.set_value (header); })
                   .result ());
    pump ();
    EXPECT_EQ (1, store->reserves);
    EXPECT_TRUE (queued.empty ());
    if (ready_request)
        EXPECT_EQ (std::future_status::timeout, result.wait_for (100ms));
    store->release.complete (fw::result_t<void>::success ());
    if (read_after_delete) {
        while (!blob_store->read_entered.load ()
               && result.wait_for (0ms) != std::future_status::ready
               && std::chrono::steady_clock::now () < until)
            pump ();
        ASSERT_TRUE (blob_store->read_entered.load ()
                     || result.wait_for (0ms) == std::future_status::ready);
        first_handler.complete (
          fw::result_t<host::instance_spot_activation_result_t>::success ({}));
    }
    if (close_during_terminal) {
        auto entered = store->terminal_entered.get_future ();
        while (entered.wait_for (0ms) != std::future_status::ready
               && std::chrono::steady_clock::now () < until)
            pump ();
        if (entered.wait_for (0ms) != std::future_status::ready) {
            store->terminal_release.complete (fw::result_t<void>::success ());
            FAIL () << "Store terminal did not start";
        }
        std::promise<void> closing_entered;
        auto closing = std::async (std::launch::async, [&] {
            closing_entered.set_value ();
            target->close ();
        });
        closing_entered.get_future ().wait ();
        EXPECT_EQ (std::future_status::timeout, closing.wait_for (100ms));
        store->terminal_release.complete (fw::result_t<void>::success ());
        ASSERT_EQ (std::future_status::ready, closing.wait_for (3s));
        closing.get ();
        return;
    }
    while ((result.wait_for (0ms) != std::future_status::ready
            || (read_after_delete && !blob_store->deleted.task ().await_ready ()))
           && std::chrono::steady_clock::now () < until)
        pump ();
    ASSERT_EQ (std::future_status::ready, result.wait_for (0ms));
    EXPECT_EQ (1, store->reserves);
    if (read_after_delete) {
        EXPECT_TRUE (blob_store->deleted.task ().await_ready ());
        EXPECT_FALSE (blob_store->read_entered.load ());
    }
    EXPECT_EQ (retransmission_mismatch
                 ? static_cast<std::uint32_t> (rt::protocol::request_terminal_result::protocolError)
                 : 0u,
               result.get ().terminal_result);
    EXPECT_EQ (retransmission_mismatch ? std::vector<std::uint64_t>{1}
                                       : (std::vector<std::uint64_t>{1, 2}),
               queued);
}

TEST (ZLinkFrameworkInstanceSpotActivation, RawMeshOwnerRequiresRuntimeFailureCollector)
{
    namespace mesh = zlink::framework::runtime::mesh;
    const mesh::raw_mesh_node_options_t options{
      .descriptor = {.mesh_name = "collector-contract",
                     .node_routing_id = zlink::routing_id_t::from ("target").to_bytes (),
                     .lifecycle_generation = 1,
                     .descriptor_revision = 1,
                     .advertised_endpoint = "tcp://127.0.0.1:0"}};
    EXPECT_THROW (mesh::raw_mesh_node_owner_t{options}, std::invalid_argument);
    EXPECT_NO_THROW (
      mesh::raw_mesh_node_owner_t{zlink::framework::test::runtime_failure_options (options)});
}

TEST (ZLinkFrameworkInstanceSpotActivation,
      SameTargetColdOperationsReserveOnceAndQueueInArrivalOrder)
{
    verify_same_target_activation_order (false);
}

TEST (ZLinkFrameworkInstanceSpotActivation, SameTargetRetransmissionMismatchIsRejectedBeforeJoin)
{
    verify_same_target_activation_order (false, true);
}

TEST (ZLinkFrameworkInstanceSpotActivation, PendingAdmissionJoinsAfterRecoveryRootDeletion)
{
    verify_same_target_activation_order (false, false, false, true);
}

TEST (ZLinkFrameworkInstanceSpotActivation, PendingAdmissionRejectsRetransmissionMetadataMismatch)
{
    verify_same_target_activation_order (false, false, false, false,
                                         retransmission_input_t::metadata);
}

TEST (ZLinkFrameworkInstanceSpotActivation, PendingAdmissionRejectsRetransmissionPayloadMismatch)
{
    verify_same_target_activation_order (false, false, false, false,
                                         retransmission_input_t::payload);
}

TEST (ZLinkFrameworkInstanceSpotActivation, HostCloseWaitsForAcceptedActivationStoreTerminal)
{
    verify_same_target_activation_order (false, true, true);
}

TEST (ZLinkFrameworkInstanceSpotActivation, ReadyOperationJoinsBeforeDurableFirstRecordAdmission)
{
    verify_same_target_activation_order (true);
}
} // namespace
