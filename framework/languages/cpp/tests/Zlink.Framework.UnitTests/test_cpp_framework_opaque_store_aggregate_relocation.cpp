/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "../support/aggregate_page_read_fixture.hpp"
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

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace zlink::framework::test::opaque_store;

class NewOwnerRecoveryTest : public ::testing::TestWithParam<std::tuple<bool, bool, bool>>
{
};

TEST_P (NewOwnerRecoveryTest, SourceDescriptorAndLeaseDoNotOwnTargetCommit)
{
    creation_terminal_failure_store_t store;
    provider_location_repository_t provider (store);
    provider.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    in_memory_location_repository_t memory;
    memory.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    location_repository_t &repository = std::get<0> (GetParam ())
                                          ? static_cast<location_repository_t &> (provider)
                                          : static_cast<location_repository_t &> (memory);
    const bool aggregate = std::get<1> (GetParam ());
    const bool target_live = std::get<2> (GetParam ());
    const auto source_claim = repository.claim_owner_lease ("source", 30s).result ().value ();
    const auto target_claim = repository.claim_owner_lease ("target", 30s).result ().value ();
    const auto source = std::get<owner_lease_claimed_t> (source_claim).token;
    const auto target = std::get<owner_lease_claimed_t> (target_claim).token;
    const auto publish = [&] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "new-owner-recovery";
        descriptor.rid = zlink::routing_id_t::from (rid);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.activation_concurrency.limit = 2;
        descriptor.security_identity = "new-owner-recovery";
        descriptor.object_capabilities = {
          {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0},
          {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::snapshot, true,
           8}};
        descriptor.capacity.actors.limit = 1;
        descriptor.capacity.spots.limit = 1;
        descriptor.capacity.spot_types.push_back (
          {placement_object_kind_t::user_spot, "room", {0, 0, 1}});
        EXPECT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
        return descriptor;
    };
    const auto source_descriptor = publish ("source-node", source);
    publish ("target-node", target);
    const object_creation_target_t source_placement{
      "new-owner-recovery", node_rid_t::from_string ("source-node"), 1, source};
    const object_creation_target_t target_placement{
      "new-owner-recovery", node_rid_t::from_string ("target-node"), 1, target};
    const auto create = [&] (placement_object_kind_t kind, std::string id) {
        object_reserve_request_t request;
        request.key = {kind, std::move (id)};
        request.intent.stable_type = kind == placement_object_kind_t::actor ? "player" : "room";
        request.target = source_placement;
        if (kind == placement_object_kind_t::actor)
            request.capacity_bundle.actor_slots = 1;
        else {
            request.capacity_bundle.spot_slots = 1;
            request.capacity_bundle.spot_type =
              spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
        }
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
    const auto actor = create (placement_object_kind_t::actor, "actor");
    const auto spot =
      aggregate ? create (placement_object_kind_t::user_spot, "spot") : authority_snapshot_t{};
    ASSERT_EQ (
      repository
        .remove_mesh_node (
          {"new-owner-recovery", zlink::routing_id_t::from (std::string ("source-node"))}, source)
        .result ()
        .value (),
      location_write_status_t::stored);
    ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
      repository.release_owner_lease (source).result ().value ()));
    if (std::get<0> (GetParam ()) && target_live) {
        store.conflicting_capacity_key = capacity_key (source_descriptor);
        store.fault = creation_terminal_failure_store_t::fault_t::capacity_conflict_once;
    }
    if (aggregate) {
        aggregate_prepare_request_t request;
        request.aggregate_id.value[0] = std::byte{1};
        request.aggregate_generation = 1;
        request.participants = {
          {actor_authority_key ("actor"), actor.store_version,
           authority_generation_transition_t::new_owner, bytes ("moved-actor")},
          {spot_authority_key ("spot"), spot.store_version,
           authority_generation_transition_t::new_owner, bytes ("moved-spot")}};
        request.target_owner = target;
        request.target_descriptor = {"new-owner-recovery",
                                     zlink::routing_id_t::from (std::string ("target-node"))};
        request.target_descriptor_lifecycle_generation = 1;
        request.capacity_bundle.actor_slots = 1;
        request.capacity_bundle.spot_slots = 1;
        request.capacity_bundle.spot_type =
          spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
        const auto prepared = repository.prepare_aggregate (request).result ().value ();
        const auto *fence = std::get_if<aggregate_prepared_t> (&prepared);
        ASSERT_NE (fence, nullptr);
        if (!target_live)
            ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
              repository.release_owner_lease (target).result ().value ()));
        EXPECT_EQ (repository.commit_aggregate (fence->fence).result ().value (),
                   target_live ? aggregate_commit_result_t::committed
                               : aggregate_commit_result_t::stale);
    } else {
        if (!target_live)
            ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
              repository.release_owner_lease (target).result ().value ()));
        const auto moved =
          repository
            .compare_exchange_authority (actor_authority_key ("actor"), actor.store_version,
                                         authority_retarget_t{bytes ("moved"), target_placement})
            .result ()
            .value ();
        EXPECT_EQ (std::holds_alternative<authority_stored_t> (moved), target_live);
    }
    const auto observed =
      repository.read_authority (actor_authority_key ("actor")).result ().value ();
    const auto *current = std::get_if<authority_snapshot_t> (&observed);
    ASSERT_NE (current, nullptr);
    EXPECT_EQ (current->object_generation, actor.object_generation);
    EXPECT_EQ (current->owner.owner_id, target_live ? target.owner_id : source.owner_id);
    EXPECT_EQ (current->allocation.target.node_rid.value (),
               target_live ? "target-node" : "source-node");
    if (target_live)
        EXPECT_NE (current->store_version, actor.store_version);
    else
        EXPECT_EQ (current->store_version, actor.store_version);
}

INSTANTIATE_TEST_SUITE_P (RepositoryImplementations,
                          NewOwnerRecoveryTest,
                          ::testing::Combine (::testing::Bool (),
                                              ::testing::Bool (),
                                              ::testing::Bool ()));
class aggregate_commit_contention_store_t final : public location_store_t
{
  public:
    enum class phase_t
    {
        transition,
        page,
        terminal,
        lease_loss,
        counter_race
    };
    task_t<store_read_result_t> read (store_key_t key) override
    {
        if (on_counter_read && key.value == "zlink:v11:authority-owner-counter"
            && ++counter_reads == 2) {
            // The injected peer step runs on the lane that resumes the
            // repository, so it is awaited rather than blocking that lane.
            auto action = std::move (on_counter_read);
            co_await action ();
        }
        co_return co_await inner.read (std::move (key));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        bool counter = false;
        bool page = false;
        bool capacity = false;
        for (const auto &mutation : request.mutations) {
            const auto &key = std::visit (
              [] (const auto &value) -> const store_key_t & { return value.key; }, mutation);
            counter |= key.value == "zlink:v11:authority-owner-counter";
            page |= key.value.starts_with ("zlink:v11:aggregate-commit:");
            capacity |= key.value.starts_with ("zlink:v11:capacity:");
        }
        const bool matching = phase == phase_t::transition ? counter
                              : phase == phase_t::page     ? page
                                                           : capacity;
        if (remaining != 0 && matching) {
            --remaining;
            ++rejected;
            if (on_conflict) {
                auto action = std::move (on_conflict);
                co_await action ();
            }
            co_return store_write_result_t{
              store_write_conflict_t{std::chrono::system_clock::now ()}};
        }
        co_return co_await inner.write (std::move (request));
    }
    in_memory_location_store_t inner;
    phase_t phase = phase_t::transition;
    std::size_t remaining = 0;
    std::size_t rejected = 0;
    std::function<task_t<void> ()> on_conflict;
    std::function<task_t<void> ()> on_counter_read;
    std::size_t counter_reads = 0;
};

class aggregate_lock_contention_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }

    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        const auto publishes_lock = std::any_of (
          request.mutations.begin (), request.mutations.end (), [] (const auto &mutation) {
              const auto *put = std::get_if<store_put_t> (&mutation);
              return put && put->key.value.starts_with ("zlink:v11:aggregate-lock:");
          });
        if (!peer_marker_published && publishes_lock) {
            peer_marker_published = true;
            auto peer_request = request;
            for (auto &mutation : peer_request.mutations) {
                auto *put = std::get_if<store_put_t> (&mutation);
                if (!put || !put->key.value.starts_with ("zlink:v11:aggregate-lock:"))
                    continue;
                auto marker = nlohmann::json::parse (
                  reinterpret_cast<const char *> (put->bytes.data ()),
                  reinterpret_cast<const char *> (put->bytes.data ()) + put->bytes.size ());
                marker["peerMarker"] = true;
                put->bytes = bytes (marker.dump ());
            }
            // The repository may resume on the inner store's lane, so await
            // the peer write instead of blocking that lane.
            const auto published = co_await inner.write (std::move (peer_request));
            if (const auto *applied = std::get_if<store_write_applied_t> (&published))
                co_return store_write_conflict_t{applied->store_now};
            co_return published;
        }
        co_return co_await inner.write (std::move (request));
    }

    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }

    in_memory_location_store_t inner;
    bool peer_marker_published = false;
};

TEST (CppFrameworkOpaqueLocationStore, AggregatePrepareAdoptsPeerLockAfterConditionalWriteConflict)
{
    aggregate_lock_contention_store_t provider;
    provider_location_repository_t repository (provider);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto source_claim =
      repository.claim_owner_lease ("aggregate-race-source-owner", 30s).result ().value ();
    const auto *source_owner = std::get_if<owner_lease_claimed_t> (&source_claim);
    ASSERT_NE (source_owner, nullptr);
    const auto target_claim =
      repository.claim_owner_lease ("aggregate-race-target-owner", 30s).result ().value ();
    const auto *target_owner = std::get_if<owner_lease_claimed_t> (&target_claim);
    ASSERT_NE (target_owner, nullptr);

    mesh_node_descriptor_t source_descriptor;
    source_descriptor.mesh_name = "aggregate-race";
    source_descriptor.rid = zlink::routing_id_t::from (std::string{"aggregate-race-source"});
    source_descriptor.lifecycle_generation = 1;
    source_descriptor.descriptor_revision = 1;
    source_descriptor.endpoint = "tcp://127.0.0.1:7001";
    source_descriptor.owner_id = source_owner->token.owner_id;
    source_descriptor.lease_generation = source_owner->token.lease_generation;
    source_descriptor.object_role = object_role_t::server;
    source_descriptor.state = framework_runtime_state_t::serving;
    source_descriptor.object_capabilities = {
      {placement_object_kind_t::actor, "player", maintenance_policy_kind_t::recreate, false, 0},
      {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::snapshot, true, 1}};
    source_descriptor.capacity.actors.limit = 1;
    source_descriptor.capacity.spots.limit = 1;
    source_descriptor.capacity.spot_types.push_back (
      {placement_object_kind_t::user_spot, "room", {0, 0, 1}});
    ASSERT_EQ (repository.update_mesh_node (source_descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    auto target_descriptor = source_descriptor;
    target_descriptor.rid = zlink::routing_id_t::from (std::string{"aggregate-race-target"});
    target_descriptor.endpoint = "tcp://127.0.0.1:7002";
    target_descriptor.owner_id = target_owner->token.owner_id;
    target_descriptor.lease_generation = target_owner->token.lease_generation;
    ASSERT_EQ (repository.update_mesh_node (target_descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    const object_creation_target_t source_target{source_descriptor.mesh_name,
                                                 node_rid_t::from_string ("aggregate-race-source"),
                                                 1, source_owner->token};
    object_reserve_request_t actor_request;
    actor_request.key = {placement_object_kind_t::actor, "aggregate-race-actor"};
    actor_request.intent.stable_type = "player";
    actor_request.target = source_target;
    actor_request.creating_payload = bytes ("creating");
    actor_request.capacity_bundle.actor_slots = 1;
    const auto actor_reserved = repository.reserve (actor_request).result ().value ();
    const auto *actor_fence = std::get_if<object_reserved_t> (&actor_reserved);
    ASSERT_NE (actor_fence, nullptr);
    const auto actor_committed =
      repository.commit ({actor_request.key, actor_fence->fence, bytes ("ready")})
        .result ()
        .value ();
    const auto *actor = std::get_if<object_committed_t> (&actor_committed);
    ASSERT_NE (actor, nullptr);

    object_reserve_request_t spot_request;
    spot_request.key = {placement_object_kind_t::user_spot, "aggregate-race-spot"};
    spot_request.intent.stable_type = "room";
    spot_request.target = source_target;
    spot_request.creating_payload = bytes ("creating");
    spot_request.capacity_bundle.spot_slots = 1;
    spot_request.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    const auto spot_reserved = repository.reserve (spot_request).result ().value ();
    const auto *spot_fence = std::get_if<object_reserved_t> (&spot_reserved);
    ASSERT_NE (spot_fence, nullptr);
    const auto spot_committed =
      repository.commit ({spot_request.key, spot_fence->fence, bytes ("ready")}).result ().value ();
    const auto *spot = std::get_if<object_committed_t> (&spot_committed);
    ASSERT_NE (spot, nullptr);

    const auto source_capacity = capacity_record (provider, source_descriptor);
    EXPECT_EQ (source_capacity.at ("active").at ("actors"), 1);
    EXPECT_EQ (source_capacity.at ("active").at ("spots"), 1);
    EXPECT_TRUE (std::holds_alternative<store_missing_t> (
      provider.read (capacity_key (target_descriptor)).result ().value ()));

    aggregate_prepare_request_t aggregate;
    aggregate.aggregate_id.value[15] = std::byte{0x44};
    aggregate.aggregate_generation = 1;
    aggregate.participants = {{actor_authority_key (actor_request.key.global_id),
                               actor->ready.store_version,
                               authority_generation_transition_t::new_owner,
                               bytes ("aggregate-actor"),
                               {}},
                              {spot_authority_key (spot_request.key.global_id),
                               spot->ready.store_version,
                               authority_generation_transition_t::new_owner,
                               bytes ("aggregate-spot"),
                               {}}};
    aggregate.target_descriptor = {target_descriptor.mesh_name, target_descriptor.rid};
    aggregate.target_descriptor_lifecycle_generation = 1;
    aggregate.capacity_bundle.actor_slots = 1;
    aggregate.capacity_bundle.spot_slots = 1;
    aggregate.capacity_bundle.spot_type =
      spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
    aggregate.target_owner = target_owner->token;

    const auto prepared = repository.prepare_aggregate (aggregate).result ().value ();
    EXPECT_TRUE (provider.peer_marker_published);
    ASSERT_TRUE (std::holds_alternative<aggregate_prepared_t> (prepared));
    for (const auto &mutation : std::vector<authority_mutation_t>{
           authority_put_t{}, authority_reincarnate_t{}, authority_delete_t{}})
        EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (
          repository
            .compare_exchange_authority (actor_authority_key (actor_request.key.global_id),
                                         actor->ready.store_version, mutation)
            .result ()
            .value ()));
    EXPECT_EQ (source_capacity, capacity_record (provider, source_descriptor));
}

TEST (CppFrameworkOpaqueLocationStore, AggregatePageReadKeepsSourceVersionUntilCommit)
{
    in_memory_location_store_t provider;
    zlink::framework::test::aggregate_page_read_keeps_source_version_until_commit (provider, bytes);
}

TEST (CppFrameworkOpaqueLocationStore, AggregateCommitRechecksFenceAfterTransientConflicts)
{
    using phase_t = aggregate_commit_contention_store_t::phase_t;
    for (const auto phase : {phase_t::transition, phase_t::page, phase_t::terminal,
                             phase_t::lease_loss, phase_t::counter_race}) {
        SCOPED_TRACE (static_cast<int> (phase));
        aggregate_commit_contention_store_t provider;
        provider_location_repository_t repository (provider);
        repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
        const auto claim =
          repository.claim_owner_lease ("aggregate-contention-owner", 30s).result ().value ();
        const auto *claimed = std::get_if<owner_lease_claimed_t> (&claim);
        ASSERT_NE (claimed, nullptr);
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "aggregate-contention";
        descriptor.rid = zlink::routing_id_t::from (std::string{"aggregate-contention-node"});
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = claimed->token.owner_id;
        descriptor.lease_generation = claimed->token.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                                   maintenance_policy_kind_t::recreate, false, 0});
        descriptor.object_capabilities.push_back ({placement_object_kind_t::user_spot, "room",
                                                   maintenance_policy_kind_t::snapshot, true, 10});
        descriptor.capacity.actors.limit = 10;
        descriptor.capacity.spots.limit = 10;
        descriptor.capacity.spot_types.push_back (
          {placement_object_kind_t::user_spot, "room", {0, 0, 10}});
        ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
        object_creation_target_t target{descriptor.mesh_name,
                                        node_rid_t::from_string ("aggregate-contention-node"), 1,
                                        claimed->token};
        aggregate_prepare_request_t aggregate;
        aggregate.aggregate_id.value[15] = std::byte{0x55};
        aggregate.aggregate_generation = 1;
        aggregate.target_descriptor = {descriptor.mesh_name, descriptor.rid};
        aggregate.target_descriptor_lifecycle_generation = 1;
        aggregate.target_owner = claimed->token;
        aggregate.capacity_bundle.actor_slots = 2;
        aggregate.capacity_bundle.spot_slots = 1;
        aggregate.capacity_bundle.spot_type =
          spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1};
        for (const auto &[kind, id] :
             {std::pair{placement_object_kind_t::actor, "aggregate-contention-actor-a"},
              std::pair{placement_object_kind_t::actor, "aggregate-contention-actor-b"},
              std::pair{placement_object_kind_t::user_spot, "aggregate-contention-spot"}}) {
            object_reserve_request_t request;
            request.key = {kind, id};
            request.intent.stable_type = kind == placement_object_kind_t::actor ? "player" : "room";
            request.target = target;
            request.creating_payload = bytes ("creating");
            if (kind == placement_object_kind_t::actor) {
                request.capacity_bundle.actor_slots = 1;
            } else {
                request.capacity_bundle.spot_slots = 1;
                request.capacity_bundle.spot_type = spot_type_capacity_delta_t{kind, "room", 1};
            }
            const auto reserved = repository.reserve (request).result ().value ();
            const auto *reservation = std::get_if<object_reserved_t> (&reserved);
            ASSERT_NE (reservation, nullptr);
            const auto committed =
              repository.commit ({request.key, reservation->fence, bytes ("ready")})
                .result ()
                .value ();
            const auto *ready = std::get_if<object_committed_t> (&committed);
            ASSERT_NE (ready, nullptr);
            aggregate.participants.push_back ({kind == placement_object_kind_t::actor
                                                 ? actor_authority_key (id)
                                                 : spot_authority_key (id),
                                               ready->ready.store_version,
                                               authority_generation_transition_t::new_owner,
                                               bytes ("aggregate-ready"),
                                               {}});
        }
        const auto prepared = repository.prepare_aggregate (aggregate).result ().value ();
        const auto *fence = std::get_if<aggregate_prepared_t> (&prepared);
        ASSERT_NE (fence, nullptr);
        provider.phase = phase;
        provider.remaining = phase == phase_t::counter_race ? 0 : 65;
        if (phase == phase_t::counter_race) {
            provider.on_counter_read = [&] () -> task_t<void> {
                object_reserve_request_t peer;
                peer.key = {placement_object_kind_t::actor, "aggregate-contention-peer"};
                peer.intent.stable_type = "player";
                peer.target = target;
                peer.creating_payload = bytes ("peer-creating");
                peer.capacity_bundle.actor_slots = 1;
                const auto reserved = co_await repository.reserve (peer);
                EXPECT_TRUE (std::holds_alternative<object_reserved_t> (reserved));
            };
        }
        if (phase == phase_t::lease_loss) {
            provider.on_conflict = [&] () -> task_t<void> {
                const auto released = co_await repository.release_owner_lease (claimed->token);
                EXPECT_TRUE (std::holds_alternative<owner_lease_released_t> (released));
            };
            EXPECT_EQ (repository.commit_aggregate (fence->fence).result ().value (),
                       aggregate_commit_result_t::stale);
            EXPECT_EQ (provider.rejected, 1u);
            provider.remaining = 0;
            EXPECT_EQ (repository.abort_aggregate (fence->fence).result ().value (),
                       aggregate_abort_result_t::aborted);
        } else {
            EXPECT_EQ (repository.commit_aggregate (fence->fence).result ().value (),
                       aggregate_commit_result_t::committed);
            EXPECT_EQ (provider.rejected, phase == phase_t::counter_race ? 0u : 65u);
            EXPECT_EQ (repository.commit_aggregate (fence->fence).result ().value (),
                       aggregate_commit_result_t::already_committed);
        }
    }
}

TEST (CppFrameworkOpaqueLocationStore, AggregateCommitUsesBoundedBatches)
{
    in_memory_location_store_t provider;
    provider_location_repository_t repository (provider);
    repository.configure_reclaim_margin (std::chrono::milliseconds::zero ());
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
      {placement_object_kind_t::user_spot, "room", maintenance_policy_kind_t::snapshot, true, 8}};
    descriptor.capacity.actors.limit = 8192;
    descriptor.capacity.spots.limit = 8;
    descriptor.capacity.spot_types.push_back (
      {placement_object_kind_t::user_spot, "room", {0, 0, 8}});
    ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                 .result ()
                 .value ()
                 .status,
               location_write_status_t::stored);

    const object_creation_target_t target{"large", node_rid_t::from_string ("node-large"), 1,
                                          claimed->token};
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
        const auto committed =
          repository.commit ({request.key, fence->fence, bytes ("ready")}).result ().value ();
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
      repository.commit ({spot_request.key, spot_fence->fence, bytes ("ready")}).result ().value ();
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
    for (const auto &key : {large_actor_0, large_actor_1, large_actor_2, large_spot_key.value}) {
        SCOPED_TRACE (key);
        const auto found = repository.read_authority ({key}).result ().value ();
        const auto *snapshot = std::get_if<authority_snapshot_t> (&found);
        ASSERT_NE (snapshot, nullptr);
        EXPECT_TRUE (snapshot->payload.size () >= 10);
        EXPECT_EQ (snapshot->owner.owner_id, "owner-large");
    }
}
}
