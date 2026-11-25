/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/execution_cases.hpp"
#include "../support/execution_actor_join_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/mesh/service_mailbox.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include <zlink/framework/contracts/spots/spot.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace zlink::framework::test::execution
{
bool verify_remote_actor_prepare_is_idempotent ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-prepare-idempotency-node");
    auto target = std::make_shared<spot_context_state_t> ();
    target->node = node;
    target->node_rid = node_rid_t::from_string ("actor-prepare-idempotency-node");
    target->spot_id = spot_id_t ("target-spot");
    target->spot_name = "target";
    target->spot_instance = std::make_shared<int> (1);
    target->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    target->channel_runtime->serializers = &serializers;
    target->serial_executor =
      std::make_shared<runtime::offload_executor_t> (2, "actor-prepare-idempotency");
    target->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *target->serial_executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    node->spot_contexts_by_id.emplace (target->spot_id, spot_context_access_t::create (target));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    node->actor_factories.emplace ("player", std::move (factory));
    int admission_calls = 0;
    spot_actor_admission_callbacks_t callbacks;
    callbacks.join = [&] (void *, std::string_view, const zlink::message_t &,
                          serializer_registry_t &) {
        ++admission_calls;
        return spot_actor_join_result_t::accept (message_t::from (std::string ("accepted")));
    };
    target->actor_admissions.emplace (std::type_index (typeid (int)), std::move (callbacks));

    spot_node_runtime_t owner (node);
    const auto relocation_store = std::make_shared<runtime::in_memory_relocation_store_t> ();
    const auto relocation_repository =
      std::make_shared<runtime::provider_relocation_repository_t> (*relocation_store);
    owner.bind_relocation_store (
      std::make_shared<runtime::stateful::public_relocation_store_adapter_t> (
        relocation_repository));
    const auto actor =
      actor_ref_access_t::make (node_rid_t::from_string ("source-node"), "player", "actor-1", 7);
    auto store = std::make_shared<wire_actor_join_authority_store_t> ();
    store->snapshot = authority_snapshot_t{
      .store_version = "actor-prepare-v1",
      .payload = runtime::encode_actor_authority_payload (actor, "source-spot", 1),
      .object_generation = 7,
      .authority_owner_generation = 19,
      .owner = location_owner_token_t{"source-owner", 29},
      .store_now = std::chrono::system_clock::now (),
      .allocation = {.state = placement_allocation_state_t::active,
                     .object_kind = placement_object_kind_t::actor,
                     .stable_type = "player",
                     .target = {.mesh_name = "actor-prepare",
                                .node_rid = node_rid_t::from_string ("source-node"),
                                .node_lifecycle_generation = 23,
                                .owner = location_owner_token_t{"source-owner", 29}}}};
    service_collection_t services;
    services.add_factory<runtime::live_location_reader_t> (
      [store] (service_provider_t &) {
          return std::make_unique<runtime::live_location_reader_t> (*store);
      },
      service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    owner.bind_service_provider (provider);
    const auto request = zlink::message_t::from (std::string ("prepare"));
    const auto first =
      owner
        .admit_remote_actor_to_spot ("transfer-1", actor, spot_id_t ("source-spot"),
                                     target->spot_id, request, 11, 13, 19, 23, 29)
        .result ();
    const auto repeated =
      owner
        .admit_remote_actor_to_spot ("transfer-1", actor, spot_id_t ("source-spot"),
                                     target->spot_id, request, 11, 13, 19, 23, 29)
        .result ();
    const auto conflicting =
      owner
        .admit_remote_actor_to_spot ("transfer-1", actor, spot_id_t ("source-spot"),
                                     target->spot_id, request, 11, 17, 19, 23, 29)
        .result ();

    target->serial_queue->close ();
    target->serial_queue->drain ();
    target->serial_executor->drain ();
    const auto passed = first && repeated && first.value ().accepted && repeated.value ().accepted
                        && first.value ().reply && repeated.value ().reply
                        && first.value ().reply->decode<std::string> () == "accepted"
                        && repeated.value ().reply->decode<std::string> () == "accepted"
                        && !conflicting
                        && conflicting.error_kind () == framework_error_kind_t::protocol_error
                        && admission_calls == 1;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}
// actorJoin(28) receiver admission is APPROVAL-ONLY (spec 15 §478-527):
// admission registers the relocation temporary queue (identity-keyed
// pending admission) with the prepared factory and replies approval — it
// must NOT construct/install the Actor, claim the target location, or
// advance membership; those belong to the later transfer/commit stages.
// A newer wire attempt for the same actor identity evicts a parked older
// attempt (later-attempt-wins, spec 15 §542-546), while a duplicate resend
// of the SAME attempt parks against the existing preparation without
// re-running the application admission callback.
bool verify_wire_actor_join_admission_is_approval_only_and_later_attempt_wins ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("wire-join-node");
    auto target = std::make_shared<spot_context_state_t> ();
    target->node = node;
    target->node_rid = node_rid_t::from_string ("wire-join-node");
    target->spot_id = spot_id_t ("target-spot");
    target->spot_name = "target";
    target->spot_instance = std::make_shared<int> (1);
    target->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    target->channel_runtime->serializers = &serializers;
    target->serial_executor =
      std::make_shared<runtime::offload_executor_t> (2, "wire-join-admission");
    target->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *target->serial_executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    node->spot_contexts_by_id.emplace (target->spot_id, spot_context_access_t::create (target));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    node->actor_factories.emplace ("player", std::move (factory));
    int admission_calls = 0;
    bool return_application_reply = true;
    spot_actor_admission_callbacks_t callbacks;
    callbacks.join = [&] (void *, std::string_view, const zlink::message_t &,
                          serializer_registry_t &) {
        ++admission_calls;
        return return_application_reply
                 ? spot_actor_join_result_t::accept (message_t::from (std::string ("approved")))
                 : spot_actor_join_result_t::accept ();
    };
    target->actor_admissions.emplace (std::type_index (typeid (int)), std::move (callbacks));

    spot_node_runtime_t owner (node);
    const auto local_rid = zlink::routing_id_t::from (std::string ("wire-join-node"));
    const auto source_rid = zlink::routing_id_t::from (std::string ("wire-join-source"));
    auto store = std::make_shared<wire_actor_join_authority_store_t> ();
    const auto stored_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("wire-join-source"), "player", "actor-1", 7);
    store->snapshot = authority_snapshot_t{
      .store_version = "wire-join-v1",
      .payload = runtime::encode_actor_authority_payload (stored_actor, "source-spot", 1),
      .object_generation = 7,
      .authority_owner_generation = 19,
      .owner = location_owner_token_t{"source-owner", 5},
      .store_now = std::chrono::system_clock::now (),
      .allocation = {.state = placement_allocation_state_t::active,
                     .object_kind = placement_object_kind_t::actor,
                     .stable_type = "player",
                     .target = {.mesh_name = "wire-join",
                                .node_rid = node_rid_t::from_string ("wire-join-source"),
                                .node_lifecycle_generation = 3,
                                .owner = location_owner_token_t{"source-owner", 5}}}};
    store->spot_snapshot = authority_snapshot_t{
      .store_version = "wire-join-target-v1",
      .object_generation = 9,
      .authority_owner_generation = 21,
      .owner = location_owner_token_t{"target-owner", 22},
      .store_now = std::chrono::system_clock::now (),
      .allocation = {.state = placement_allocation_state_t::active,
                     .object_kind = placement_object_kind_t::user_spot,
                     .stable_type = "target",
                     .target = {.mesh_name = "wire-join",
                                .node_rid = node_rid_t::from_string ("wire-join-node"),
                                .node_lifecycle_generation = 1,
                                .owner = location_owner_token_t{"target-owner", 22}}}};
    service_collection_t services;
    services.add_factory<runtime::live_location_reader_t> (
      [store] (service_provider_t &) {
          return std::make_unique<runtime::live_location_reader_t> (*store);
      },
      service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    owner.bind_service_provider (provider);
    const auto resolver_owner = std::make_shared<wire_join_spot_resolver_t> ();
    auto &resolver = *resolver_owner;
    resolver.address.node_rid = local_rid;
    resolver.address.spot_id = "target-spot";
    // Deliberately stale: canonical target admission must ignore this
    // cacheable routing projection and read store->spot_snapshot instead.
    resolver.address.spot_generation = 8;
    resolver.address.node_generation = 2;
    resolver.address.authority_owner_generation = 20;
    resolver.address.owner = location_owner_token_t{"stale-owner", 23};
    owner.bind_spot_location_resolver (resolver_owner);

    const auto make_request = [&] (std::uint64_t correlation) {
        return runtime::protocol::actor_join_request_t{
          correlation,
          runtime::protocol::actor_route_fence_t{"actor-1", 7, source_rid.to_bytes (), 3, 19, 5},
          false,
          runtime::protocol::spot_route_fence_t{"target-spot", 9, local_rid.to_bytes (), 1, 21,
                                                22}};
    };
    const auto transfer_id_for = [&] (std::uint64_t correlation) {
        return canonical_actor_join_handoff_id (source_rid.to_bytes (), "actor-1", 7, 3,
                                                correlation);
    };

    runtime::host::actor_join_operation_result_t first;
    runtime::mesh::actor_join_wire_outcome_t wire_outcome;
    const auto reply_round_trip = [&] {
        using namespace std::chrono_literals;
        const auto descriptor = [] (std::string rid, std::uint64_t generation) {
            return runtime::mesh::service_node_descriptor_t{
              "wire-join",
              zlink::routing_id_t::from (std::move (rid)).to_bytes (),
              generation,
              1,
              "tcp://127.0.0.1:0",
              {{"actor-join", 100}},
              runtime::mesh::service_node_state_t::preparing};
        };
        runtime::mesh::raw_mesh_node_owner_t source (runtime::mesh::raw_mesh_node_options_t{
          .descriptor = descriptor ("wire-join-source", 3),
          .runtime_failures =
            std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
        runtime::mesh::raw_mesh_node_owner_t target_owner (runtime::mesh::raw_mesh_node_options_t{
          .descriptor = descriptor ("wire-join-node", 1),
          .runtime_failures =
            std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
        source.start ();
        target_owner.start ();
        const auto target_descriptor = target_owner.topology ().local_descriptor ();
        source.expect_peer (target_descriptor);
        const bool connected = source.connect_peer (target_owner.endpoint (), target_descriptor);
        const auto connect_deadline = std::chrono::steady_clock::now () + 2s;
        const auto peer_ready = [&] {
            const auto peer = source.topology ().peer (target_descriptor.node_routing_id);
            return peer && peer->ready;
        };
        while (connected && !peer_ready ()
               && std::chrono::steady_clock::now () < connect_deadline) {
            const auto now = runtime::mesh::service_liveness_registry_t::clock_t::now ();
            (void) source.observe_routes ();
            (void) target_owner.observe_routes ();
            (void) source.pump_one (now).result ();
            (void) target_owner.pump_one (now).result ();
            std::this_thread::sleep_for (1ms);
        }
        if (!connected || !peer_ready ()) {
            source.close ();
            target_owner.close ();
            return false;
        }

        const auto request = make_request (4211);
        std::atomic_bool settled{false};
        std::thread request_thread ([&] {
            const auto result =
              source
                .request_actor_join (target_descriptor.node_routing_id, request, std::nullopt, 2s)
                .result ();
            if (result)
                wire_outcome = result.value ();
            settled.store (true, std::memory_order_release);
        });

        std::shared_ptr<runtime::mesh::service_mailbox_claim_t> claim;
        const auto claim_deadline = std::chrono::steady_clock::now () + 2s;
        while (!claim && std::chrono::steady_clock::now () < claim_deadline) {
            claim = target_owner.mailbox ().try_claim (
              runtime::mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
            if (!claim) {
                (void) target_owner
                  .pump_one (runtime::mesh::service_liveness_registry_t::clock_t::now ())
                  .result ();
            }
        }
        bool replied = false;
        if (claim && claim->records.size () == 1) {
            const auto &record = (*claim->records.front ());
            first = admit_wire_actor_join (node, local_rid, request, std::nullopt, &serializers);
            replied = target_owner.reply_actor_join (
              record, first.join_result, first.spot, first.membership_epoch,
              first.receive_chunk_limit_bytes, first.terminal_result, first.failure_code,
              first.application_reply);
            replied = target_owner.mailbox ().release (*claim) && replied;
        }

        const auto settle_deadline = std::chrono::steady_clock::now () + 2s;
        while (!settled.load (std::memory_order_acquire)
               && std::chrono::steady_clock::now () < settle_deadline) {
            (void) source.pump_one (runtime::mesh::service_liveness_registry_t::clock_t::now ())
              .result ();
            std::this_thread::sleep_for (1ms);
        }
        request_thread.join ();
        source.close ();
        target_owner.close ();

        const auto encoded_reply =
          detail::message_to_raw (message_t::from (std::string ("approved")), serializers)
            .to_bytes ();
        std::vector<std::uint8_t> expected_payload{
          0,
          0,
          0,
          1,
          static_cast<std::uint8_t> ((encoded_reply.size () >> 24u) & 0xffu),
          static_cast<std::uint8_t> ((encoded_reply.size () >> 16u) & 0xffu),
          static_cast<std::uint8_t> ((encoded_reply.size () >> 8u) & 0xffu),
          static_cast<std::uint8_t> (encoded_reply.size () & 0xffu)};
        expected_payload.insert (expected_payload.end (), encoded_reply.begin (),
                                 encoded_reply.end ());
        const auto unwrapped_reply =
          first.application_reply
            ? unwrap_canonical_actor_join_application_reply (*first.application_reply)
            : std::vector<std::uint8_t>{};
        const auto reconstructed_reply = zlink::message_t::from (unwrapped_reply);
        return replied && settled.load (std::memory_order_acquire) && first.application_reply
               && wire_outcome.application_reply
               && first.application_reply == wire_outcome.application_reply
               && wire_outcome.application_reply->packet_name ()
                    == runtime::protocol::framework_multipart_packet_name
               && wire_outcome.application_reply->content_type ()
                    == runtime::protocol::framework_multipart_content_type
               && wire_outcome.application_reply->payload_bytes () == expected_payload
               && unwrapped_reply == encoded_reply
               && serializers.get<std::string> ().deserialize (
                    detail::encoded_payload_from_raw (reconstructed_reply))
                    == "approved";
    }();
    const bool first_approved =
      first.join_result == runtime::protocol::actor_join_result_t::accepted && first.spot
      && first.spot->spot_id == "target-spot"
      && first.spot->object_generation == 9
      // Approval-only: the accepted reply carries the PROPOSED membership
      // epoch (no membership has moved, so current-none + 1 == 1) and the
      // conservative advertised receive chunk limit.
      && first.membership_epoch == 1 && first.receive_chunk_limit_bytes == 32768;
    // No lifecycle side effects during admission: no Actor instance was
    // constructed or installed, no membership was recorded, and no target
    // location/commit state exists — only the temporary-queue registration
    // (target_pending pending admission) on the coordinator.
    const bool approval_only =
      node->actor_instances.empty () && node->actor_spot_ids.empty ()
      && node->core_actor_membership_epochs.empty ()
      && node->actor_transfer_coordinator->admission (transfer_id_for (4211)).has_value ()
      && node->actor_transfer_coordinator->phase ("player:actor-1")
           == actor_move_phase_t::target_pending;

    // A duplicate resend of the same attempt parks against the existing
    // preparation without re-running the application admission callback.
    const auto repeated =
      admit_wire_actor_join (node, local_rid, make_request (4211), std::nullopt, &serializers);
    const bool duplicate_parked =
      repeated.join_result == runtime::protocol::actor_join_result_t::accepted
      && admission_calls == 1;

    const auto reply_without_serializers =
      admit_wire_actor_join (node, local_rid, make_request (4211), std::nullopt, nullptr);
    const bool reply_requires_serializers =
      reply_without_serializers.terminal_result == 104
      && reply_without_serializers.failure_code
           == static_cast<std::uint32_t> (
             runtime::protocol::framework_error_code::requestProtocolError);

    // Later-attempt-wins: a NEWER attempt (fresh correlation → distinct
    // derived transfer identity) evicts the parked older attempt.
    return_application_reply = false;
    const auto newer =
      admit_wire_actor_join (node, local_rid, make_request (4213), std::nullopt, nullptr);
    const bool later_attempt_wins =
      newer.join_result == runtime::protocol::actor_join_result_t::accepted
      && newer.terminal_result == 0 && !newer.application_reply
      && !node->actor_transfer_coordinator->admission (transfer_id_for (4211)).has_value ()
      && node->actor_transfer_coordinator->admission (transfer_id_for (4213)).has_value ()
      && node->actor_transfer_coordinator->is_current ("player:actor-1", transfer_id_for (4213))
      && node->actor_instances.empty () && node->actor_spot_ids.empty ();

    // A missing Authority row is a typed NotFound terminal, not an
    // application-level Rejected result.
    const auto actor_snapshot = store->snapshot;
    store->snapshot.reset ();
    auto unknown_request = make_request (4215);
    unknown_request.actor.actor_id = "actor-unknown";
    const auto unknown =
      admit_wire_actor_join (node, local_rid, unknown_request, std::nullopt, &serializers);
    const bool unknown_not_found =
      unknown.terminal_result == 102
      && unknown.failure_code
           == static_cast<std::uint32_t> (
             runtime::protocol::framework_error_code::requestTargetNotFound)
      && !unknown.spot;
    store->snapshot = actor_snapshot;

    auto stale_request = make_request (4216);
    ++stale_request.actor.authority_owner_generation;
    const auto stale =
      admit_wire_actor_join (node, local_rid, stale_request, std::nullopt, &serializers);
    const bool stale_protocol_error =
      stale.terminal_result == 104
      && stale.failure_code
           == static_cast<std::uint32_t> (
             runtime::protocol::framework_error_code::requestProtocolError)
      && !stale.spot;

    // Command-28 admission must continue to reject a route fence that does
    // not exactly match the committed target Spot Authority row.
    auto stale_target_store_fence_request = make_request (4218);
    ++stale_target_store_fence_request.target_spot.authority_owner_generation;
    const auto stale_target_store_fence = admit_wire_actor_join (
      node, local_rid, stale_target_store_fence_request, std::nullopt, &serializers);
    const bool stale_target_store_fence_protocol_error =
      stale_target_store_fence.terminal_result == 104
      && stale_target_store_fence.failure_code
           == static_cast<std::uint32_t> (
             runtime::protocol::framework_error_code::requestProtocolError)
      && !stale_target_store_fence.spot;

    const auto malformed_terminal = [&] (std::string actor_id, std::string spot_id) {
        auto malformed = make_request (4217);
        malformed.actor.actor_id = std::move (actor_id);
        malformed.target_spot.spot_id = std::move (spot_id);
        const auto result =
          admit_wire_actor_join (node, local_rid, malformed, std::nullopt, &serializers);
        return result.terminal_result == 104
               && result.failure_code
                    == static_cast<std::uint32_t> (
                      runtime::protocol::framework_error_code::requestProtocolError);
    };
    const bool malformed_typed =
      malformed_terminal (" \t", "target-spot")
      && malformed_terminal (std::string ("actor\0bad", 9), "target-spot")
      && malformed_terminal ("actor-1", " \n")
      && malformed_terminal ("actor-1", std::string ("spot\0bad", 8));

    // The first Join retains the Spot lifecycle position. The next admission
    // must return to the receiver before that receiver processes the terminal.
    auto second_actor = make_request (4220);
    second_actor.actor.actor_id = "actor-2";
    auto second_snapshot = *actor_snapshot;
    second_snapshot.payload = runtime::encode_actor_authority_payload (
      actor_ref_access_t::make (node_rid_t::from_string ("wire-join-source"), "player", "actor-2",
                                7),
      "source-spot", 1);
    store->snapshot = second_snapshot;
    auto second_promise =
      std::make_shared<std::promise<runtime::host::actor_join_operation_result_t>> ();
    auto second_future = second_promise->get_future ();
    owner.dispatch_wire_actor_join_admission (
      spot_id_t ("target-spot"), second_actor.actor.actor_id,
      [node, local_rid, second_actor, &serializers, second_promise] () -> task_t<void> {
          second_promise->set_value (
            admit_wire_actor_join (node, local_rid, second_actor, std::nullopt, &serializers));
          co_return;
      },
      [second_promise] (result_t<void>) { second_promise->set_value ({}); });
    node->actor_transfer_coordinator->fail_commit (transfer_id_for (4213), false);
    const bool second_completed =
      second_future.wait_for (std::chrono::seconds (2)) == std::future_status::ready
      && second_future.get ().join_result == runtime::protocol::actor_join_result_t::accepted;
    const auto second_transfer_id =
      canonical_actor_join_handoff_id (source_rid.to_bytes (), "actor-2", 7, 3, 4220);
    node->actor_transfer_coordinator->fail_commit (second_transfer_id, false);
    store->snapshot = actor_snapshot;
    service_collection_t unavailable_services;
    auto unavailable_provider = unavailable_services.build_provider ();
    owner.bind_service_provider (unavailable_provider);
    const auto unavailable =
      admit_wire_actor_join (node, local_rid, make_request (4221), std::nullopt, &serializers);
    const bool unavailable_kind_preserved =
      unavailable.terminal_result == 105
      && unavailable.failure_code
           == static_cast<std::uint32_t> (
             runtime::protocol::framework_error_code::routeNotConnected)
      && runtime::protocol::valid_terminal_failure (
        unavailable.terminal_result,
        static_cast<runtime::protocol::framework_error_code> (unavailable.failure_code))
      && runtime::host::classify_relocation_failure_code (unavailable.failure_code)
           == framework_error_kind_t::unavailable;
    target->serial_queue->close ();
    target->serial_queue->drain ();
    target->serial_executor->drain ();
    const auto passed = reply_round_trip && first_approved && approval_only && duplicate_parked
                        && reply_requires_serializers && later_attempt_wins && unknown_not_found
                        && stale_protocol_error && stale_target_store_fence_protocol_error
                        && malformed_typed && second_completed && unavailable_kind_preserved;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}

bool verify_wire_join_requires_active_local_target ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("wire-join-inactive-target");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (2);
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    serializer_registry_t serializers;
    node->channel_runtime->serializers = &serializers;
    spot_node_runtime_t owner (node);
    std::atomic<int> materializations{0};
    node->spot_factories.emplace ("cold", std::type_index (typeid (int)));
    spot_lifecycle_callbacks_t lifecycle;
    lifecycle.create_spot_context_instance = [&] (spot_context_t,
                                                  service_provider_t &) -> std::shared_ptr<void> {
        ++materializations;
        return std::make_shared<int> (1);
    };
    node->spot_lifecycles.emplace ("cold", std::move (lifecycle));
    auto inactive = std::make_shared<spot_context_state_t> ();
    inactive->node = node;
    inactive->spot_id = spot_id_t ("inactive-target");
    node->spot_contexts_by_id.emplace (inactive->spot_id, spot_context_access_t::create (inactive));
    std::atomic<int> accepted{0};
    std::atomic<int> unavailable{0};
    const auto receive = [&] (const char *target) {
        owner.dispatch_wire_actor_join_admission (
          spot_id_t (target), std::string_view{},
          [&] () -> task_t<void> {
              ++accepted;
              co_return;
          },
          [&] (result_t<void>) { ++unavailable; });
    };
    receive ("cold-target");
    receive ("inactive-target");
    receive ("following-target");
    const bool immediate_replies = unavailable == 3 && accepted == 0;
    node->worker_executor->drain ();
    const auto passed = immediate_replies && materializations == 0
                        && node->spot_contexts_by_id.count (spot_id_t ("cold-target")) == 0
                        && node->spot_contexts_by_id.count (spot_id_t ("following-target")) == 0;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}

bool verify_relocation_abort_does_not_hold_receive_worker ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("relocation-abort-node");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (2);
    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("relocation-abort-node");
    spot->spot_id = spot_id_t ("abort-target");
    spot->spot_name = "abort-target";
    spot->spot_instance = std::make_shared<int> (1);
    spot->serial_executor = node->worker_executor;
    spot->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *spot->serial_executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    std::promise<void> closing_started;
    auto started = closing_started.get_future ();
    std::promise<void> release_closing;
    auto released = release_closing.get_future ().share ();
    spot->lifecycle.on_closing = [&] (void *, const spot_closing_context_t &, std::stop_token) {
        closing_started.set_value ();
        released.wait ();
        return task_t<void> (result_t<void>::success ());
    };
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));
    spot_node_runtime_t owner (node);
    std::promise<void> following_frame;
    auto following = following_frame.get_future ();
    std::thread receiver ([&] {
        owner
          .abort_relocation_materialization_async (
            {{.kind = runtime::stateful::object_kind_t::user_spot, .key = "abort-target"}})
          .result ()
          .value ();
        owner.dispatch_wire_actor_join_admission (
          spot_id_t ("following-target"), std::string_view{}, [] () -> task_t<void> { co_return; },
          [&] (result_t<void>) { following_frame.set_value (); });
    });
    const bool entered = started.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    const bool progressed =
      entered && following.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    release_closing.set_value ();
    receiver.join ();
    spot->serial_queue->drain ();
    node->worker_executor->drain ();
    return progressed;
}

bool verify_target_commit_stages_source_prefix_before_live_dispatch ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_transfer_coordinator_t coordinator;
    std::vector<handoff_packet_t> discarded;
    const auto source = actor_ref_access_t::make (node_rid_t::from_string ("source-node"), "player",
                                                  "actor-cutover-order", 7);
    pending_actor_admission_t admission{.actor_key = "player:actor-cutover-order",
                                        .source_actor = source,
                                        .source_spot_id = "source-spot",
                                        .target_spot_id = "target-spot",
                                        .deadline = std::chrono::steady_clock::now ()
                                                    + std::chrono::seconds (30),
                                        .completion_operation_id_high = 31,
                                        .completion_operation_id_low = 37};
    if (!coordinator.try_add_admission ("transfer-cutover-order", admission)
        || !coordinator.begin_commit ("transfer-cutover-order", source, "target-spot", discarded)) {
        return false;
    }
    const auto packet = [] (std::string sequence) {
        handoff_packet_t value;
        value.packet_name = "handoff";
        value.metadata.emplace ("sequence", std::move (sequence));
        return value;
    };
    if (!coordinator.stage_commit_backlog ("transfer-cutover-order", {packet ("B1"), packet ("B2")})
        || coordinator.try_append_backlog ("player:actor-cutover-order", packet ("D1"))
             != handoff_append_result_t::appended) {
        return false;
    }
    const auto replay = coordinator.complete_commit_and_take_backlog ("transfer-cutover-order",
                                                                      source, "target-spot");
    return replay && replay->size () == 3 && (*replay)[0].metadata.at ("sequence") == "B1"
           && (*replay)[1].metadata.at ("sequence") == "B2"
           && (*replay)[2].metadata.at ("sequence") == "D1";
}

// Spec 15 §4.2 step 2-4: an arrival for the joining Actor between
// OnActorJoin Accepted (target_pending) and PREPARE (target_committing)
// must park in the relocation temporary queue instead of being dropped, and
// PREPARE must migrate it into the real queue in order — ahead of anything
// that arrives after PREPARE starts. This drives the production admission
// entry points directly (try_add_admission / try_append_backlog /
// begin_commit / complete_commit_and_take_backlog), the same level this
// suite already uses for
// verify_target_commit_stages_source_prefix_before_live_dispatch.
bool verify_actor_join_prewarm_parks_arrival_before_prepare ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_transfer_coordinator_t coordinator;
    std::vector<handoff_packet_t> discarded;
    const auto source = actor_ref_access_t::make (node_rid_t::from_string ("source-node"), "player",
                                                  "actor-prewarm", 7);
    pending_actor_admission_t admission{.actor_key = "player:actor-prewarm",
                                        .source_actor = source,
                                        .source_spot_id = "source-spot",
                                        .target_spot_id = "target-spot",
                                        .deadline = std::chrono::steady_clock::now ()
                                                    + std::chrono::seconds (30),
                                        .completion_operation_id_high = 41,
                                        .completion_operation_id_low = 43};
    if (!coordinator.try_add_admission ("transfer-prewarm", admission)) {
        return false;
    }
    // Accepted has returned to source; PREPARE has not run yet. An arrival
    // for this exact Actor must park here (spec 15 §4.2 "temporary
    // queue가 등록되어 있어도 ... application handler를 실행하지 않는다") —
    // before the fix, target_pending was not in try_append_backlog's
    // allowed-phase set and this call returned not_moving, letting
    // relay_actor_packet fall through to entry-spot auto-join or a
    // not-found failure instead of parking.
    if (coordinator.phase ("player:actor-prewarm") != actor_move_phase_t::target_pending) {
        return false;
    }
    const auto packet = [] (std::string sequence) {
        handoff_packet_t value;
        value.packet_name = "handoff";
        value.metadata.emplace ("sequence", std::move (sequence));
        return value;
    };
    if (coordinator.try_append_backlog ("player:actor-prewarm", packet ("EARLY"))
        != handoff_append_result_t::appended) {
        return false;
    }
    if (!coordinator.begin_commit ("transfer-prewarm", source, "target-spot", discarded)) {
        return false;
    }
    if (coordinator.try_append_backlog ("player:actor-prewarm", packet ("AFTER_PREPARE"))
        != handoff_append_result_t::appended) {
        return false;
    }
    const auto replay =
      coordinator.complete_commit_and_take_backlog ("transfer-prewarm", source, "target-spot");
    return replay && replay->size () == 2 && (*replay)[0].metadata.at ("sequence") == "EARLY"
           && (*replay)[1].metadata.at ("sequence") == "AFTER_PREPARE";
}

// Spec 15 §4.2 "같은 object의 relocation temporary queue는 하나만 존재한다" /
// "나중 attempt가 유효하며": a newer exact identity (different transfer_id)
// for the same object displaces an admission-time placeholder that has not
// started PREPARE yet, and any arrival parked for the displaced identity
// does not leak into the new attempt's replay.
bool verify_actor_join_prewarm_newest_attempt_evicts_placeholder ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_transfer_coordinator_t coordinator;
    std::vector<handoff_packet_t> discarded;
    const auto source = actor_ref_access_t::make (node_rid_t::from_string ("source-node"), "player",
                                                  "actor-evict", 7);
    pending_actor_admission_t first{.actor_key = "player:actor-evict",
                                    .source_actor = source,
                                    .source_spot_id = "source-spot",
                                    .target_spot_id = "target-spot",
                                    .deadline =
                                      std::chrono::steady_clock::now () + std::chrono::seconds (30),
                                    .completion_operation_id_high = 51,
                                    .completion_operation_id_low = 53};
    pending_actor_admission_t second = first;
    second.completion_operation_id_low = 59;

    if (!coordinator.try_add_admission ("transfer-evict-1", first)) {
        return false;
    }
    const auto packet = [] (std::string sequence) {
        handoff_packet_t value;
        value.packet_name = "handoff";
        value.metadata.emplace ("sequence", std::move (sequence));
        return value;
    };
    if (coordinator.try_append_backlog ("player:actor-evict", packet ("STALE"))
        != handoff_append_result_t::appended) {
        return false;
    }
    // A newer exact identity for the same object arrives before the first
    // reaches PREPARE — it must win, and the stale placeholder (including
    // whatever it parked) must not survive.
    if (!coordinator.admit_attempt ("player:actor-evict", "transfer-evict-2", discarded)
        || !coordinator.try_add_admission ("transfer-evict-2", second)) {
        return false;
    }
    if (discarded.size () != 1 || discarded[0].metadata.at ("sequence") != "STALE")
        return false;
    discarded.clear ();
    if (coordinator.admission ("transfer-evict-1").has_value ()) {
        return false;
    }
    if (coordinator.phase ("player:actor-evict") != actor_move_phase_t::target_pending) {
        return false;
    }
    if (coordinator.try_append_backlog ("player:actor-evict", packet ("FRESH"))
        != handoff_append_result_t::appended) {
        return false;
    }
    if (!coordinator.begin_commit ("transfer-evict-2", source, "target-spot", discarded)) {
        return false;
    }
    const auto replay =
      coordinator.complete_commit_and_take_backlog ("transfer-evict-2", source, "target-spot");
    return replay && replay->size () == 1 && (*replay)[0].metadata.at ("sequence") == "FRESH";
}

// Rejected/expiry/prepare-failure cleanup must release a parked backlog
// exactly once — not strand it under the actor_key for a later, unrelated
// admission to inherit (spec 15 §4.2 cleanup requirement).
bool verify_actor_join_prewarm_fail_commit_clears_parked_backlog ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_transfer_coordinator_t coordinator;
    std::vector<handoff_packet_t> discarded;
    const auto source =
      actor_ref_access_t::make (node_rid_t::from_string ("source-node"), "player", "actor-fail", 7);
    pending_actor_admission_t admission{.actor_key = "player:actor-fail",
                                        .source_actor = source,
                                        .source_spot_id = "source-spot",
                                        .target_spot_id = "target-spot",
                                        .deadline = std::chrono::steady_clock::now ()
                                                    + std::chrono::seconds (30),
                                        .completion_operation_id_high = 61,
                                        .completion_operation_id_low = 63};
    if (!coordinator.try_add_admission ("transfer-fail", admission)) {
        return false;
    }
    handoff_packet_t stale;
    stale.packet_name = "handoff";
    stale.metadata.emplace ("sequence", "STRANDED");
    if (coordinator.try_append_backlog ("player:actor-fail", stale)
        != handoff_append_result_t::appended) {
        return false;
    }
    if (!coordinator.begin_commit ("transfer-fail", source, "target-spot", discarded)) {
        return false;
    }
    auto failed_backlog = coordinator.fail_commit ("transfer-fail", false);
    if (failed_backlog.size () != 1 || failed_backlog[0].metadata.at ("sequence") != "STRANDED")
        return false;
    if (coordinator.phase ("player:actor-fail").has_value ()) {
        return false;
    }
    // A later, unrelated admission for the same object must start clean.
    pending_actor_admission_t retry = admission;
    if (!coordinator.try_add_admission ("transfer-fail-retry", retry)) {
        return false;
    }
    if (!coordinator.begin_commit ("transfer-fail-retry", source, "target-spot", discarded)) {
        return false;
    }
    const auto replay =
      coordinator.complete_commit_and_take_backlog ("transfer-fail-retry", source, "target-spot");
    return replay && replay->empty ();
}

// Spec 15 §4.2 newest-attempt-wins parity: a newer exact identity must
// evict an older attempt even after that older attempt is past PREPARE
// (target_committing) — not only the admission-time placeholder
// (target_pending) verify_actor_join_prewarm_newest_attempt_evicts_placeholder
// already covers. Attempt A parks a frame, reaches target_committing (past
// PREPARE) and parks a second frame there; attempt B then arrives with a
// different transfer_id for the same object and must win: A's admission and
// both of its parked frames are gone (failed exactly once — the same single
// erase fail_commit/cleanup_expired already use to reclaim a dead attempt),
// and B parks and replays its own arrival cleanly.
bool verify_actor_join_prewarm_newest_attempt_evicts_live_attempt_past_prepare ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_transfer_coordinator_t coordinator;
    std::vector<handoff_packet_t> discarded;
    const auto source = actor_ref_access_t::make (node_rid_t::from_string ("source-node"), "player",
                                                  "actor-evict-live", 7);
    const std::string key = "player:actor-evict-live";
    pending_actor_admission_t first{.actor_key = key,
                                    .source_actor = source,
                                    .source_spot_id = "source-spot",
                                    .target_spot_id = "target-spot",
                                    .deadline =
                                      std::chrono::steady_clock::now () + std::chrono::seconds (30),
                                    .completion_operation_id_high = 71,
                                    .completion_operation_id_low = 73};
    pending_actor_admission_t second = first;
    second.completion_operation_id_low = 79;

    const auto packet = [] (std::string sequence) {
        handoff_packet_t value;
        value.packet_name = "handoff";
        value.metadata.emplace ("sequence", std::move (sequence));
        return value;
    };

    if (!coordinator.try_add_admission ("transfer-evict-live-A", first)) {
        return false;
    }
    if (coordinator.try_append_backlog (key, packet ("A_PARKED_EARLY"))
        != handoff_append_result_t::appended) {
        return false;
    }
    // Attempt A reaches PREPARE (target_committing) — the "live attempt"
    // case, distinct from the target_pending placeholder the other eviction
    // test covers.
    if (!coordinator.begin_commit ("transfer-evict-live-A", source, "target-spot", discarded)
        || coordinator.phase (key) != actor_move_phase_t::target_committing
        || !coordinator.is_current (key, "transfer-evict-live-A")) {
        return false;
    }
    if (coordinator.try_append_backlog (key, packet ("A_PARKED_AFTER_PREPARE"))
        != handoff_append_result_t::appended) {
        return false;
    }

    // A newer exact identity for the same object arrives — it must win even
    // though A is already past PREPARE.
    if (!coordinator.admit_attempt (key, "transfer-evict-live-B", discarded)
        || !coordinator.try_add_admission ("transfer-evict-live-B", second)) {
        return false;
    }
    if (discarded.size () != 2 || discarded[0].metadata.at ("sequence") != "A_PARKED_EARLY"
        || discarded[1].metadata.at ("sequence") != "A_PARKED_AFTER_PREPARE")
        return false;
    discarded.clear ();
    // A is gone: its admission record and both frames it parked (failed
    // exactly once, by the same single erase fail_commit/cleanup_expired
    // use) do not survive the eviction.
    if (coordinator.admission ("transfer-evict-live-A").has_value ()
        || coordinator.is_current (key, "transfer-evict-live-A")
        || coordinator.phase (key) != actor_move_phase_t::target_pending) {
        return false;
    }
    // B starts clean and parks its own arrival.
    if (coordinator.try_append_backlog (key, packet ("B_PARKED"))
        != handoff_append_result_t::appended) {
        return false;
    }
    if (!coordinator.begin_commit ("transfer-evict-live-B", source, "target-spot", discarded)) {
        return false;
    }
    const auto replay =
      coordinator.complete_commit_and_take_backlog ("transfer-evict-live-B", source, "target-spot");
    return replay && replay->size () == 1 && (*replay)[0].metadata.at ("sequence") == "B_PARKED";
}
}
