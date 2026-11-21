/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include <fstream>
#include <condition_variable>
#include <zlink/framework/contracts/configuration/app.hpp>

#include <gtest/gtest.h>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../support/opaque_store_creation_fixture.hpp"
#include "../support/opaque_store_counter_conflict_fixture.hpp"

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace zlink::framework::test::opaque_store;

const store_key_t object_generation_counter_key{"zlink:v11:object-counter"};
const store_key_t authority_owner_generation_counter_key{"zlink:v11:authority-owner-counter"};

TEST (ZLinkFrameworkOpaqueStoreProviders, DescriptorCapacityUsesCurrentStoreCounts)
{
    in_memory_location_store_t store;
    provider_location_repository_t writer (store), reader (store);
    writer.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto owner =
      std::get<owner_lease_claimed_t> (
        writer.claim_owner_lease ("capacity-projection-owner", 60s).result ().value ())
        .token;
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "capacity-projection";
    descriptor.rid = zlink::routing_id_t::from ("capacity-node");
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.security_identity = "capacity-node";
    descriptor.entry_spot_id = "capacity-entry";
    descriptor.object_capabilities = {
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::disabled, false, 0,
       true},
      {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::disabled, false, 2},
      {placement_object_kind_t::instance_spot, "unused", maintenance_policy_kind_t::disabled, false,
       5}};
    // Published usage is a stale copy; a missing capacity record means zero.
    descriptor.capacity.actors = {1, 1, 3};
    descriptor.capacity.spots = {1, 1, 3};
    descriptor.capacity.spot_types = {
      {placement_object_kind_t::user_spot, "room", {1, 0, 2}},
      {placement_object_kind_t::instance_spot, "unused", {1, 0, 5}}};
    ASSERT_EQ (writer.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    const auto observe = [&] (std::uint64_t actors_active, std::uint64_t actors_reserved,
                              std::uint64_t spots_active, std::uint64_t spots_reserved) {
        const auto page = reader.list_mesh_nodes (descriptor.mesh_name, {1, {}}).result ();
        ASSERT_TRUE (page);
        ASSERT_EQ (page.value ().items.size (), 1u);
        const auto &current = page.value ().items.front ();
        EXPECT_EQ (current.rid, descriptor.rid);
        EXPECT_EQ (current.lifecycle_generation, descriptor.lifecycle_generation);
        EXPECT_EQ (current.descriptor_revision, descriptor.descriptor_revision);
        EXPECT_EQ (current.owner_id, descriptor.owner_id);
        EXPECT_EQ (current.capacity.actors.limit, 3);
        EXPECT_EQ (current.capacity.spots.limit, 3);
        EXPECT_EQ (current.capacity.actors.active, actors_active);
        EXPECT_EQ (current.capacity.actors.reserved, actors_reserved);
        EXPECT_EQ (current.capacity.spots.active, spots_active);
        EXPECT_EQ (current.capacity.spots.reserved, spots_reserved);
        ASSERT_EQ (current.capacity.spot_types.size (), 2u);
        const auto &room = current.capacity.spot_types[0];
        EXPECT_EQ (room.usage.limit, 2);
        EXPECT_EQ (room.usage.active, spots_active);
        EXPECT_EQ (room.usage.reserved, spots_reserved);
        const auto &unused = current.capacity.spot_types[1];
        EXPECT_EQ (unused.usage.limit, 5);
        EXPECT_EQ (unused.usage.active, 0u);
        EXPECT_EQ (unused.usage.reserved, 0u);
    };
    observe (0, 0, 0, 0);
    object_reserve_request_t actor;
    actor.key = {placement_object_kind_t::actor, "capacity-actor"};
    actor.intent.stable_type = "player";
    actor.target = {descriptor.mesh_name, node_rid_t::from_string (descriptor.rid.to_string ()), 1,
                    owner};
    actor.capacity_bundle.actor_slots = 1;
    object_reserve_request_t spot;
    spot.key = {placement_object_kind_t::user_spot, "capacity-spot"};
    spot.intent.stable_type = "room";
    spot.target = actor.target;
    spot.capacity_bundle = {
      0, 1, spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1}};
    const auto actor_reserved =
      std::get<object_reserved_t> (writer.reserve (actor).result ().value ());
    const auto spot_reserved =
      std::get<object_reserved_t> (writer.reserve (spot).result ().value ());
    observe (0, 1, 0, 1);
    const auto actor_ready = encode_actor_authority_payload (actor_authority_payload_t{
      .state = actor_authority_state_t::ready,
      .stable_type = "player",
      .actor_id = actor.key.global_id,
      .current_spot_id = *descriptor.entry_spot_id,
      .current_spot_generation = 1,
      .current_spot_kind = actor_authority_spot_kind_t::entry,
      .owner_id = owner.owner_id,
      .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
      .mesh_name = descriptor.mesh_name,
      .node_rid = actor.target.node_rid,
      .node_generation = 1});
    ASSERT_TRUE (std::holds_alternative<object_committed_t> (
      writer.commit ({actor.key, actor_reserved.fence, actor_ready}).result ().value ()));
    const auto spot_ready = encode_user_spot_authority_payload (user_spot_authority_payload_t{
      .state = user_spot_authority_state_t::ready,
      .stable_type = "room",
      .spot_id = spot.key.global_id,
      .owner_id = owner.owner_id,
      .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
      .mesh_name = descriptor.mesh_name,
      .node_rid = spot.target.node_rid,
      .node_generation = 1});
    ASSERT_TRUE (std::holds_alternative<object_committed_t> (
      writer.commit ({spot.key, spot_reserved.fence, spot_ready}).result ().value ()));
    observe (1, 0, 1, 0);
    for (const auto &key :
         {actor_authority_key (actor.key.global_id), spot_authority_key (spot.key.global_id)}) {
        const auto current =
          std::get<authority_snapshot_t> (writer.read_authority (key).result ().value ());
        ASSERT_TRUE (std::holds_alternative<authority_deleted_t> (
          writer.compare_exchange_authority (key, current.store_version, authority_delete_t{})
            .result ()
            .value ()));
    }
    observe (0, 0, 0, 0);
    auto second = descriptor;
    second.rid = zlink::routing_id_t::from ("capacity-z");
    second.entry_spot_id = "capacity-entry-z";
    ASSERT_EQ (writer.update_mesh_node (second, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    const auto first_page = reader.list_mesh_nodes (descriptor.mesh_name, {1, {}}).result ();
    ASSERT_TRUE (first_page);
    ASSERT_TRUE (first_page.value ().continuation_token.has_value ());
    const auto second_page =
      reader.list_mesh_nodes (descriptor.mesh_name, {1, first_page.value ().continuation_token})
        .result ();
    ASSERT_TRUE (second_page);
    ASSERT_EQ (second_page.value ().items.size (), 1u);
    EXPECT_EQ (second_page.value ().items.front ().rid, second.rid);
    EXPECT_FALSE (second_page.value ().continuation_token.has_value ());
    EXPECT_EQ (second_page.value ().items.front ().capacity.spots.active, 0u);
    EXPECT_EQ (second_page.value ().items.front ().capacity.spots.limit, 3);
}

class reject_next_authority_capacity_write_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }

    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        if (reject_next) {
            std::size_t authority_mutations = 0;
            std::size_t capacity_mutations = 0;
            for (const auto &mutation : request.mutations) {
                const auto &key = std::visit (
                  [] (const auto &value) -> const store_key_t & { return value.key; }, mutation);
                if (key.value.starts_with (std::string ("authority") + '\0'))
                    ++authority_mutations;
                if (key.value.starts_with ("zlink:v11:capacity:"))
                    ++capacity_mutations;
            }
            if (authority_mutations != 0 && capacity_mutations != 0) {
                reject_next = false;
                rejected_atomic_batch = true;
                rejected_capacity_mutations = capacity_mutations;
                const auto authority_key = std::visit (
                  [] (const auto &mutation) { return mutation.key; }, request.mutations.front ());
                // Awaited: the repository may resume on the inner store's lane.
                const auto row = std::get<store_found_t> (co_await inner.read (authority_key));
                store_write_request_t concurrent{
                  {store_version_condition_t{authority_key, row.value.version}},
                  {store_put_t{authority_key, row.value.bytes, std::nullopt}}};
                (void) co_await inner.write (std::move (concurrent));
                co_return store_write_result_t{
                  store_write_conflict_t{std::chrono::system_clock::now ()}};
            }
        }
        co_return co_await inner.write (std::move (request));
    }

    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }

    in_memory_location_store_t inner;
    bool reject_next = false;
    bool rejected_atomic_batch = false;
    std::size_t rejected_capacity_mutations = 0;
};

TEST (CppFrameworkOpaqueLocationStore, AbortedReservationCanBeReservedAgainThroughProvider)
{
    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto claim = repository.claim_owner_lease ("owner-retry", 30s).result ().value ();
    const auto *claimed = std::get_if<owner_lease_claimed_t> (&claim);
    ASSERT_NE (claimed, nullptr);

    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "retry";
    descriptor.rid = zlink::routing_id_t::from (std::string{"node-retry"});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = claimed->token.owner_id;
    descriptor.lease_generation = claimed->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0});
    descriptor.capacity.actors.limit = 1;
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "actor-retry"};
    request.intent.stable_type = "player";
    request.target = {"retry", node_rid_t::from_string ("node-retry"), 1, claimed->token};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;

    const auto first = repository.reserve (request).result ().value ();
    const auto *first_reservation = std::get_if<object_reserved_t> (&first);
    ASSERT_NE (first_reservation, nullptr);
    ASSERT_TRUE (std::holds_alternative<object_aborted_t> (
      repository.abort ({request.key, first_reservation->fence}).result ().value ()));

    const auto second = repository.reserve (request).result ().value ();
    const auto *second_reservation = std::get_if<object_reserved_t> (&second);
    ASSERT_NE (second_reservation, nullptr);
    EXPECT_NE (second_reservation->fence.reservation_id, first_reservation->fence.reservation_id);
    const auto capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("pending").at ("actors"), 1);
    EXPECT_EQ (capacity.at ("active").at ("actors"), 0);

    EXPECT_TRUE (std::holds_alternative<object_abort_stale_t> (
      repository.abort ({request.key, first_reservation->fence}).result ().value ()));
    EXPECT_TRUE (std::holds_alternative<object_aborted_t> (
      repository.abort ({request.key, second_reservation->fence}).result ().value ()));
    EXPECT_EQ (capacity_record (provider, descriptor).at ("pending").at ("actors"), 0);
}

TEST (CppFrameworkOpaqueLocationStore, PrivateRepositoryPersistsAuthorityLifecycleThroughProvider)
{
    creation_terminal_failure_store_t provider;
    provider_location_repository_t repository (provider);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto claim = repository.claim_owner_lease ("owner-a", 30s).result ().value ();
    const auto *claimed = std::get_if<owner_lease_claimed_t> (&claim);
    ASSERT_NE (claimed, nullptr);

    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "play";
    descriptor.rid = zlink::routing_id_t::from (std::string{"node-7"});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = claimed->token.owner_id;
    descriptor.lease_generation = claimed->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0});
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::snapshot, true, 10});
    descriptor.capacity.actors.limit = 10;
    descriptor.capacity.spots.limit = 10;
    descriptor.capacity.spot_types.push_back (
      {placement_object_kind_t::user_spot, "room", {0, 0, 10}});
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    object_creation_target_t target{"play", node_rid_t::from_string ("node-7"), 1, claimed->token};
    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "actor-1"};
    request.intent.stable_type = "player";
    request.target = target;
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;
    const auto reserved = repository.reserve (request).result ().value ();
    const auto *reservation = std::get_if<object_reserved_t> (&reserved);
    ASSERT_NE (reservation, nullptr);
    // Missing canonical counter rows bootstrap at issue 1 and are stored as
    // bare next-to-issue decimals in the same reserve batch.
    EXPECT_EQ (
      std::get<store_found_t> (provider.read (object_generation_counter_key).result ().value ())
        .value.bytes,
      bytes ("2"));
    EXPECT_EQ (std::get<store_found_t> (
                 provider.read (authority_owner_generation_counter_key).result ().value ())
                 .value.bytes,
               bytes ("2"));
    // store_version is the provider's own opaque per-key version
    // (checklist C-4d), not a per-record counter reset to "1" -- assert it
    // round-trips to a live re-read instead of pinning a literal that
    // depends on how many other keys this store instance already wrote.
    EXPECT_FALSE (reservation->creating.store_version.empty ());
    EXPECT_EQ (reservation->fence.expected_store_version, reservation->creating.store_version);
    auto nodes = repository.list_mesh_nodes ("play").result ().value ();
    ASSERT_EQ (nodes.items.size (), 1u);
    auto capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("pending").at ("actors"), 1);
    EXPECT_EQ (capacity.at ("active").at ("actors"), 0);
    EXPECT_EQ (nodes.items.front ().capacity.actors.reserved,
               capacity.at ("pending").at ("actors").get<std::uint64_t> ());
    EXPECT_EQ (nodes.items.front ().capacity.actors.active, 0u);

    const auto ready =
      repository.commit ({request.key, reservation->fence, bytes ("ready")}).result ().value ();
    const auto *committed = std::get_if<object_committed_t> (&ready);
    ASSERT_NE (committed, nullptr);
    EXPECT_EQ (committed->ready.payload, bytes ("ready"));
    EXPECT_EQ (committed->ready.allocation.state, placement_allocation_state_t::active);
    nodes = repository.list_mesh_nodes ("play").result ().value ();
    ASSERT_EQ (nodes.items.size (), 1u);
    capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("pending").at ("actors"), 0);
    EXPECT_EQ (capacity.at ("active").at ("actors"), 1);

    EXPECT_EQ (nodes.items.front ().capacity.actors.active,
               capacity.at ("active").at ("actors").get<std::uint64_t> ());
    EXPECT_EQ (nodes.items.front ().capacity.actors.reserved,
               capacity.at ("pending").at ("actors").get<std::uint64_t> ());

    provider_location_repository_t reopened (provider);
    reopened.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto actor_key = actor_authority_key ("actor-1");
    const auto spot_key = spot_authority_key ("spot-1");
    const auto found = reopened.read_authority (actor_key).result ().value ();
    const auto *snapshot = std::get_if<authority_snapshot_t> (&found);
    ASSERT_NE (snapshot, nullptr);
    EXPECT_EQ (snapshot->payload, bytes ("ready"));

    const auto restored =
      reopened
        .compare_exchange_authority (actor_key, snapshot->store_version,
                                     authority_restore_t{bytes ("restored"), claimed->token})
        .result ()
        .value ();
    const auto *stored = std::get_if<authority_stored_t> (&restored);
    ASSERT_NE (stored, nullptr);
    EXPECT_EQ (stored->snapshot.payload, bytes ("restored"));

    const auto moved = reopened
                         .compare_exchange_authority (actor_key, stored->snapshot.store_version,
                                                      authority_retarget_t{bytes ("moved"), target})
                         .result ()
                         .value ();
    const auto *moved_authority = std::get_if<authority_stored_t> (&moved);
    ASSERT_NE (moved_authority, nullptr);
    EXPECT_EQ (moved_authority->snapshot.payload, bytes ("moved"));
    capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("pending").at ("actors"), 0);
    EXPECT_EQ (capacity.at ("active").at ("actors"), 1);

    object_reserve_request_t spot_request;
    spot_request.key = {placement_object_kind_t::user_spot, "spot-1"};
    spot_request.intent.stable_type = "room";
    spot_request.target = target;
    spot_request.creating_payload = bytes ("spot-creating");
    spot_request.capacity_bundle.spot_slots = 1;
    spot_request.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    const auto spot_reserved = reopened.reserve (spot_request).result ().value ();
    const auto *spot_reservation = std::get_if<object_reserved_t> (&spot_reserved);
    ASSERT_NE (spot_reservation, nullptr);
    const auto spot_committed =
      reopened.commit ({spot_request.key, spot_reservation->fence, bytes ("spot-ready")})
        .result ()
        .value ();
    const auto *spot_ready = std::get_if<object_committed_t> (&spot_committed);
    ASSERT_NE (spot_ready, nullptr);

    aggregate_prepare_request_t aggregate;
    aggregate.aggregate_id.value[15] = std::byte{1};
    aggregate.aggregate_generation = 1;
    aggregate.participants = {{actor_key,
                               moved_authority->snapshot.store_version,
                               authority_generation_transition_t::new_owner,
                               bytes ("actor-aggregate"),
                               {}},
                              {spot_key,
                               spot_ready->ready.store_version,
                               authority_generation_transition_t::new_owner,
                               bytes ("spot-aggregate"),
                               {}}};
    aggregate.target_descriptor = {"play", descriptor.rid};
    aggregate.target_descriptor_lifecycle_generation = 1;
    aggregate.capacity_bundle.actor_slots = 1;
    aggregate.capacity_bundle.spot_slots = 1;
    aggregate.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    aggregate.target_owner = claimed->token;
    const auto prepared = reopened.prepare_aggregate (aggregate).result ().value ();
    const auto *aggregate_fence = std::get_if<aggregate_prepared_t> (&prepared);
    ASSERT_NE (aggregate_fence, nullptr);
    provider.conflicting_capacity_key = capacity_key (descriptor);
    provider.fault = creation_terminal_failure_store_t::fault_t::capacity_conflict_once;
    EXPECT_EQ (reopened.commit_aggregate (aggregate_fence->fence).result ().value (),
               aggregate_commit_result_t::committed);
    // The two-participant aggregate issues 4 and 5 after reserve/retarget/
    // reserve, then stores the next-to-issue value 6 in one transition batch.
    EXPECT_EQ (std::get<store_found_t> (
                 provider.read (authority_owner_generation_counter_key).result ().value ())
                 .value.bytes,
               bytes ("6"));
    const auto aggregated_actor = reopened.read_authority (actor_key).result ().value ();
    ASSERT_TRUE (std::holds_alternative<authority_snapshot_t> (aggregated_actor));
    EXPECT_EQ (std::get<authority_snapshot_t> (aggregated_actor).payload,
               bytes ("actor-aggregate"));

    const auto page = reopened.list_authorities ("zla1:a:", std::nullopt, 10).result ().value ();
    const auto *items = std::get_if<authority_page_t> (&page);
    ASSERT_NE (items, nullptr);
    ASSERT_EQ (items->items.size (), 1u);
    EXPECT_EQ (items->items.front ().key.value, actor_key.value);

    const auto current_actor =
      std::get<authority_snapshot_t> (reopened.read_authority (actor_key).result ().value ());
    const auto current_spot =
      std::get<authority_snapshot_t> (reopened.read_authority (spot_key).result ().value ());
    aggregate_prepare_request_t fenced_aggregate;
    fenced_aggregate.aggregate_id.value[15] = std::byte{3};
    fenced_aggregate.aggregate_generation = 1;
    fenced_aggregate.participants = {{actor_key,
                                      current_actor.store_version,
                                      authority_generation_transition_t::new_owner,
                                      bytes ("fenced-actor"),
                                      {}},
                                     {spot_key,
                                      current_spot.store_version,
                                      authority_generation_transition_t::new_owner,
                                      bytes ("fenced-spot"),
                                      {}}};
    fenced_aggregate.inventory_digest = {};
    fenced_aggregate.target_descriptor = {descriptor.mesh_name, descriptor.rid};
    fenced_aggregate.target_descriptor_lifecycle_generation = 1;
    fenced_aggregate.capacity_bundle.actor_slots = 1;
    fenced_aggregate.capacity_bundle.spot_slots = 1;
    fenced_aggregate.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    fenced_aggregate.target_owner = claimed->token;
    auto unsupported_membership = fenced_aggregate;
    unsupported_membership.aggregate_id.value[14] = std::byte{0x33};
    unsupported_membership.participants.front ().membership_mutation = {std::byte{0x01}};
    EXPECT_TRUE (std::holds_alternative<aggregate_prepare_conflict_t> (
      reopened.prepare_aggregate (unsupported_membership).result ().value ()));
    const auto fenced_prepared = reopened.prepare_aggregate (fenced_aggregate).result ().value ();
    const auto *fenced = std::get_if<aggregate_prepared_t> (&fenced_prepared);
    ASSERT_NE (fenced, nullptr);
    capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("pending").at ("actors"), 1);
    EXPECT_EQ (capacity.at ("pending").at ("spots"), 1);
    EXPECT_EQ (reopened.commit_aggregate (fenced->fence).result ().value (),
               aggregate_commit_result_t::committed);
    capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("pending").at ("actors"), 0);
    EXPECT_EQ (capacity.at ("pending").at ("spots"), 0);

    const auto abort_actor =
      std::get<authority_snapshot_t> (reopened.read_authority (actor_key).result ().value ());
    const auto abort_spot =
      std::get<authority_snapshot_t> (reopened.read_authority (spot_key).result ().value ());
    aggregate_prepare_request_t abort_aggregate = fenced_aggregate;
    abort_aggregate.aggregate_id.value[15] = std::byte{4};
    abort_aggregate.participants[0].expected_store_version = abort_actor.store_version;
    abort_aggregate.participants[0].authority_payload = bytes ("abort-actor");
    abort_aggregate.participants[1].expected_store_version = abort_spot.store_version;
    abort_aggregate.participants[1].authority_payload = bytes ("abort-spot");
    const auto abort_prepared = reopened.prepare_aggregate (abort_aggregate).result ().value ();
    const auto *abort_fence = std::get_if<aggregate_prepared_t> (&abort_prepared);
    ASSERT_NE (abort_fence, nullptr);
    EXPECT_EQ (reopened.abort_aggregate (abort_fence->fence).result ().value (),
               aggregate_abort_result_t::aborted);
    capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("pending").at ("actors"), 0);
    EXPECT_EQ (capacity.at ("pending").at ("spots"), 0);

    // A committed creation keeps its reservation record until the authority
    // is deleted. Deletion must derive that record from the encoded authority
    // key so the same global actor ID can be created again.
    const auto deleted_actor = reopened.read_authority (actor_key).result ().value ();
    const auto *deleted_snapshot = std::get_if<authority_snapshot_t> (&deleted_actor);
    ASSERT_NE (deleted_snapshot, nullptr);
    const auto deleted = reopened
                           .compare_exchange_authority (actor_key, deleted_snapshot->store_version,
                                                        authority_delete_t{})
                           .result ()
                           .value ();
    ASSERT_TRUE (std::holds_alternative<authority_deleted_t> (deleted));
    const auto recreated = reopened.reserve (request).result ().value ();
    const auto *recreated_reservation = std::get_if<object_reserved_t> (&recreated);
    ASSERT_NE (recreated_reservation, nullptr);
    const auto recreated_commit =
      reopened.commit ({request.key, recreated_reservation->fence, bytes ("recreated")})
        .result ()
        .value ();
    ASSERT_TRUE (std::holds_alternative<object_committed_t> (recreated_commit));

    // A stored INT64_MAX is exhausted before an aggregate record transition;
    // neither its counter bytes nor the gated authority record may change.
    const auto exhausted_actor =
      std::get<authority_snapshot_t> (reopened.read_authority (actor_key).result ().value ());
    const auto exhausted_spot =
      std::get<authority_snapshot_t> (reopened.read_authority (spot_key).result ().value ());
    auto exhausted_aggregate = abort_aggregate;
    exhausted_aggregate.aggregate_id.value[15] = std::byte{5};
    exhausted_aggregate.participants[0].expected_store_version = exhausted_actor.store_version;
    exhausted_aggregate.participants[1].expected_store_version = exhausted_spot.store_version;
    const auto exhausted_prepared =
      reopened.prepare_aggregate (exhausted_aggregate).result ().value ();
    const auto *exhausted_fence = std::get_if<aggregate_prepared_t> (&exhausted_prepared);
    ASSERT_NE (exhausted_fence, nullptr);
    const auto &authority_counter_key = authority_owner_generation_counter_key;
    const auto counter_before =
      std::get<store_found_t> (provider.read (authority_counter_key).result ().value ());
    const auto maximum = std::to_string (std::numeric_limits<std::int64_t>::max ());
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider
        .write ({.conditions = {store_version_condition_t{authority_counter_key,
                                                          counter_before.value.version}},
                 .mutations = {store_put_t{authority_counter_key, bytes (maximum), std::nullopt}}})
        .result ()
        .value ()));
    EXPECT_EQ (reopened.commit_aggregate (exhausted_fence->fence).result ().value (),
               aggregate_commit_result_t::generation_exhausted);
    EXPECT_EQ (std::get<store_found_t> (provider.read (authority_counter_key).result ().value ())
                 .value.bytes,
               bytes (maximum));
    EXPECT_EQ (
      std::get<authority_snapshot_t> (reopened.read_authority (actor_key).result ().value ())
        .store_version,
      exhausted_actor.store_version);
}

TEST (CppFrameworkOpaqueLocationStore, EveryAuthorityMutationRechecksConflictQualification)
{
    for (int kind = 0; kind != 5; ++kind) {
        for (int loss = 0; loss != 3; ++loss) {
            SCOPED_TRACE (kind);
            SCOPED_TRACE (loss);
            counter_conflict_store_t provider;
            provider_location_repository_t repository (provider);
            repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
            const auto owner =
              std::get<owner_lease_claimed_t> (
                repository.claim_owner_lease ("mutation-owner", 30s).result ().value ())
                .token;
            mesh_node_descriptor_t descriptor;
            descriptor.mesh_name = "mutation-mesh";
            descriptor.rid = zlink::routing_id_t::from (std::string{"mutation-node"});
            descriptor.lifecycle_generation = 1;
            descriptor.descriptor_revision = 1;
            descriptor.endpoint = "tcp://127.0.0.1:7001";
            descriptor.owner_id = owner.owner_id;
            descriptor.lease_generation = owner.lease_generation;
            descriptor.object_role = object_role_t::server;
            descriptor.state = framework_runtime_state_t::serving;
            descriptor.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                                       maintenance_policy_kind_t::recreate, false,
                                                       0});
            descriptor.capacity.actors.limit = 2;
            ASSERT_EQ (location_write_status_t::stored,
                       repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                         .result ()
                         .value ()
                         .status);
            object_reserve_request_t request;
            request.key = {placement_object_kind_t::actor, "mutation-actor"};
            request.intent.stable_type = "player";
            request.target = {descriptor.mesh_name, node_rid_t::from_string ("mutation-node"), 1,
                              owner};
            request.capacity_bundle.actor_slots = 1;
            const auto reserved =
              std::get<object_reserved_t> (repository.reserve (request).result ().value ());
            const auto ready = std::get<object_committed_t> (
                                 repository.commit ({request.key, reserved.fence, bytes ("ready")})
                                   .result ()
                                   .value ())
                                 .ready;
            const auto key = actor_authority_key (request.key.global_id);
            const auto capacity_before = capacity_record (provider.inner, descriptor);
            const std::vector<authority_mutation_t> mutations{
              authority_put_t{bytes ("changed")}, authority_reincarnate_t{bytes ("changed")},
              authority_restore_t{bytes ("changed"), owner},
              authority_retarget_t{bytes ("changed"), request.target}, authority_delete_t{}};
            provider.remaining_conflicts = loss == 0 ? 65 : 1;
            provider.on_conflict = [&] () -> task_t<void> {
                if (loss == 2) {
                    EXPECT_TRUE (std::holds_alternative<owner_lease_released_t> (
                      co_await repository.release_owner_lease (owner)));
                } else if (loss == 1) {
                    authority_mutation_t concurrent = authority_put_t{bytes ("ready")};
                    EXPECT_TRUE (std::holds_alternative<authority_stored_t> (
                      co_await repository.compare_exchange_authority (key, ready.store_version,
                                                                      std::move (concurrent))));
                } else {
                    const auto row_key = object_generation_counter_key;
                    const auto row =
                      std::get<store_found_t> (co_await provider.inner.read (row_key));
                    store_write_request_t concurrent{
                      {}, {store_put_t{row_key, row.value.bytes, std::nullopt}}};
                    EXPECT_TRUE (std::holds_alternative<store_write_applied_t> (
                      co_await provider.inner.write (std::move (concurrent))));
                }
            };
            const auto result =
              repository.compare_exchange_authority (key, ready.store_version, mutations[kind])
                .result ()
                .value ();
            if (loss != 0) {
                EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (result));
                EXPECT_EQ (provider.rejected, 1u);
                const auto current = std::get<authority_snapshot_t> (
                  repository.read_authority (key).result ().value ());
                EXPECT_EQ (current.payload, bytes ("ready"));
                EXPECT_EQ (capacity_before, capacity_record (provider.inner, descriptor));
            } else {
                EXPECT_EQ (provider.rejected, 65u);
                EXPECT_EQ (provider.remaining_conflicts, 0u);
                if (kind == 4) {
                    EXPECT_TRUE (std::holds_alternative<authority_deleted_t> (result));
                    EXPECT_TRUE (std::holds_alternative<authority_missing_t> (
                      repository.read_authority (key).result ().value ()));
                } else {
                    const auto *stored = std::get_if<authority_stored_t> (&result);
                    EXPECT_NE (stored, nullptr);
                    if (!stored)
                        continue;
                    EXPECT_EQ (stored->snapshot.payload, bytes ("changed"));
                    EXPECT_NE (stored->snapshot.store_version, ready.store_version);
                    EXPECT_EQ (capacity_before, capacity_record (provider.inner, descriptor));
                }
            }
        }
    }
}

TEST (CppFrameworkOpaqueLocationStore, ReincarnateAtomicallyIssuesPairAndPreservesCapacity)
{
    counter_conflict_store_t conflict_store;
    auto &provider = conflict_store.inner;
    provider_location_repository_t repository (conflict_store);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto owner = std::get<owner_lease_claimed_t> (
                         repository.claim_owner_lease ("reincarnate-owner", 30s).result ().value ())
                         .token;
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "reincarnate-mesh";
    descriptor.rid = zlink::routing_id_t::from (std::string{"reincarnate-node"});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0});
    descriptor.capacity.actors.limit = 1;
    ASSERT_EQ (location_write_status_t::stored,
               repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status);
    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "reincarnate-actor"};
    request.intent.stable_type = "player";
    request.target = {descriptor.mesh_name, node_rid_t::from_string ("reincarnate-node"), 1, owner};
    request.capacity_bundle.actor_slots = 1;
    const auto reservation =
      std::get<object_reserved_t> (repository.reserve (request).result ().value ());
    const auto key = actor_authority_key (request.key.global_id);
    const auto &object_counter = object_generation_counter_key;
    const auto &owner_counter = authority_owner_generation_counter_key;
    const auto counter = [&] (const store_key_t &counter_key) {
        return std::get<store_found_t> (provider.read (counter_key).result ().value ()).value;
    };
    const auto object_before = counter (object_counter);
    const auto owner_before = counter (owner_counter);
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (
      repository
        .compare_exchange_authority (key, reservation.creating.store_version,
                                     authority_reincarnate_t{bytes ("new")})
        .result ()
        .value ()));
    EXPECT_EQ (object_before.version.value, counter (object_counter).version.value);
    EXPECT_EQ (owner_before.version.value, counter (owner_counter).version.value);
    const auto ready =
      std::get<object_committed_t> (
        repository.commit ({request.key, reservation.fence, bytes ("ready")}).result ().value ())
        .ready;
    const auto capacity_before = capacity_record (provider, descriptor);
    auto result = repository
                    .compare_exchange_authority (key, ready.store_version,
                                                 authority_reincarnate_t{bytes ("new")})
                    .result ()
                    .value ();
    auto *stored = std::get_if<authority_stored_t> (&result);
    ASSERT_NE (nullptr, stored);
    EXPECT_GT (stored->snapshot.object_generation, ready.object_generation);
    EXPECT_GT (stored->snapshot.authority_owner_generation, ready.authority_owner_generation);
    EXPECT_EQ (owner.owner_id, stored->snapshot.owner.owner_id);
    EXPECT_EQ (owner.lease_generation, stored->snapshot.owner.lease_generation);
    EXPECT_EQ (ready.allocation.target.node_rid.value (),
               stored->snapshot.allocation.target.node_rid.value ());
    EXPECT_EQ (capacity_before, capacity_record (provider, descriptor));
    EXPECT_EQ (bytes (std::to_string (stored->snapshot.object_generation + 1)),
               counter (object_counter).bytes);
    EXPECT_EQ (bytes (std::to_string (stored->snapshot.authority_owner_generation + 1)),
               counter (owner_counter).bytes);
    auto over_capacity = request;
    over_capacity.key.global_id = "reincarnate-over-capacity";
    EXPECT_TRUE (std::holds_alternative<object_placement_capacity_exhausted_t> (
      repository.reserve (over_capacity).result ().value ()));
    EXPECT_EQ (capacity_before, capacity_record (provider, descriptor));
    for (const auto &mutation :
         std::vector<authority_mutation_t>{authority_reincarnate_t{}, authority_delete_t{}})
        EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (
          repository.compare_exchange_authority (key, ready.store_version, mutation)
            .result ()
            .value ()));

    for (const auto &conflict_key : {object_counter, owner_counter}) {
        conflict_store.conflict_key = conflict_key;
        result = repository
                   .compare_exchange_authority (key, stored->snapshot.store_version,
                                                authority_reincarnate_t{bytes ("counter-conflict")})
                   .result ()
                   .value ();
        stored = std::get_if<authority_stored_t> (&result);
        ASSERT_NE (stored, nullptr);
        EXPECT_FALSE (conflict_store.conflict_key);
        EXPECT_EQ (bytes ("counter-conflict"), stored->snapshot.payload);
        EXPECT_EQ (bytes (std::to_string (stored->snapshot.object_generation + 1)),
                   counter (object_counter).bytes);
        EXPECT_EQ (bytes (std::to_string (stored->snapshot.authority_owner_generation + 1)),
                   counter (owner_counter).bytes);
        EXPECT_EQ (
          stored->snapshot.store_version,
          std::get<authority_snapshot_t> (repository.read_authority (key).result ().value ())
            .store_version);
        EXPECT_EQ (capacity_before, capacity_record (provider, descriptor));
    }
    const auto object_unexhausted = counter (object_counter);
    const auto owner_unexhausted = counter (owner_counter);
    for (const auto &exhausted_key : {object_counter, owner_counter}) {
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          provider
            .write (
              {{},
               {store_put_t{exhausted_key,
                            bytes (std::to_string (std::numeric_limits<std::int64_t>::max ())),
                            std::nullopt}}})
            .result ()
            .value ()));
        const auto object_exhausted = counter (object_counter);
        const auto owner_exhausted = counter (owner_counter);
        EXPECT_TRUE (std::holds_alternative<authority_generation_exhausted_t> (
          repository
            .compare_exchange_authority (key, stored->snapshot.store_version,
                                         authority_reincarnate_t{bytes ("exhausted")})
            .result ()
            .value ()));
        EXPECT_EQ (object_exhausted.version.value, counter (object_counter).version.value);
        EXPECT_EQ (owner_exhausted.version.value, counter (owner_counter).version.value);
        EXPECT_EQ (
          stored->snapshot.store_version,
          std::get<authority_snapshot_t> (repository.read_authority (key).result ().value ())
            .store_version);
        EXPECT_EQ (capacity_before, capacity_record (provider, descriptor));
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          provider
            .write ({{},
                     {store_put_t{object_counter, object_unexhausted.bytes, std::nullopt},
                      store_put_t{owner_counter, owner_unexhausted.bytes, std::nullopt}}})
            .result ()
            .value ()));
    }
    repository.release_owner_lease (owner).result ().value ();
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (
      repository
        .compare_exchange_authority (key, stored->snapshot.store_version,
                                     authority_reincarnate_t{bytes ("expired")})
        .result ()
        .value ()));
    EXPECT_EQ (object_unexhausted.bytes, counter (object_counter).bytes);
    EXPECT_EQ (owner_unexhausted.bytes, counter (owner_counter).bytes);
    EXPECT_EQ (capacity_before, capacity_record (provider, descriptor));
}

TEST (CppFrameworkOpaqueLocationStore, ReservationLivesOnlyInReservedAuthorityRow)
{
    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto lease = repository.claim_owner_lease ("single-row-owner", 30s).result ().value ();
    const auto *owner = std::get_if<owner_lease_claimed_t> (&lease);
    ASSERT_NE (owner, nullptr);

    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "single-row-mesh";
    descriptor.rid = zlink::routing_id_t::from (std::string{"single-row-node"});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = owner->token.owner_id;
    descriptor.lease_generation = owner->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities.push_back (
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0});
    descriptor.capacity.actors.limit = 1;
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "single-row-actor"};
    request.intent.stable_type = "player";
    request.intent.request_content_reference = "inline-v1:cmVxdWVzdA";
    request.intent.request_sha256 = sha256 (bytes ("request"));
    request.intent.request_encoded_size = 7;
    request.target = {"single-row-mesh", node_rid_t::from_string ("single-row-node"), 1,
                      owner->token};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;

    const auto reserved = repository.reserve (request).result ().value ();
    const auto *reservation = std::get_if<object_reserved_t> (&reserved);
    ASSERT_NE (reservation, nullptr);
    const auto authority =
      repository.read_authority (actor_authority_key (request.key.global_id)).result ().value ();
    const auto *snapshot = std::get_if<authority_snapshot_t> (&authority);
    ASSERT_NE (snapshot, nullptr);
    EXPECT_EQ (snapshot->allocation.state, placement_allocation_state_t::reserved);
    ASSERT_TRUE (snapshot->pending_creation);
    EXPECT_EQ (snapshot->pending_creation->reservation_id, reservation->fence.reservation_id);
    EXPECT_EQ (snapshot->pending_creation->request_content_reference,
               request.intent.request_content_reference);
    EXPECT_EQ (snapshot->pending_creation->request_sha256, request.intent.request_sha256);
    EXPECT_EQ (snapshot->pending_creation->request_encoded_size,
               request.intent.request_encoded_size);

    const auto reservation_rows =
      provider
        .scan ({.prefix = "zlink:v11:creation-reservation:", .cursor = std::nullopt, .limit = 10})
        .result ()
        .value ();
    const auto *reservation_page = std::get_if<store_scan_page_t> (&reservation_rows);
    ASSERT_NE (reservation_page, nullptr);
    EXPECT_TRUE (reservation_page->items.empty ());

    const auto committed =
      repository.commit ({request.key, reservation->fence, bytes ("ready")}).result ().value ();
    const auto *ready = std::get_if<object_committed_t> (&committed);
    ASSERT_NE (ready, nullptr);
    EXPECT_EQ (ready->ready.allocation.state, placement_allocation_state_t::active);
    EXPECT_FALSE (ready->ready.pending_creation);
}

TEST (CppFrameworkOpaqueLocationStore, RetargetUsesCapacityRowsAtomically)
{
    reject_next_authority_capacity_write_store_t provider;
    provider_location_repository_t repository (provider);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto source_claim = repository.claim_owner_lease ("source-owner", 30s).result ().value ();
    const auto target_claim = repository.claim_owner_lease ("target-owner", 30s).result ().value ();
    const auto *source_owner = std::get_if<owner_lease_claimed_t> (&source_claim);
    const auto *target_owner = std::get_if<owner_lease_claimed_t> (&target_claim);
    ASSERT_NE (source_owner, nullptr);
    ASSERT_NE (target_owner, nullptr);

    const auto descriptor = [] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t value;
        value.mesh_name = "retarget";
        value.rid = zlink::routing_id_t::from (std::move (rid));
        value.lifecycle_generation = 1;
        value.descriptor_revision = 1;
        value.endpoint = "tcp://127.0.0.1:7001";
        value.owner_id = owner.owner_id;
        value.lease_generation = owner.lease_generation;
        value.object_role = object_role_t::server;
        value.state = framework_runtime_state_t::serving;
        value.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                              maintenance_policy_kind_t::recreate, false, 0});
        value.capacity.actors.limit = 10;
        return value;
    };
    auto source_descriptor = descriptor ("source-node", source_owner->token);
    auto target_descriptor = descriptor ("target-node", target_owner->token);
    ASSERT_EQ (repository.update_mesh_node (source_descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    ASSERT_EQ (repository.update_mesh_node (target_descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);
    const object_creation_target_t source_target{
      "retarget", node_rid_t::from_string ("source-node"), 1, source_owner->token};
    const object_creation_target_t target_target{
      "retarget", node_rid_t::from_string ("target-node"), 1, target_owner->token};

    const auto create_actor = [&] (std::string id) {
        object_reserve_request_t request;
        request.key = {placement_object_kind_t::actor, std::move (id)};
        request.intent.stable_type = "player";
        request.target = source_target;
        request.creating_payload = bytes ("creating");
        request.capacity_bundle.actor_slots = 1;
        const auto reserved = repository.reserve (request).result ().value ();
        const auto *reservation = std::get_if<object_reserved_t> (&reserved);
        EXPECT_NE (reservation, nullptr);
        if (!reservation)
            return authority_snapshot_t{};
        const auto committed =
          repository.commit ({request.key, reservation->fence, bytes ("ready")}).result ().value ();
        const auto *ready = std::get_if<object_committed_t> (&committed);
        EXPECT_NE (ready, nullptr);
        return ready ? ready->ready : authority_snapshot_t{};
    };

    auto moved_actor = create_actor ("actor-moved");
    const auto moved =
      repository
        .compare_exchange_authority (actor_authority_key ("actor-moved"), moved_actor.store_version,
                                     authority_retarget_t{bytes ("moved"), target_target})
        .result ()
        .value ();
    const auto *moved_snapshot = std::get_if<authority_stored_t> (&moved);
    ASSERT_NE (moved_snapshot, nullptr);
    EXPECT_EQ (moved_snapshot->snapshot.allocation.target.node_rid.value (), "target-node");
    auto source_capacity = capacity_record (provider, source_descriptor);
    auto target_capacity = capacity_record (provider, target_descriptor);
    EXPECT_EQ (source_capacity.at ("active").at ("actors"), 0);
    EXPECT_EQ (target_capacity.at ("active").at ("actors"), 1);

    auto atomic_actor = create_actor ("actor-atomic");
    const store_key_t node_source_capacity_key{"zlink:v11:capacity:retarget:source-node"};
    const auto canonical_source_row = std::get<store_found_t> (
      provider.inner.read (capacity_key (source_descriptor)).result ().value ());
    const auto node_source_capacity = nlohmann::json{
      {"active", {{"actors", 1}, {"spots", 0}, {"spotTypes", nlohmann::json::object ()}}},
      {"pending", {{"actors", 0}, {"spots", 0}, {"spotTypes", nlohmann::json::object ()}}}};
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider.inner
        .write ({.conditions =
                   {
                     store_version_condition_t{capacity_key (source_descriptor),
                                               canonical_source_row.value.version},
                   },
                 .mutations = {store_put_t{node_source_capacity_key,
                                           bytes (node_source_capacity.dump ()), std::nullopt}}})
        .result ()
        .value ()));
    provider.reject_next = true;
    const auto rejected = repository
                            .compare_exchange_authority (
                              actor_authority_key ("actor-atomic"), atomic_actor.store_version,
                              authority_retarget_t{bytes ("must-not-commit"), target_target})
                            .result ()
                            .value ();
    ASSERT_TRUE (std::holds_alternative<authority_conflict_t> (rejected));
    EXPECT_TRUE (provider.rejected_atomic_batch);
    EXPECT_EQ (provider.rejected_capacity_mutations, 2u);
    const auto after_rejected = std::get<authority_snapshot_t> (
      repository.read_authority (actor_authority_key ("actor-atomic")).result ().value ());
    EXPECT_NE (after_rejected.store_version, atomic_actor.store_version);
    EXPECT_EQ (after_rejected.allocation.target.node_rid.value (), "source-node");
    auto node_source_row =
      std::get<store_found_t> (provider.inner.read (node_source_capacity_key).result ().value ());
    auto node_source =
      nlohmann::json::parse (reinterpret_cast<const char *> (node_source_row.value.bytes.data ()),
                             reinterpret_cast<const char *> (node_source_row.value.bytes.data ())
                               + node_source_row.value.bytes.size ());
    target_capacity = capacity_record (provider, target_descriptor);
    EXPECT_EQ (node_source.at ("active").at ("actors"), 1);
    EXPECT_EQ (target_capacity.at ("active").at ("actors"), 1);

    atomic_actor = after_rejected;
    node_source["active"]["actors"] = 0;
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider.inner
        .write ({.conditions = {store_version_condition_t{node_source_capacity_key,
                                                          node_source_row.value.version}},
                 .mutations = {store_put_t{node_source_capacity_key, bytes (node_source.dump ()),
                                           std::nullopt}}})
        .result ()
        .value ()));
    const auto underflow = repository
                             .compare_exchange_authority (
                               actor_authority_key ("actor-atomic"), atomic_actor.store_version,
                               authority_retarget_t{bytes ("underflow"), target_target})
                             .result ()
                             .value ();
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (underflow));
    node_source_row =
      std::get<store_found_t> (provider.inner.read (node_source_capacity_key).result ().value ());
    node_source =
      nlohmann::json::parse (reinterpret_cast<const char *> (node_source_row.value.bytes.data ()),
                             reinterpret_cast<const char *> (node_source_row.value.bytes.data ())
                               + node_source_row.value.bytes.size ());
    target_capacity = capacity_record (provider, target_descriptor);
    EXPECT_EQ (node_source.at ("active").at ("actors"), 0);
    EXPECT_EQ (target_capacity.at ("active").at ("actors"), 1);
}
}
