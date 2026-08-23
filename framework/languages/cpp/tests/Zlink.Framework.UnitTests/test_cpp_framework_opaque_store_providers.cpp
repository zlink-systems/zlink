/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <limits>
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

std::string segment (std::string_view value)
{
    return std::to_string (value.size ()) + ":" + std::string (value) + ":";
}

store_key_t capacity_key (const mesh_node_descriptor_t &descriptor)
{
    return {"zlink:v11:capacity:" + segment (descriptor.mesh_name)
            + segment (descriptor.rid.to_hex ())
            + std::to_string (descriptor.lifecycle_generation)};
}

nlohmann::json capacity_record (location_store_t &provider,
                                const mesh_node_descriptor_t &descriptor)
{
    const auto row = provider.read (capacity_key (descriptor)).result ().value ();
    const auto *found = std::get_if<store_found_t> (&row);
    EXPECT_NE (found, nullptr);
    return found ? nlohmann::json::parse (
                     reinterpret_cast<const char *> (found->value.bytes.data ()),
                     reinterpret_cast<const char *> (found->value.bytes.data ())
                       + found->value.bytes.size ())
                 : nlohmann::json{};
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

class reject_next_authority_capacity_write_store_t final :
    public location_store_t
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
                const auto &key = std::visit ([] (const auto &value) -> const store_key_t & {
                    return value.key;
                }, mutation);
                if (key.value.starts_with (std::string ("authority") + '\0'))
                    ++authority_mutations;
                if (key.value.starts_with ("zlink:v11:capacity:"))
                    ++capacity_mutations;
            }
            if (authority_mutations != 0 && capacity_mutations != 0) {
                reject_next = false;
                rejected_atomic_batch = true;
                rejected_capacity_mutations = capacity_mutations;
                return task_t<store_write_result_t> (
                  result_t<store_write_result_t>::success (
                    store_write_result_t{store_write_conflict_t{
                      std::chrono::system_clock::now ()}}));
            }
        }
        return inner.write (std::move (request));
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
    // Missing canonical counter rows bootstrap at issue 1 and are stored as
    // bare next-to-issue decimals in the same reserve batch.
    EXPECT_EQ (std::get<store_found_t> (
                 provider.read ({"zlink:v11:object-counter"}).result ().value ())
                 .value.bytes,
               bytes ("2"));
    EXPECT_EQ (std::get<store_found_t> (
                 provider.read ({"zlink:v11:authority-owner-counter"}).result ().value ())
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
    EXPECT_EQ (capacity.at ("actorsPending"), 1);
    EXPECT_EQ (capacity.at ("actorsActive"), 0);
    EXPECT_EQ (nodes.items.front ().capacity.actors.reserved, 0u);
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
    EXPECT_EQ (capacity.at ("actorsPending"), 0);
    EXPECT_EQ (capacity.at ("actorsActive"), 1);

    provider_location_repository_t reopened (provider);
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

    const auto moved =
      reopened
        .compare_exchange_authority (actor_key, stored->snapshot.store_version,
                                     authority_retarget_t{bytes ("moved"), target})
        .result ()
        .value ();
    const auto *moved_authority = std::get_if<authority_stored_t> (&moved);
    ASSERT_NE (moved_authority, nullptr);
    EXPECT_EQ (moved_authority->snapshot.payload, bytes ("moved"));
    capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("actorsPending"), 0);
    EXPECT_EQ (capacity.at ("actorsActive"), 1);

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
    EXPECT_EQ (reopened.commit_aggregate (aggregate_fence->fence).result ().value (),
               aggregate_commit_result_t::committed);
    // The two-participant aggregate issues 4 and 5 after reserve/retarget/
    // reserve, then stores the next-to-issue value 6 in one transition batch.
    EXPECT_EQ (std::get<store_found_t> (
                 provider.read ({"zlink:v11:authority-owner-counter"}).result ().value ())
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
    fenced_aggregate.participants = {
      {actor_key, current_actor.store_version,
       authority_generation_transition_t::new_owner, bytes ("fenced-actor"),
       {}},
      {spot_key, current_spot.store_version,
       authority_generation_transition_t::new_owner, bytes ("fenced-spot"),
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
    unsupported_membership.participants.front ().membership_mutation = {
      std::byte{0x01}};
    EXPECT_TRUE (std::holds_alternative<aggregate_prepare_conflict_t> (
      reopened.prepare_aggregate (unsupported_membership).result ().value ()));
    const auto fenced_prepared = reopened.prepare_aggregate (fenced_aggregate).result ().value ();
    const auto *fenced = std::get_if<aggregate_prepared_t> (&fenced_prepared);
    ASSERT_NE (fenced, nullptr);
    capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("actorsPending"), 1);
    EXPECT_EQ (capacity.at ("spotsPending"), 1);
    EXPECT_EQ (reopened.commit_aggregate (fenced->fence).result ().value (),
               aggregate_commit_result_t::committed);
    capacity = capacity_record (provider, descriptor);
    EXPECT_EQ (capacity.at ("actorsPending"), 0);
    EXPECT_EQ (capacity.at ("spotsPending"), 0);

    const auto abort_actor = std::get<authority_snapshot_t> (
      reopened.read_authority (actor_key).result ().value ());
    const auto abort_spot = std::get<authority_snapshot_t> (
      reopened.read_authority (spot_key).result ().value ());
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
    EXPECT_EQ (capacity.at ("actorsPending"), 0);
    EXPECT_EQ (capacity.at ("spotsPending"), 0);

    // A committed creation keeps its reservation record until the authority
    // is deleted. Deletion must derive that record from the encoded authority
    // key so the same global actor ID can be created again.
    const auto deleted_actor = reopened.read_authority (actor_key).result ().value ();
    const auto *deleted_snapshot = std::get_if<authority_snapshot_t> (&deleted_actor);
    ASSERT_NE (deleted_snapshot, nullptr);
    const auto deleted = reopened
                           .compare_exchange_authority (
                             actor_key, deleted_snapshot->store_version, authority_delete_t{})
                           .result ()
                           .value ();
    ASSERT_TRUE (std::holds_alternative<authority_deleted_t> (deleted));
    const auto recreated = reopened.reserve (request).result ().value ();
    const auto *recreated_reservation = std::get_if<object_reserved_t> (&recreated);
    ASSERT_NE (recreated_reservation, nullptr);
    const auto recreated_commit = reopened
                                    .commit ({request.key, recreated_reservation->fence,
                                              bytes ("recreated")})
                                    .result ()
                                    .value ();
    ASSERT_TRUE (std::holds_alternative<object_committed_t> (recreated_commit));

    // A stored INT64_MAX is exhausted before an aggregate record transition;
    // neither its counter bytes nor the gated authority record may change.
    const auto exhausted_actor = std::get<authority_snapshot_t> (
      reopened.read_authority (actor_key).result ().value ());
    const auto exhausted_spot = std::get<authority_snapshot_t> (
      reopened.read_authority (spot_key).result ().value ());
    auto exhausted_aggregate = abort_aggregate;
    exhausted_aggregate.aggregate_id.value[15] = std::byte{5};
    exhausted_aggregate.participants[0].expected_store_version = exhausted_actor.store_version;
    exhausted_aggregate.participants[1].expected_store_version = exhausted_spot.store_version;
    const auto exhausted_prepared =
      reopened.prepare_aggregate (exhausted_aggregate).result ().value ();
    const auto *exhausted_fence = std::get_if<aggregate_prepared_t> (&exhausted_prepared);
    ASSERT_NE (exhausted_fence, nullptr);
    const store_key_t authority_counter_key{"zlink:v11:authority-owner-counter"};
    const auto counter_before = std::get<store_found_t> (
      provider.read (authority_counter_key).result ().value ());
    const auto maximum = std::to_string (std::numeric_limits<std::int64_t>::max ());
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider.write ({.conditions = {store_version_condition_t{authority_counter_key,
                                                                 counter_before.value.version}},
                       .mutations = {store_put_t{authority_counter_key, bytes (maximum),
                                                  std::nullopt}}})
        .result ()
        .value ()));
    EXPECT_EQ (reopened.commit_aggregate (exhausted_fence->fence).result ().value (),
               aggregate_commit_result_t::generation_exhausted);
    EXPECT_EQ (std::get<store_found_t> (provider.read (authority_counter_key).result ().value ())
                 .value.bytes,
               bytes (maximum));
    EXPECT_EQ (std::get<authority_snapshot_t> (
                 reopened.read_authority (actor_key).result ().value ())
                 .store_version,
               exhausted_actor.store_version);

}

TEST (CppFrameworkOpaqueLocationStore, RetargetUsesCapacityRowsAtomically)
{
    reject_next_authority_capacity_write_store_t provider;
    provider_location_repository_t repository (provider);
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
        value.object_capabilities.push_back (
          {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0});
        value.capacity.actors.limit = 10;
        return value;
    };
    auto source_descriptor = descriptor ("source-node", source_owner->token);
    auto target_descriptor = descriptor ("target-node", target_owner->token);
    ASSERT_EQ (repository.update_mesh_node (source_descriptor, location_write_intent_t::new_claim)
                 .result ().value ().status,
               location_write_status_t::stored);
    ASSERT_EQ (repository.update_mesh_node (target_descriptor, location_write_intent_t::new_claim)
                 .result ().value ().status,
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
        const auto committed = repository
                                 .commit ({request.key, reservation->fence, bytes ("ready")})
                                 .result ().value ();
        const auto *ready = std::get_if<object_committed_t> (&committed);
        EXPECT_NE (ready, nullptr);
        return ready ? ready->ready : authority_snapshot_t{};
    };

    auto moved_actor = create_actor ("actor-moved");
    const auto moved = repository
                         .compare_exchange_authority (
                           actor_authority_key ("actor-moved"), moved_actor.store_version,
                           authority_retarget_t{bytes ("moved"), target_target})
                         .result ().value ();
    const auto *moved_snapshot = std::get_if<authority_stored_t> (&moved);
    ASSERT_NE (moved_snapshot, nullptr);
    EXPECT_EQ (moved_snapshot->snapshot.allocation.target.node_rid.value (), "target-node");
    auto source_capacity = capacity_record (provider, source_descriptor);
    auto target_capacity = capacity_record (provider, target_descriptor);
    EXPECT_EQ (source_capacity.at ("actorsActive"), 0);
    EXPECT_EQ (target_capacity.at ("actorsActive"), 1);

    auto atomic_actor = create_actor ("actor-atomic");
    const store_key_t node_source_capacity_key{
      "zlink:v11:capacity:retarget:source-node"};
    const auto canonical_source_row = std::get<store_found_t> (
      provider.inner.read (capacity_key (source_descriptor)).result ().value ());
    const auto node_source_capacity = nlohmann::json{
      {"active", {{"actors", 1}, {"spots", 0}, {"spotTypes", nlohmann::json::object ()}}},
      {"pending", {{"actors", 0}, {"spots", 0}, {"spotTypes", nlohmann::json::object ()}}}};
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider.inner
        .write ({.conditions = {store_version_condition_t{
                                  capacity_key (source_descriptor),
                                  canonical_source_row.value.version},
                                store_missing_condition_t{node_source_capacity_key}},
                 .mutations = {store_delete_t{capacity_key (source_descriptor)},
                               store_put_t{node_source_capacity_key,
                                           bytes (node_source_capacity.dump ()), std::nullopt}}})
        .result ().value ()));
    provider.reject_next = true;
    const auto rejected = repository
                            .compare_exchange_authority (
                              actor_authority_key ("actor-atomic"), atomic_actor.store_version,
                              authority_retarget_t{bytes ("must-not-commit"), target_target})
                            .result ().value ();
    ASSERT_TRUE (std::holds_alternative<authority_conflict_t> (rejected));
    EXPECT_TRUE (provider.rejected_atomic_batch);
    EXPECT_EQ (provider.rejected_capacity_mutations, 2u);
    const auto after_rejected = std::get<authority_snapshot_t> (
      repository.read_authority (actor_authority_key ("actor-atomic")).result ().value ());
    EXPECT_EQ (after_rejected.store_version, atomic_actor.store_version);
    EXPECT_EQ (after_rejected.allocation.target.node_rid.value (), "source-node");
    auto node_source_row = std::get<store_found_t> (
      provider.inner.read (node_source_capacity_key).result ().value ());
    auto node_source = nlohmann::json::parse (
      reinterpret_cast<const char *> (node_source_row.value.bytes.data ()),
      reinterpret_cast<const char *> (node_source_row.value.bytes.data ())
        + node_source_row.value.bytes.size ());
    target_capacity = capacity_record (provider, target_descriptor);
    EXPECT_EQ (node_source.at ("active").at ("actors"), 1);
    EXPECT_EQ (target_capacity.at ("actorsActive"), 1);

    node_source["active"]["actors"] = 0;
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
      provider.inner
        .write ({.conditions = {store_version_condition_t{
                   node_source_capacity_key, node_source_row.value.version}},
                 .mutations = {store_put_t{node_source_capacity_key,
                                            bytes (node_source.dump ()), std::nullopt}}})
        .result ().value ()));
    const auto underflow = repository
                             .compare_exchange_authority (
                               actor_authority_key ("actor-atomic"), atomic_actor.store_version,
                               authority_retarget_t{bytes ("underflow"), target_target})
                             .result ().value ();
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (underflow));
    node_source_row = std::get<store_found_t> (
      provider.inner.read (node_source_capacity_key).result ().value ());
    node_source = nlohmann::json::parse (
      reinterpret_cast<const char *> (node_source_row.value.bytes.data ()),
      reinterpret_cast<const char *> (node_source_row.value.bytes.data ())
        + node_source_row.value.bytes.size ());
    target_capacity = capacity_record (provider, target_descriptor);
    EXPECT_EQ (node_source.at ("active").at ("actors"), 0);
    EXPECT_EQ (target_capacity.at ("actorsActive"), 1);
}

TEST (CppFrameworkOpaqueLocationStore, AggregateCommitUsesBoundedBatches)
{
    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    // This bounded-batch stress test commits 2050 participants through the
    // in-memory fixture store, which takes tens of seconds; the owner-lease TTL
    // must outlast the whole prepare/commit flow so the terminal CAS still finds
    // the lease valid.
    const auto claim = repository.claim_owner_lease ("owner-large", 300s).result ().value ();
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
        participants.push_back ({actor_authority_key (global_id),
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
    const auto large_spot_key = spot_authority_key ("spot-large");
    participants.push_back ({large_spot_key,
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

    const auto large_actor_0 = actor_authority_key ("actor-00000").value;
    const auto large_actor_1 = actor_authority_key ("actor-01024").value;
    const auto large_actor_2 = actor_authority_key ("actor-02048").value;
    for (const auto &key : {large_actor_0, large_actor_1, large_actor_2,
                            large_spot_key.value}) {
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

// Rewrites every stored canonical JSON row so its `recordVersion` field is
// ABSENT (not merely unknown), returning how many rows were stripped.
// Provider-private rows (counters, reservations, ...) either are not JSON
// objects or carry no recordVersion and pass through untouched.
std::size_t strip_record_version_fields (in_memory_location_store_t &store)
{
    std::size_t stripped = 0;
    std::optional<store_scan_cursor_t> cursor;
    do {
        auto result = store.scan ({"", cursor, 256}).result ().value ();
        auto *page = std::get_if<store_scan_page_t> (&result);
        if (page == nullptr)
            break;
        for (const auto &item : page->items) {
            const std::string text (
              reinterpret_cast<const char *> (item.value.bytes.data ()),
              item.value.bytes.size ());
            auto record = nlohmann::json::parse (text, nullptr, false);
            if (!record.is_object () || !record.contains ("recordVersion"))
                continue;
            record.erase ("recordVersion");
            (void) store
              .write ({.conditions = {},
                       .mutations = {store_put_t{item.key, bytes (record.dump ()),
                                                 std::chrono::hours (1)}}})
              .result ()
              .value ();
            ++stripped;
        }
        cursor = page->next_cursor;
    } while (cursor);
    return stripped;
}

// Spec 21 §2.4 fail-closed: a canonical row whose recordVersion is MISSING
// is just as unrecognized as one with a wrong value — the reader must fail
// explicitly instead of guessing how to read it (java is strict; dotnet
// went strict in f2dfa809e8; this pins the cpp readers: owner lease,
// descriptor envelope, and authority).
TEST (CppFrameworkOpaqueLocationStore, MissingRecordVersionFailsClosed)
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
    descriptor.capacity.actors.limit = 10;
    descriptor.capacity.spots.limit = 10;
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    object_creation_target_t target{"play", node_rid_t::from_string ("node-7"), 1,
                                    claimed->token};
    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "actor-1"};
    request.intent.stable_type = "player";
    request.target = target;
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;
    const auto reserved = repository.reserve (request).result ().value ();
    const auto *reservation = std::get_if<object_reserved_t> (&reserved);
    ASSERT_NE (reservation, nullptr);
    const auto ready =
      repository.commit ({request.key, reservation->fence, bytes ("ready")}).result ().value ();
    ASSERT_NE (std::get_if<object_committed_t> (&ready), nullptr);

    // Sanity: intact rows read fine before the corruption.
    const auto actor_key = actor_authority_key ("actor-1");
    ASSERT_TRUE (std::holds_alternative<owner_lease_found_t> (
      repository.read_owner_lease ("owner-a").result ().value ()));
    ASSERT_TRUE (std::holds_alternative<authority_snapshot_t> (
      repository.read_authority (actor_key).result ().value ()));
    ASSERT_EQ (repository.list_mesh_nodes ("play").result ().value ().items.size (), 1u);

    // Owner lease + MeshNode descriptor + authority at minimum.
    ASSERT_GE (strip_record_version_fields (provider), 3u);

    provider_location_repository_t reopened (provider);
    EXPECT_THROW ((void) reopened.read_owner_lease ("owner-a"), std::invalid_argument);
    EXPECT_THROW ((void) reopened.read_authority (actor_key), std::invalid_argument);
    EXPECT_THROW ((void) reopened.list_mesh_nodes ("play"), std::invalid_argument);
}

} // namespace
