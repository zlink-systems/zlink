/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "../support/location_reclaim_clock.hpp"
#include <gtest/gtest.h>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace std::chrono_literals;

namespace
{
class observed_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        if (before_write) {
            auto callback = std::exchange (before_write, {});
            co_await callback ();
        }
        writes.push_back (request);
        co_return co_await inner.write (std::move (request));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        if (fail_scan)
            throw framework_exception_t (framework_error_kind_t::unavailable, "membership scan");
        if (expire_scan && request.cursor)
            co_return store_scan_expired_t{};
        request.limit = 1;
        co_return co_await inner.scan (std::move (request));
    }
    in_memory_location_store_t inner;
    std::vector<store_write_request_t> writes;
    std::function<task_t<void> ()> before_write;
    bool fail_scan = false;
    bool expire_scan = false;
};

class DiscardedAuthority : public ::testing::TestWithParam<bool>
{
  protected:
    observed_store_t opaque;
    provider_location_repository_t provider{opaque};
    in_memory_location_repository_t memory;
    location_repository_t *repository = nullptr;
    location_reclaim_test_access_t clock;
    location_owner_token_t owner;
    mesh_node_descriptor_t descriptor;

    void SetUp () override
    {
        repository = GetParam () ? static_cast<location_repository_t *> (&provider) : &memory;
        clock.bind (*repository);
        repository->configure_reclaim_margin (0ms);
        owner = std::get<owner_lease_claimed_t> (
                  repository->claim_owner_lease ("discard-owner", 1h).result ().value ())
                  .token;
        descriptor.mesh_name = "discard-mesh";
        descriptor.rid = zlink::routing_id_t::from ("discard-node");
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.security_identity = "discard-node";
        descriptor.entry_spot_id = "discard-entry";
        descriptor.capacity.actors.limit = 20;
        descriptor.capacity.spots.limit = 20;
        descriptor.capacity.spot_types = {
          {placement_object_kind_t::user_spot, "room", {0, 0, 20}},
          {placement_object_kind_t::instance_spot, "instance", {0, 0, 20}}};
        descriptor.object_capabilities = {
          {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0},
          {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::recreate, false,
           20},
          {placement_object_kind_t::instance_spot, "instance", maintenance_policy_kind_t::recreate,
           false, 20}};
        publish ();
    }

    void publish ()
    {
        EXPECT_EQ (repository->update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
    }

    object_reserve_request_t request (std::string id, bool spot = false)
    {
        object_reserve_request_t r;
        r.key = {spot ? placement_object_kind_t::user_spot : placement_object_kind_t::actor, id};
        r.intent.stable_type = spot ? "room" : "player";
        r.target = {descriptor.mesh_name, node_rid_t::from_string (descriptor.rid.to_string ()), 1,
                    owner};
        r.capacity_bundle =
          spot ? placement_capacity_bundle_t{0, 1,
                                             spot_type_capacity_delta_t{
                                               placement_object_kind_t::user_spot, "room", 1}}
               : placement_capacity_bundle_t{1, 0, std::nullopt};
        return r;
    }

    authority_snapshot_t
    create (std::string id, const authority_snapshot_t *spot = nullptr, bool user_spot = false)
    {
        auto r = request (id, user_spot);
        const auto reserved =
          std::get<object_reserved_t> (repository->reserve (r).result ().value ());
        std::vector<std::byte> payload;
        if (user_spot)
            payload = encode_user_spot_authority_payload (
              {.stable_type = "room",
               .spot_id = id,
               .owner_id = owner.owner_id,
               .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
               .mesh_name = descriptor.mesh_name,
               .node_rid = r.target.node_rid,
               .node_generation = 1});
        else
            payload = encode_actor_authority_payload (
              {.stable_type = "player",
               .actor_id = id,
               .current_spot_id = spot ? "room" : "discard-entry",
               .current_spot_generation = spot ? spot->object_generation : 1,
               .current_spot_kind =
                 spot ? actor_authority_spot_kind_t::user : actor_authority_spot_kind_t::entry,
               .owner_id = owner.owner_id,
               .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
               .mesh_name = descriptor.mesh_name,
               .node_rid = r.target.node_rid,
               .node_generation = 1});
        return std::get<object_committed_t> (
                 repository->commit ({r.key, reserved.fence, payload}).result ().value ())
          .ready;
    }

    location_owner_token_t replace_owner ()
    {
        EXPECT_TRUE (std::holds_alternative<owner_lease_released_t> (
          repository->release_owner_lease (owner).result ().value ()));
        auto next = std::get<owner_lease_claimed_t> (
                      repository->claim_owner_lease (owner.owner_id, 1h).result ().value ())
                      .token;
        descriptor.lease_generation = next.lease_generation;
        ++descriptor.descriptor_revision;
        publish ();
        return next;
    }

    authority_snapshot_t read (authority_key_t key)
    {
        return std::get<authority_snapshot_t> (
          repository->read_authority_core (key).result ().value ());
    }
};

TEST_P (DiscardedAuthority, PreserveReplacesOnlyLeaseToken)
{
    const auto before = create ("actor");
    const auto key = actor_authority_key ("actor");
    const auto next = replace_owner ();
    const auto result = repository
                          ->compare_exchange_authority (key, before.store_version,
                                                        authority_put_t::preserve (before, next))
                          .result ()
                          .value ();
    ASSERT_TRUE (std::holds_alternative<authority_stored_t> (result));
    const auto after = read (key);
    EXPECT_EQ (after.object_generation, before.object_generation);
    EXPECT_EQ (after.authority_owner_generation, before.authority_owner_generation);
    EXPECT_EQ (after.allocation.capacity_bundle, before.allocation.capacity_bundle);
    EXPECT_EQ (after.owner.lease_generation, next.lease_generation);
    EXPECT_EQ (after.allocation.target.owner.lease_generation, next.lease_generation);
    EXPECT_EQ (decode_direct_actor_authority_payload (after.payload)->owner_lease_generation,
               static_cast<std::uint64_t> (next.lease_generation));
    EXPECT_NE (after.store_version, before.store_version);
    const auto capacity = repository->list_mesh_nodes_core (descriptor.mesh_name)
                            .result ()
                            .value ()
                            .items.front ()
                            .capacity;
    EXPECT_EQ (capacity.actors.active, 1u);
    EXPECT_EQ (capacity.actors.reserved, 0u);
    EXPECT_TRUE (repository->cleanup_discarded (key, before, next).result ().value ());
    const auto released = repository->list_mesh_nodes_core (descriptor.mesh_name)
                            .result ()
                            .value ()
                            .items.front ()
                            .capacity;
    EXPECT_EQ (released.actors.active, 0u);
    owner = next;
    const auto later = create ("later");
    EXPECT_EQ (later.object_generation, before.object_generation + 1);
    EXPECT_EQ (later.authority_owner_generation, before.authority_owner_generation + 1);
}

TEST_P (DiscardedAuthority, InvalidOldTokenCleanupDoesNotRequireCreationOrDisabledPolicy)
{
    const auto before = create ("actor");
    const auto key = actor_authority_key ("actor");
    const auto next = replace_owner ();
    repository->configure_reclaim_margin (5s);
    EXPECT_FALSE (repository->cleanup_discarded (key, before, next).result ().value ());
    clock.advance (14999ms);
    EXPECT_FALSE (repository->cleanup_discarded (key, before, next).result ().value ());
    EXPECT_EQ (read (key).store_version, before.store_version);
    clock.advance (1ms);
    EXPECT_TRUE (repository->cleanup_discarded (key, before, next).result ().value ());
    EXPECT_TRUE (std::holds_alternative<authority_missing_t> (
      repository->read_authority_core (key).result ().value ()));
    EXPECT_TRUE (repository->cleanup_discarded (key, before, next).result ().value ());
}

TEST_P (DiscardedAuthority, LiveTokenCleanupUsesCurrentVersionAndPreservesOtherActor)
{
    const auto before = create ("actor");
    const auto other = create ("other");
    const auto key = actor_authority_key ("actor");
    ASSERT_TRUE (std::holds_alternative<authority_stored_t> (
      repository
        ->compare_exchange_authority (key, before.store_version, authority_put_t{before.payload})
        .result ()
        .value ()));
    EXPECT_TRUE (repository->cleanup_discarded (key, before, owner).result ().value ());
    EXPECT_EQ (read (actor_authority_key ("other")).store_version, other.store_version);
}

TEST_P (DiscardedAuthority, OtherIncarnationCompletesWithoutDeleting)
{
    const auto before = create ("actor");
    const auto key = actor_authority_key ("actor");
    ASSERT_TRUE (std::holds_alternative<authority_stored_t> (
      repository
        ->compare_exchange_authority (key, before.store_version,
                                      authority_reincarnate_t{before.payload})
        .result ()
        .value ()));
    const auto current = read (key);
    EXPECT_TRUE (repository->cleanup_discarded (key, before, owner).result ().value ());
    EXPECT_EQ (read (key).store_version, current.store_version);
}

TEST_P (DiscardedAuthority, PreserveRejectsStaleOrChangedEvidenceWithoutMutation)
{
    const auto before = create ("actor");
    const auto key = actor_authority_key ("actor");
    const auto next = replace_owner ();
    for (int scenario = 0; scenario != 13; ++scenario) {
        SCOPED_TRACE (scenario);
        auto evidence = before;
        auto replacement = next;
        auto version = before.store_version;
        switch (scenario) {
            case 0:
                version += "stale";
                break;
            case 1:
                ++evidence.object_generation;
                break;
            case 2:
                ++evidence.authority_owner_generation;
                break;
            case 3:
                ++evidence.allocation.target.node_lifecycle_generation;
                break;
            case 4:
                ++evidence.allocation.capacity_bundle.actor_slots;
                break;
            case 5:
                evidence.owner.owner_id = "another-owner";
                break;
            case 6:
                evidence.allocation.state = placement_allocation_state_t::reserved;
                break;
            case 7:
                evidence.allocation.stable_type = "other";
                break;
            case 8:
                evidence.payload.push_back (std::byte{1});
                break;
            case 9:
                replacement.owner_id = "another-owner";
                break;
            case 10:
                replacement = owner;
                break;
            case 11:
                ++replacement.lease_generation;
                break;
            case 12:
                ++evidence.allocation.target.owner.lease_generation;
                break;
        }
        const auto result = repository
                              ->compare_exchange_authority (
                                key, version, authority_put_t::preserve (evidence, replacement))
                              .result ()
                              .value ();
        EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (result));
        const auto after = read (key);
        EXPECT_EQ (after.store_version, before.store_version);
        EXPECT_EQ (after.payload, before.payload);
    }
}

TEST_P (DiscardedAuthority, PreserveRequiresDifferentGenerationEvenWhenCurrentTokenIsLive)
{
    const auto before = create ("actor");
    const auto result =
      repository
        ->compare_exchange_authority (actor_authority_key ("actor"), before.store_version,
                                      authority_put_t::preserve (before, owner))
        .result ()
        .value ();
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (result));
    EXPECT_EQ (read (actor_authority_key ("actor")).store_version, before.store_version);
}

TEST_P (DiscardedAuthority, ReservedAuthorityRejectsPreserveAndCleanup)
{
    const auto r = request ("reserved");
    const auto reserved = std::get<object_reserved_t> (repository->reserve (r).result ().value ());
    const auto key = actor_authority_key ("reserved");
    const auto before = read (key);
    const auto next = replace_owner ();
    const auto result = repository
                          ->compare_exchange_authority (key, before.store_version,
                                                        authority_put_t::preserve (before, next))
                          .result ()
                          .value ();
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (result));
    EXPECT_FALSE (repository->cleanup_discarded (key, before, next).result ().value ());
    EXPECT_EQ (read (key).store_version, before.store_version);
}

TEST_P (DiscardedAuthority, SpotCleanupRequiresEveryMemberToBeRemoved)
{
    const auto spot = create ("room", nullptr, true);
    const auto actor = create ("actor", &spot);
    const auto other = create ("other", &spot);
    const auto key = spot_authority_key ("room");
    EXPECT_FALSE (repository->cleanup_discarded (key, spot, owner).result ().value ());
    EXPECT_TRUE (repository->cleanup_discarded (actor_authority_key ("actor"), actor, owner)
                   .result ()
                   .value ());
    EXPECT_EQ (read (actor_authority_key ("other")).store_version, other.store_version);
    EXPECT_FALSE (repository->cleanup_discarded (key, spot, owner).result ().value ());
    EXPECT_TRUE (repository->cleanup_discarded (actor_authority_key ("other"), other, owner)
                   .result ()
                   .value ());
    EXPECT_TRUE (repository->cleanup_discarded (key, spot, owner).result ().value ());
}

TEST_P (DiscardedAuthority, StaleCleanupKeepsOtherActorAndGenerationCapacity)
{
    const auto before = create ("actor");
    const auto other = create ("other");
    const auto next = replace_owner ();
    const auto writes = opaque.writes.size ();
    EXPECT_TRUE (repository->cleanup_discarded (actor_authority_key ("actor"), before, next)
                   .result ()
                   .value ());
    EXPECT_EQ (read (actor_authority_key ("other")).store_version, other.store_version);
    if (GetParam ()) {
        for (auto i = writes; i < opaque.writes.size (); ++i)
            for (const auto &mutation : opaque.writes[i].mutations)
                if (const auto *deleted = std::get_if<store_delete_t> (&mutation))
                    EXPECT_EQ (deleted->key.value,
                               std::string ("authority") + '\0' + "actor" + '\0' + "actor");
    }
    owner = next;
    const auto after = create ("new");
    EXPECT_EQ (after.object_generation, other.object_generation + 1);
    EXPECT_EQ (after.authority_owner_generation, other.authority_owner_generation + 1);
}

TEST_P (DiscardedAuthority, SpotScanFailureOrExpiryNeverMeansZeroRows)
{
    if (!GetParam ())
        return; // The lane owns a complete in-memory membership snapshot.
    const auto spot = create ("room", nullptr, true);
    create ("unrelated");
    const auto key = spot_authority_key ("room");
    opaque.fail_scan = true;
    const auto failed = repository->cleanup_discarded (key, spot, owner).result ();
    EXPECT_FALSE (failed.has_value ());
    opaque.fail_scan = false;
    opaque.expire_scan = true;
    EXPECT_FALSE (repository->cleanup_discarded (key, spot, owner).result ().value ());
    EXPECT_EQ (read (key).store_version, spot.store_version);
    opaque.expire_scan = false;
    EXPECT_TRUE (repository->cleanup_discarded (key, spot, owner).result ().value ());
}

TEST_P (DiscardedAuthority, DeleteRetriesUnrelatedDescriptorRepublish)
{
    if (!GetParam ())
        return;
    const auto before = create ("actor");
    const auto writes_before = opaque.writes.size ();
    opaque.before_write = [&] () -> task_t<void> {
        ++descriptor.descriptor_revision;
        EXPECT_EQ (
          (co_await repository->update_mesh_node (descriptor, location_write_intent_t::renew))
            .status,
          location_write_status_t::stored);
    };
    EXPECT_TRUE (repository->cleanup_discarded (actor_authority_key ("actor"), before, owner)
                   .result ()
                   .value ());
    EXPECT_FALSE (static_cast<bool> (opaque.before_write));
    std::size_t deletes = 0;
    for (auto i = writes_before; i < opaque.writes.size (); ++i)
        for (const auto &mutation : opaque.writes[i].mutations)
            if (const auto *deleted = std::get_if<store_delete_t> (&mutation);
                deleted
                && deleted->key.value
                     == std::string ("authority") + '\0' + "actor" + '\0' + "actor")
                ++deletes;
    EXPECT_EQ (deletes, 2u);
}

TEST_P (DiscardedAuthority, PreserveDoesNotWaitForDescriptorLeaseRepublish)
{
    const auto before = create ("actor");
    ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
      repository->release_owner_lease (owner).result ().value ()));
    const auto next = std::get<owner_lease_claimed_t> (
                        repository->claim_owner_lease (owner.owner_id, 1h).result ().value ())
                        .token;
    const auto result =
      repository
        ->compare_exchange_authority (actor_authority_key ("actor"), before.store_version,
                                      authority_put_t::preserve (before, next))
        .result ()
        .value ();
    ASSERT_TRUE (std::holds_alternative<authority_stored_t> (result));
    EXPECT_EQ (read (actor_authority_key ("actor")).owner.lease_generation, next.lease_generation);
    EXPECT_TRUE (repository->cleanup_discarded (actor_authority_key ("actor"), before, next)
                   .result ()
                   .value ());
}

TEST_P (DiscardedAuthority, PreserveRejectsChangedDescriptorLifecycleOrOwner)
{
    const auto before = create ("actor");
    const auto next = replace_owner ();
    ASSERT_EQ (repository->remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, next)
                 .result ()
                 .value (),
               location_write_status_t::stored);
    ++descriptor.lifecycle_generation;
    ++descriptor.descriptor_revision;
    publish ();
    const auto result =
      repository
        ->compare_exchange_authority (actor_authority_key ("actor"), before.store_version,
                                      authority_put_t::preserve (before, next))
        .result ()
        .value ();
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (result));
    EXPECT_EQ (read (actor_authority_key ("actor")).store_version, before.store_version);
}

TEST_P (DiscardedAuthority, CreationStillRequiresDisabledPolicyAfterCleanupObservation)
{
    const auto before = create ("actor");
    const auto next = replace_owner ();
    repository->configure_reclaim_margin (5s);
    const auto key = actor_authority_key ("actor");
    EXPECT_FALSE (repository->cleanup_discarded (key, before, next).result ().value ());
    clock.advance (15s);
    auto r = request ("actor");
    const auto refused =
      repository->release_ended_reservation (key, before.store_version, {}, r).result ();
    EXPECT_FALSE (refused.has_value ());
    EXPECT_EQ (refused.error_kind (), framework_error_kind_t::unavailable);
    EXPECT_EQ (read (key).store_version, before.store_version);
    r.relocation_policy = detail::factory_relocation_kind_t::disabled;
    EXPECT_TRUE (
      repository->release_ended_reservation (key, before.store_version, {}, r).result ().value ());
}

TEST_P (DiscardedAuthority, StaleSpotCleanupDoesNotReleaseAnyMember)
{
    const auto spot = create ("room", nullptr, true);
    const auto member = create ("actor", &spot);
    const auto next = replace_owner ();
    const auto key = spot_authority_key ("room");
    EXPECT_FALSE (repository->cleanup_discarded (key, spot, next).result ().value ());
    EXPECT_EQ (read (actor_authority_key ("actor")).store_version, member.store_version);
    EXPECT_TRUE (repository->cleanup_discarded (actor_authority_key ("actor"), member, next)
                   .result ()
                   .value ());
    EXPECT_TRUE (repository->cleanup_discarded (key, spot, next).result ().value ());
}

TEST_P (DiscardedAuthority, SpotRetainsRemoteOwnerMembershipUntilThatActorCleanup)
{
    const auto spot = create ("room", nullptr, true);
    const auto local = create ("local", &spot);
    const auto local_owner = owner;
    owner = std::get<owner_lease_claimed_t> (
              repository->claim_owner_lease ("remote-owner", 1h).result ().value ())
              .token;
    descriptor.owner_id = owner.owner_id;
    descriptor.lease_generation = owner.lease_generation;
    descriptor.rid = zlink::routing_id_t::from ("remote-node");
    descriptor.entry_spot_id = "remote-entry";
    descriptor.security_identity = "remote-node";
    publish ();
    const auto remote = create ("remote", &spot);
    ASSERT_TRUE (repository->cleanup_discarded (actor_authority_key ("local"), local, local_owner)
                   .result ()
                   .value ());
    const auto key = spot_authority_key ("room");
    EXPECT_FALSE (repository->cleanup_discarded (key, spot, local_owner).result ().value ());
    EXPECT_EQ (read (actor_authority_key ("remote")).store_version, remote.store_version);
    ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
      repository->release_owner_lease (owner).result ().value ()));
    EXPECT_FALSE (repository->cleanup_discarded (key, spot, local_owner).result ().value ());
    EXPECT_EQ (read (actor_authority_key ("remote")).store_version, remote.store_version);
    EXPECT_TRUE (repository->cleanup_discarded (actor_authority_key ("remote"), remote, owner)
                   .result ()
                   .value ());
    EXPECT_TRUE (repository->cleanup_discarded (key, spot, local_owner).result ().value ());
}

TEST_P (DiscardedAuthority, CleanupWinsAgainstPreserveAtTheSameVersion)
{
    const auto before = create ("actor");
    const auto key = actor_authority_key ("actor");
    const auto next = replace_owner ();
    ASSERT_TRUE (repository->cleanup_discarded (key, before, next).result ().value ());
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (
      repository
        ->compare_exchange_authority (key, before.store_version,
                                      authority_put_t::preserve (before, next))
        .result ()
        .value ()));
    EXPECT_TRUE (std::holds_alternative<authority_missing_t> (
      repository->read_authority_core (key).result ().value ()));
}

TEST_P (DiscardedAuthority, StaleCleanupDoesNotDeletePreserveWinner)
{
    if (!GetParam ())
        return;
    const auto before = create ("actor");
    const auto key = actor_authority_key ("actor");
    const auto next = replace_owner ();
    opaque.before_write = [&] () -> task_t<void> {
        const auto result = co_await repository->compare_exchange_authority (
          key, before.store_version, authority_put_t::preserve (before, next));
        EXPECT_TRUE (std::holds_alternative<authority_stored_t> (result));
    };
    EXPECT_FALSE (repository->cleanup_discarded (key, before, next).result ().value ());
    EXPECT_EQ (read (key).owner.lease_generation, next.lease_generation);
    EXPECT_TRUE (repository->cleanup_discarded (key, before, next).result ().value ());
}

TEST_P (DiscardedAuthority, LiveCleanupUsesObservedDescriptorIncludingMissingOrNewLifecycle)
{
    const auto before = create ("actor");
    ASSERT_EQ (repository->remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, owner)
                 .result ()
                 .value (),
               location_write_status_t::stored);
    ++descriptor.lifecycle_generation;
    ++descriptor.descriptor_revision;
    publish ();
    EXPECT_TRUE (repository->cleanup_discarded (actor_authority_key ("actor"), before, owner)
                   .result ()
                   .value ());
    ASSERT_EQ (repository->remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, owner)
                 .result ()
                 .value (),
               location_write_status_t::stored);
    descriptor.lifecycle_generation = 1;
    ++descriptor.descriptor_revision;
    publish ();
    const auto second = create ("second");
    EXPECT_EQ (repository->remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, owner)
                 .result ()
                 .value (),
               location_write_status_t::stored);
    EXPECT_TRUE (repository->cleanup_discarded (actor_authority_key ("second"), second, owner)
                   .result ()
                   .value ());
}

TEST_P (DiscardedAuthority, UserSpotPreserveMaintainsIncarnationAndCapacity)
{
    const auto before = create ("room", nullptr, true);
    const auto next = replace_owner ();
    const auto key = spot_authority_key ("room");
    ASSERT_TRUE (std::holds_alternative<authority_stored_t> (
      repository
        ->compare_exchange_authority (key, before.store_version,
                                      authority_put_t::preserve (before, next))
        .result ()
        .value ()));
    const auto after = read (key);
    EXPECT_EQ (after.object_generation, before.object_generation);
    EXPECT_EQ (after.authority_owner_generation, before.authority_owner_generation);
    EXPECT_EQ (after.allocation.capacity_bundle, before.allocation.capacity_bundle);
    EXPECT_EQ (decode_direct_user_spot_authority_payload (after.payload)->owner_lease_generation,
               static_cast<std::uint64_t> (next.lease_generation));
    EXPECT_TRUE (repository->cleanup_discarded (key, before, next).result ().value ());
}

TEST_P (DiscardedAuthority, InstanceSpotPreserveMaintainsIncarnationAndCapacity)
{
    auto r = request ("instance", true);
    r.key.kind = placement_object_kind_t::instance_spot;
    r.intent.stable_type = "instance";
    r.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::instance_spot, "instance", 1};
    const auto reserved = std::get<object_reserved_t> (repository->reserve (r).result ().value ());
    const auto payload = encode_instance_spot_authority_payload (
      {.stable_type = "instance",
       .spot_id = "instance",
       .owner_id = owner.owner_id,
       .owner_lease_generation = static_cast<std::uint64_t> (owner.lease_generation),
       .mesh_name = descriptor.mesh_name,
       .node_rid = r.target.node_rid,
       .node_generation = 1});
    const auto before = std::get<object_committed_t> (
                          repository->commit ({r.key, reserved.fence, payload}).result ().value ())
                          .ready;
    const auto next = replace_owner ();
    const auto key = spot_authority_key ("instance");
    ASSERT_TRUE (std::holds_alternative<authority_stored_t> (
      repository
        ->compare_exchange_authority (key, before.store_version,
                                      authority_put_t::preserve (before, next))
        .result ()
        .value ()));
    const auto after = read (key);
    EXPECT_EQ (after.object_generation, before.object_generation);
    EXPECT_EQ (after.authority_owner_generation, before.authority_owner_generation);
    EXPECT_EQ (after.allocation.capacity_bundle, before.allocation.capacity_bundle);
    EXPECT_EQ (decode_instance_spot_authority_payload (after.payload)->owner_lease_generation,
               static_cast<std::uint64_t> (next.lease_generation));
    EXPECT_TRUE (repository->cleanup_discarded (key, before, next).result ().value ());
}

TEST_P (DiscardedAuthority, PreserveRejectsActiveCreatingLifecycle)
{
    const auto before = create ("actor");
    const auto key = actor_authority_key ("actor");
    auto actor = *decode_direct_actor_authority_payload (before.payload);
    actor.state = actor_authority_state_t::creating;
    ASSERT_TRUE (std::holds_alternative<authority_stored_t> (
      repository
        ->compare_exchange_authority (key, before.store_version,
                                      authority_put_t{encode_actor_authority_payload (actor)})
        .result ()
        .value ()));
    const auto creating = read (key);
    const auto next = replace_owner ();
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (
      repository
        ->compare_exchange_authority (key, creating.store_version,
                                      authority_put_t::preserve (creating, next))
        .result ()
        .value ()));
    EXPECT_EQ (read (key).store_version, creating.store_version);
}

TEST_P (DiscardedAuthority, CleanupUsesCoreWhenStoreAdmissionIsClosed)
{
    const auto before = create ("actor");
    auto access = std::make_shared<location_repository_t::access_t> ();
    access->admit = [] { return false; };
    repository->configure_access (access);
    EXPECT_TRUE (repository->cleanup_discarded (actor_authority_key ("actor"), before, owner)
                   .result ()
                   .value ());
    EXPECT_TRUE (std::holds_alternative<authority_missing_t> (
      repository->read_authority_core (actor_authority_key ("actor")).result ().value ()));
}

TEST_P (DiscardedAuthority, PreserveRejectsReleasedReplacementToken)
{
    const auto before = create ("actor");
    const auto next = replace_owner ();
    ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
      repository->release_owner_lease (next).result ().value ()));
    const auto key = actor_authority_key ("actor");
    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (
      repository
        ->compare_exchange_authority (key, before.store_version,
                                      authority_put_t::preserve (before, next))
        .result ()
        .value ()));
    EXPECT_EQ (read (key).store_version, before.store_version);
}

INSTANTIATE_TEST_SUITE_P (ProviderAndMemory, DiscardedAuthority, ::testing::Bool ());
} // namespace
