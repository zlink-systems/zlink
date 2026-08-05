/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;

std::vector<std::byte> bytes (std::string_view value)
{
    std::vector<std::byte> result;
    result.reserve (value.size ());
    for (const auto character : value)
        result.push_back (static_cast<std::byte> (static_cast<unsigned char> (character)));
    return result;
}

class post_commit_failure_location_store_t final :
    public location_store_t
{
  public:
    task_t<store_read_result_t> read (
      store_key_t key) override
    {
        return inner.read (std::move (key));
    }

    task_t<store_write_result_t> write (
      store_write_request_t request) override
    {
        auto committed = inner.write (std::move (request));
        if (_fail_next_write) {
            _fail_next_write = false;
            committed.result ().value ();
            return task_t<store_write_result_t> (
              result_t<store_write_result_t>::failure (
                framework_error_kind_t::unavailable,
                "reply was lost after commit"));
        }
        return committed;
    }

    task_t<store_scan_result_t> scan (
      store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }

    in_memory_location_store_t inner;

  private:
    bool _fail_next_write = true;
};

class post_commit_failure_relocation_store_t final :
    public relocation_store_t
{
  public:
    task_t<blob_put_result_t> put (
      blob_reference_t reference,
      std::span<const std::byte> payload,
      std::chrono::milliseconds retention) override
    {
        auto committed =
          inner.put (reference, payload, retention);
        if (_fail_next_put) {
            _fail_next_put = false;
            committed.result ().value ();
            return task_t<blob_put_result_t> (
              result_t<blob_put_result_t>::failure (
                framework_error_kind_t::unavailable,
                "reply was lost after commit"));
        }
        return committed;
    }

    task_t<blob_read_result_t> read (
      blob_reference_t reference) override
    {
        return inner.read (std::move (reference));
    }

    task_t<blob_renew_result_t> renew (
      blob_reference_t reference,
      std::chrono::milliseconds retention) override
    {
        return inner.renew (
          std::move (reference), retention);
    }

    task_t<void> erase (
      blob_reference_t reference) override
    {
        return inner.erase (std::move (reference));
    }

    in_memory_relocation_store_t inner;

  private:
    bool _fail_next_put = true;
};

TEST (CppFrameworkOpaqueLocationStore, AtomicWriteUsesExactVersions)
{
    in_memory_location_store_t store;
    const auto first =
      store
        .write ({.conditions = {
                   store_missing_condition_t{
                     {.value = "authority:a"}}},
                 .mutations = {store_put_t{{.value = "authority:a"}, bytes ("one"), std::nullopt},
                               store_put_t{{.value = "capacity:n"}, bytes ("reserved"), 30s}}})
        .result ()
        .value ();
    const auto *applied = std::get_if<store_write_applied_t> (&first);
    ASSERT_NE (applied, nullptr);
    ASSERT_EQ (applied->put_versions.size (), 2u);

    const auto read = store.read ({.value = "authority:a"}).result ().value ();
    const auto *found = std::get_if<store_found_t> (&read);
    ASSERT_NE (found, nullptr);
    EXPECT_EQ (found->value.bytes, bytes ("one"));

    const auto conflict =
      store
        .write ({.conditions = {
                   store_version_condition_t{
                     {.value = "authority:a"},
                     {.value = "stale"}}},
                 .mutations = {store_put_t{{.value = "authority:a"}, bytes ("two"), std::nullopt},
                               store_delete_t{{.value = "capacity:n"}}}})
        .result ()
        .value ();
    EXPECT_TRUE (std::holds_alternative<store_write_conflict_t> (conflict));

    const auto unchanged =
      std::get<store_found_t> (store.read ({.value = "authority:a"}).result ().value ());
    EXPECT_EQ (unchanged.value.bytes, bytes ("one"));
    EXPECT_TRUE (std::holds_alternative<store_found_t> (
      store.read ({.value = "capacity:n"}).result ().value ()));
}

TEST (CppFrameworkOpaqueLocationStore, ScanKeepsTheFirstPageSnapshot)
{
    in_memory_location_store_t store;
    for (const auto *key : {"descriptor:a", "descriptor:b", "other:a"}) {
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          store
            .write (
              {.conditions = {
                 store_missing_condition_t{
                   {.value = key}}},
               .mutations = {store_put_t{{.value = key}, bytes (key), std::nullopt}}})
            .result ()
            .value ()));
    }

    auto first = std::get<store_scan_page_t> (
      store.scan ({.prefix = "descriptor:", .cursor = std::nullopt, .limit = 1})
        .result ()
        .value ());
    ASSERT_EQ (first.items.size (), 1u);
    ASSERT_TRUE (first.next_cursor.has_value ());

    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      store
        .write (
          {.conditions = {
             store_missing_condition_t{
               {.value = "descriptor:c"}}},
           .mutations = {store_put_t{{.value = "descriptor:c"}, bytes ("late"), std::nullopt}}})
        .result ()
        .value ()));

    const auto second = std::get<store_scan_page_t> (
      store.scan ({.prefix = "descriptor:", .cursor = first.next_cursor, .limit = 10})
        .result ()
        .value ());
    ASSERT_EQ (second.items.size (), 1u);
    EXPECT_EQ (second.items.front ().key.value, "descriptor:b");
}

TEST (CppFrameworkOpaqueLocationStore, CursorFromAnotherStoreInstanceExpires)
{
    in_memory_location_store_t first;
    for (const auto *key : {"descriptor:a", "descriptor:b"}) {
        ASSERT_TRUE (
          std::holds_alternative<store_write_applied_t> (
            first
              .write (
                {.conditions = {
                   store_missing_condition_t{
                     {.value = key}}},
                 .mutations = {
                   store_put_t{
                     {.value = key},
                     bytes (key),
                     std::nullopt}}})
              .result ()
              .value ()));
    }
    const auto page = std::get<store_scan_page_t> (
      first
        .scan (
          {.prefix = "descriptor:",
           .cursor = std::nullopt,
           .limit = 1})
        .result ()
        .value ());
    ASSERT_TRUE (page.next_cursor.has_value ());

    in_memory_location_store_t restarted;
    EXPECT_TRUE (
      std::holds_alternative<store_scan_expired_t> (
        restarted
          .scan (
            {.prefix = "descriptor:",
             .cursor = page.next_cursor,
             .limit = 1})
          .result ()
          .value ()));
}

TEST (CppFrameworkOpaqueLocationStore, RepositoryReconcilesLostCommitReply)
{
    post_commit_failure_location_store_t provider;
    provider_location_repository_t repository (provider);

    const auto result =
      repository.claim_owner_lease ("owner-a", 30s)
        .result ()
        .value ();
    ASSERT_TRUE (
      std::holds_alternative<owner_lease_claimed_t> (
        result));

    provider_location_repository_t reopened (
      provider.inner);
    EXPECT_TRUE (
      std::holds_alternative<owner_lease_found_t> (
        reopened.read_owner_lease ("owner-a")
          .result ()
          .value ()));
}

TEST (CppFrameworkOpaqueLocationStore, PrivateRepositoryPersistsLeaseAndDescriptorThroughProvider)
{
    in_memory_location_store_t provider;
    provider_location_repository_t first (provider);
    const auto claim = first.claim_owner_lease ("owner-a", 30s).result ().value ();
    const auto *claimed = std::get_if<owner_lease_claimed_t> (&claim);
    ASSERT_NE (claimed, nullptr);

    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "play";
    descriptor.rid = zlink::routing_id_t::from (std::uint32_t{7});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7001";
    descriptor.owner_id = claimed->token.owner_id;
    descriptor.lease_generation = claimed->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;

    const auto stored =
      first.update_mesh_node (descriptor, location_write_intent_t::new_claim).result ().value ();
    ASSERT_EQ (stored.status, location_write_status_t::stored);

    provider_location_repository_t second (provider);
    const auto lease = second.read_owner_lease ("owner-a").result ().value ();
    ASSERT_TRUE (std::holds_alternative<owner_lease_found_t> (lease));
    const auto page = second.list_mesh_nodes ("play").result ().value ();
    ASSERT_EQ (page.items.size (), 1u);
    EXPECT_EQ (page.items.front ().rid.to_string (), descriptor.rid.to_string ());
    EXPECT_EQ (page.items.front ().owner_id, "owner-a");
}

TEST (CppFrameworkOpaqueLocationStore, PrivateRepositoryPersistsAuthorityLifecycleThroughProvider)
{
    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
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
    EXPECT_EQ (reservation->creating.store_version, "1");
    auto nodes = repository.list_mesh_nodes ("play").result ().value ();
    ASSERT_EQ (nodes.items.size (), 1u);
    EXPECT_EQ (nodes.items.front ().capacity.actors.reserved, 1u);
    EXPECT_EQ (nodes.items.front ().capacity.actors.active, 0u);

    const auto ready =
      repository.commit ({request.key, reservation->fence, bytes ("ready")}).result ().value ();
    const auto *committed = std::get_if<object_committed_t> (&ready);
    ASSERT_NE (committed, nullptr);
    EXPECT_EQ (committed->ready.payload, bytes ("ready"));
    EXPECT_EQ (committed->ready.allocation.state, placement_allocation_state_t::active);
    nodes = repository.list_mesh_nodes ("play").result ().value ();
    ASSERT_EQ (nodes.items.size (), 1u);
    EXPECT_EQ (nodes.items.front ().capacity.actors.reserved, 0u);
    EXPECT_EQ (nodes.items.front ().capacity.actors.active, 1u);

    provider_location_repository_t reopened (provider);
    const auto found = reopened.read_authority ({"1:actor-1"}).result ().value ();
    const auto *snapshot = std::get_if<authority_snapshot_t> (&found);
    ASSERT_NE (snapshot, nullptr);
    EXPECT_EQ (snapshot->payload, bytes ("ready"));

    const auto restored =
      reopened
        .compare_exchange_authority ({"1:actor-1"}, snapshot->store_version,
                                     authority_restore_t{bytes ("restored"), claimed->token})
        .result ()
        .value ();
    const auto *stored = std::get_if<authority_stored_t> (&restored);
    ASSERT_NE (stored, nullptr);
    EXPECT_EQ (stored->snapshot.payload, bytes ("restored"));

    relocation_capacity_reserve_request_t relocation;
    relocation.reservation_id[15] = std::byte{1};
    relocation.key = {"1:actor-1"};
    relocation.expected_store_version = stored->snapshot.store_version;
    relocation.object_kind = placement_object_kind_t::actor;
    relocation.stable_type = "player";
    relocation.source = target;
    relocation.target = target;
    relocation.capacity_bundle.actor_slots = 1;
    const auto relocation_reserved =
      reopened.reserve_relocation_capacity (relocation).result ().value ();
    const auto *relocation_fence =
      std::get_if<relocation_capacity_reserved_t> (&relocation_reserved);
    ASSERT_NE (relocation_fence, nullptr);
    nodes = reopened.list_mesh_nodes ("play").result ().value ();
    EXPECT_EQ (nodes.items.front ().capacity.actors.reserved, 1u);
    EXPECT_EQ (reopened.abort_relocation_capacity (relocation_fence->fence).result ().value (),
               relocation_capacity_abort_result_t::aborted);
    nodes = reopened.list_mesh_nodes ("play").result ().value ();
    EXPECT_EQ (nodes.items.front ().capacity.actors.reserved, 0u);

    relocation.reservation_id[15] = std::byte{2};
    const auto relocation_commit_reservation =
      reopened.reserve_relocation_capacity (relocation).result ().value ();
    const auto *commit_fence =
      std::get_if<relocation_capacity_reserved_t> (&relocation_commit_reservation);
    ASSERT_NE (commit_fence, nullptr);
    const auto moved =
      reopened
        .compare_exchange_authority ({"1:actor-1"}, stored->snapshot.store_version,
                                     authority_put_t{bytes ("moved"),
                                                     authority_generation_transition_t::new_owner,
                                                     claimed->token, commit_fence->fence})
        .result ()
        .value ();
    const auto *moved_authority = std::get_if<authority_stored_t> (&moved);
    ASSERT_NE (moved_authority, nullptr);
    EXPECT_EQ (moved_authority->snapshot.payload, bytes ("moved"));
    EXPECT_EQ (reopened.abort_relocation_capacity (commit_fence->fence).result ().value (),
               relocation_capacity_abort_result_t::already_committed);

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
    aggregate.participants = {{{"1:actor-1"},
                               moved_authority->snapshot.store_version,
                               authority_generation_transition_t::new_owner,
                               bytes ("actor-aggregate"),
                               {}},
                              {{"2:spot-1"},
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
    EXPECT_EQ (reopened.commit_aggregate (aggregate_fence->fence).result ().value (),
               aggregate_commit_result_t::committed);
    const auto aggregated_actor = reopened.read_authority ({"1:actor-1"}).result ().value ();
    ASSERT_TRUE (std::holds_alternative<authority_snapshot_t> (aggregated_actor));
    EXPECT_EQ (std::get<authority_snapshot_t> (aggregated_actor).payload,
               bytes ("actor-aggregate"));

    const auto page = reopened.list_authorities ("1:", std::nullopt, 10).result ().value ();
    const auto *items = std::get_if<authority_page_t> (&page);
    ASSERT_NE (items, nullptr);
    ASSERT_EQ (items->items.size (), 1u);
    EXPECT_EQ (items->items.front ().key.value, "1:actor-1");

    const auto current_actor =
      std::get<authority_snapshot_t> (reopened.read_authority ({"1:actor-1"}).result ().value ());
    const auto current_spot =
      std::get<authority_snapshot_t> (reopened.read_authority ({"2:spot-1"}).result ().value ());
    const auto reserve_move = [&] (std::byte id,
                                   std::string key,
                                   placement_object_kind_t kind,
                                   std::string stable_type,
                                   placement_capacity_bundle_t capacity,
                                   std::string expected_store_version) {
        relocation_capacity_reserve_request_t request;
        request.reservation_id[15] = id;
        request.key = {std::move (key)};
        request.expected_store_version = std::move (expected_store_version);
        request.object_kind = kind;
        request.stable_type = std::move (stable_type);
        request.source = target;
        request.target = target;
        request.capacity_bundle = std::move (capacity);
        const auto result = reopened.reserve_relocation_capacity (request).result ().value ();
        const auto *reserved = std::get_if<relocation_capacity_reserved_t> (&result);
        EXPECT_NE (reserved, nullptr);
        return reserved ? reserved->fence : relocation_capacity_fence_t{};
    };
    placement_capacity_bundle_t actor_capacity;
    actor_capacity.actor_slots = 1;
    const auto actor_fence = reserve_move (std::byte{3}, "1:actor-1",
                                           placement_object_kind_t::actor, "player",
                                           actor_capacity, current_actor.store_version);
    placement_capacity_bundle_t spot_capacity;
    spot_capacity.spot_slots = 1;
    spot_capacity.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    const auto spot_fence = reserve_move (std::byte{4}, "2:spot-1",
                                          placement_object_kind_t::user_spot, "room",
                                          spot_capacity, current_spot.store_version);

    aggregate_prepare_request_t fenced_aggregate;
    fenced_aggregate.aggregate_id.value[15] = std::byte{3};
    fenced_aggregate.aggregate_generation = 1;
    fenced_aggregate.participants = {
      {{"1:actor-1"}, current_actor.store_version,
       authority_generation_transition_t::new_owner, bytes ("fenced-actor"),
       {}, actor_fence},
      {{"2:spot-1"}, current_spot.store_version,
       authority_generation_transition_t::new_owner, bytes ("fenced-spot"),
       {}, spot_fence}};
    fenced_aggregate.inventory_digest = {};
    fenced_aggregate.target_descriptor = {descriptor.mesh_name, descriptor.rid};
    fenced_aggregate.target_descriptor_lifecycle_generation = 1;
    fenced_aggregate.capacity_bundle.actor_slots = 1;
    fenced_aggregate.capacity_bundle.spot_slots = 1;
    fenced_aggregate.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    fenced_aggregate.target_owner = claimed->token;
    fenced_aggregate.capacity_fences = {actor_fence, spot_fence};
    auto unsupported_membership = fenced_aggregate;
    unsupported_membership.aggregate_id.value[14] = std::byte{0x33};
    unsupported_membership.participants.front ().membership_mutation = {
      std::byte{0x01}};
    EXPECT_TRUE (std::holds_alternative<aggregate_prepare_conflict_t> (
      reopened.prepare_aggregate (unsupported_membership).result ().value ()));
    const auto fenced_prepared = reopened.prepare_aggregate (fenced_aggregate).result ().value ();
    const auto *fenced = std::get_if<aggregate_prepared_t> (&fenced_prepared);
    ASSERT_NE (fenced, nullptr);
    nodes = reopened.list_mesh_nodes ("play").result ().value ();
    EXPECT_EQ (nodes.items.front ().capacity.actors.reserved, 1u);
    EXPECT_EQ (nodes.items.front ().capacity.spots.reserved, 1u);
    EXPECT_EQ (reopened.commit_aggregate (fenced->fence).result ().value (),
               aggregate_commit_result_t::committed);
    EXPECT_EQ (reopened.abort_relocation_capacity (actor_fence).result ().value (),
               relocation_capacity_abort_result_t::already_committed);
    EXPECT_EQ (reopened.abort_relocation_capacity (spot_fence).result ().value (),
               relocation_capacity_abort_result_t::already_committed);
    nodes = reopened.list_mesh_nodes ("play").result ().value ();
    EXPECT_EQ (nodes.items.front ().capacity.actors.reserved, 0u);
    EXPECT_EQ (nodes.items.front ().capacity.spots.reserved, 0u);

    const auto abort_actor = std::get<authority_snapshot_t> (
      reopened.read_authority ({"1:actor-1"}).result ().value ());
    const auto abort_spot = std::get<authority_snapshot_t> (
      reopened.read_authority ({"2:spot-1"}).result ().value ());
    const auto abort_actor_fence = reserve_move (std::byte{5}, "1:actor-1",
                                                 placement_object_kind_t::actor, "player",
                                                 actor_capacity, abort_actor.store_version);
    const auto abort_spot_fence = reserve_move (std::byte{6}, "2:spot-1",
                                                placement_object_kind_t::user_spot, "room",
                                                spot_capacity, abort_spot.store_version);
    aggregate_prepare_request_t abort_aggregate = fenced_aggregate;
    abort_aggregate.aggregate_id.value[15] = std::byte{4};
    abort_aggregate.participants[0].expected_store_version = abort_actor.store_version;
    abort_aggregate.participants[0].authority_payload = bytes ("abort-actor");
    abort_aggregate.participants[0].capacity_fence = abort_actor_fence;
    abort_aggregate.participants[1].expected_store_version = abort_spot.store_version;
    abort_aggregate.participants[1].authority_payload = bytes ("abort-spot");
    abort_aggregate.participants[1].capacity_fence = abort_spot_fence;
    abort_aggregate.capacity_fences = {abort_actor_fence, abort_spot_fence};
    const auto abort_prepared = reopened.prepare_aggregate (abort_aggregate).result ().value ();
    const auto *abort_fence = std::get_if<aggregate_prepared_t> (&abort_prepared);
    ASSERT_NE (abort_fence, nullptr);
    EXPECT_EQ (reopened.abort_aggregate (abort_fence->fence).result ().value (),
               aggregate_abort_result_t::aborted);
    EXPECT_EQ (reopened.abort_relocation_capacity (abort_actor_fence).result ().value (),
               relocation_capacity_abort_result_t::already_aborted);
    EXPECT_EQ (reopened.abort_relocation_capacity (abort_spot_fence).result ().value (),
               relocation_capacity_abort_result_t::already_aborted);
    nodes = reopened.list_mesh_nodes ("play").result ().value ();
    EXPECT_EQ (nodes.items.front ().capacity.actors.reserved, 0u);
    EXPECT_EQ (nodes.items.front ().capacity.spots.reserved, 0u);

}

TEST (CppFrameworkOpaqueLocationStore, AggregateCommitUsesBoundedBatches)
{
    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    const auto claim = repository.claim_owner_lease ("owner-large", 30s).result ().value ();
    const auto *claimed = std::get_if<owner_lease_claimed_t> (&claim);
    ASSERT_NE (claimed, nullptr);

    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "large";
    descriptor.rid = zlink::routing_id_t::from (std::string{"node-large"});
    descriptor.lifecycle_generation = 1;
    descriptor.descriptor_revision = 1;
    descriptor.endpoint = "tcp://127.0.0.1:7101";
    descriptor.owner_id = claimed->token.owner_id;
    descriptor.lease_generation = claimed->token.lease_generation;
    descriptor.object_role = object_role_t::server;
    descriptor.state = framework_runtime_state_t::serving;
    descriptor.object_capabilities = {
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0},
      {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::snapshot, true,
       8}};
    descriptor.capacity.actors.limit = 8192;
    descriptor.capacity.spots.limit = 8;
    descriptor.capacity.spot_types.push_back (
      {placement_object_kind_t::user_spot, "room", {0, 0, 8}});
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    const object_creation_target_t target{
      "large", node_rid_t::from_string ("node-large"), 1, claimed->token};
    std::vector<aggregate_participant_t> participants;
    participants.reserve (2050);
    for (std::size_t index = 0; index < 2049; ++index) {
        const auto suffix = std::to_string (100000 + index).substr (1);
        const auto global_id = "actor-" + suffix;
        object_reserve_request_t request;
        request.key = {placement_object_kind_t::actor, global_id};
        request.intent.stable_type = "player";
        request.target = target;
        request.creating_payload = bytes ("creating");
        request.capacity_bundle.actor_slots = 1;
        const auto reserved = repository.reserve (request).result ().value ();
        const auto *fence = std::get_if<object_reserved_t> (&reserved);
        ASSERT_NE (fence, nullptr);
        const auto committed = repository.commit ({request.key, fence->fence, bytes ("ready")})
                                  .result ()
                                  .value ();
        const auto *ready = std::get_if<object_committed_t> (&committed);
        ASSERT_NE (ready, nullptr);
        participants.push_back ({{"1:" + global_id},
                                  ready->ready.store_version,
                                  authority_generation_transition_t::new_owner,
                                  bytes ("aggregate-" + suffix),
                                  {}});
    }

    object_reserve_request_t spot_request;
    spot_request.key = {placement_object_kind_t::user_spot, "spot-large"};
    spot_request.intent.stable_type = "room";
    spot_request.target = target;
    spot_request.creating_payload = bytes ("creating");
    spot_request.capacity_bundle.spot_slots = 1;
    spot_request.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    const auto spot_reserved = repository.reserve (spot_request).result ().value ();
    const auto *spot_fence = std::get_if<object_reserved_t> (&spot_reserved);
    ASSERT_NE (spot_fence, nullptr);
    const auto spot_committed =
      repository.commit ({spot_request.key, spot_fence->fence, bytes ("ready")})
        .result ()
        .value ();
    const auto *spot_ready = std::get_if<object_committed_t> (&spot_committed);
    ASSERT_NE (spot_ready, nullptr);
    participants.push_back ({{"2:spot-large"},
                              spot_ready->ready.store_version,
                              authority_generation_transition_t::new_owner,
                              bytes ("aggregate-spot"),
                              {}});

    aggregate_prepare_request_t aggregate;
    aggregate.aggregate_id.value[0] = std::byte{0x22};
    aggregate.aggregate_generation = 1;
    aggregate.participants = std::move (participants);
    aggregate.target_descriptor = {descriptor.mesh_name, descriptor.rid};
    aggregate.target_descriptor_lifecycle_generation = 1;
    aggregate.capacity_bundle.actor_slots = 2049;
    aggregate.capacity_bundle.spot_slots = 1;
    aggregate.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    aggregate.target_owner = claimed->token;
    const auto prepared = repository.prepare_aggregate (aggregate).result ().value ();
    const auto *prepared_fence = std::get_if<aggregate_prepared_t> (&prepared);
    ASSERT_NE (prepared_fence, nullptr);
    EXPECT_EQ (repository.commit_aggregate (prepared_fence->fence).result ().value (),
               aggregate_commit_result_t::committed);

    for (const auto &key : {std::string{"1:actor-00000"}, std::string{"1:actor-01024"},
                            std::string{"1:actor-02048"}, std::string{"2:spot-large"}}) {
        SCOPED_TRACE (key);
        const auto found = repository.read_authority ({key}).result ().value ();
        const auto *snapshot = std::get_if<authority_snapshot_t> (&found);
        ASSERT_NE (snapshot, nullptr);
        EXPECT_TRUE (snapshot->payload.size () >= 10);
        EXPECT_EQ (snapshot->owner.owner_id, "owner-large");
    }
}

TEST (CppFrameworkOpaqueRelocationStore, CallerIssuedReferenceSupportsExactReconcile)
{
    in_memory_relocation_store_t store;
    const blob_reference_t reference{"relocation:operation-0001:chunk-0000"};
    const auto payload = bytes ("immutable");

    const auto stored = store.put (reference, payload, 30s).result ().value ();
    EXPECT_TRUE (std::holds_alternative<blob_stored_t> (stored));

    const auto replay = store.put (reference, payload, 30s).result ().value ();
    EXPECT_TRUE (std::holds_alternative<blob_already_stored_t> (replay));

    const auto conflict = store.put (reference, bytes ("changed"), 30s).result ().value ();
    EXPECT_TRUE (std::holds_alternative<blob_conflict_t> (conflict));

    const auto exact = store.read (reference).result ().value ();
    const auto *found = std::get_if<blob_found_t> (&exact);
    ASSERT_NE (found, nullptr);
    EXPECT_EQ (found->bytes, payload);

    store.erase (reference).result ().value ();
    EXPECT_TRUE (
      std::holds_alternative<blob_missing_t> (store.read (reference).result ().value ()));
}

TEST (CppFrameworkOpaqueRelocationStore, PrivateRepositoryUsesTheRegisteredOpaqueProvider)
{
    in_memory_relocation_store_t provider;
    provider_relocation_repository_t repository (provider);
    const auto payload = bytes ("repository-payload");

    const auto stored = repository.put_relocation (payload, 1h).result ().value ();
    EXPECT_FALSE (stored.reference.empty ());
    EXPECT_GT (stored.expires_at, stored.store_now);

    const auto provider_read =
      provider.read (blob_reference_t{stored.reference}).result ().value ();
    const auto *provider_found = std::get_if<blob_found_t> (&provider_read);
    ASSERT_NE (provider_found, nullptr);
    EXPECT_EQ (provider_found->bytes, payload);

    const auto repository_read = repository.get_relocation (stored.reference).result ().value ();
    const auto *repository_found = std::get_if<relocation_found_t> (&repository_read);
    ASSERT_NE (repository_found, nullptr);
    EXPECT_EQ (repository_found->payload, payload);

    const auto renewed = repository.renew_relocation (stored.reference, 2h).result ().value ();
    EXPECT_TRUE (std::holds_alternative<relocation_renewed_t> (renewed));

    EXPECT_EQ (repository.delete_relocation (stored.reference).result ().value (),
               relocation_delete_result_t::deleted);
    EXPECT_TRUE (std::holds_alternative<relocation_missing_t> (
      repository.get_relocation (stored.reference).result ().value ()));
}

TEST (CppFrameworkOpaqueRelocationStore, RepositoryReconcilesLostCommitReply)
{
    post_commit_failure_relocation_store_t provider;
    provider_relocation_repository_t repository (provider);
    const auto payload = bytes ("immutable-after-timeout");

    const auto stored =
      repository.put_relocation (payload, 1h)
        .result ()
        .value ();
    const auto read =
      provider.inner
        .read (blob_reference_t{stored.reference})
        .result ()
        .value ();
    const auto *found = std::get_if<blob_found_t> (&read);
    ASSERT_NE (found, nullptr);
    EXPECT_EQ (found->bytes, payload);
}

} // namespace
