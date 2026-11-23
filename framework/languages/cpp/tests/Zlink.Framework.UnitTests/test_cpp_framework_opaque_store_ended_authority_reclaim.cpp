/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "../support/location_reclaim_clock.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/provider_relocation_repository.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "../support/actor_authority_fixture.hpp"
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

#include "../support/opaque_store_record_fixture.hpp"
#include "../support/opaque_store_reclaim_fixture.hpp"

namespace zlink::framework::runtime
{
struct in_memory_location_repository_test_access_t
{
    static auto save_owner (in_memory_location_repository_t &repository,
                            const location_owner_token_t &owner)
    {
        const auto row = repository._leases.at (owner.owner_id);
        return [&repository, owner, row] {
            repository._leases[owner.owner_id] = row;
            repository._active_lease_generations[owner.owner_id] = owner.lease_generation;
        };
    }
};
} // namespace zlink::framework::runtime

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;
using namespace zlink::framework::test::opaque_store;

class ActiveActorReclaimTest : public ::testing::TestWithParam<std::tuple<bool, std::string>>
{
};

TEST_P (ActiveActorReclaimTest, RecreatesOnlyEndedDisabledActor)
{
    reclaim_scan_store_t opaque;
    provider_location_repository_t provider (opaque);
    provider.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    in_memory_location_repository_t memory;
    memory.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    location_repository_t &repository = std::get<0> (GetParam ())
                                          ? static_cast<location_repository_t &> (provider)
                                          : static_cast<location_repository_t &> (memory);
    location_reclaim_test_access_t clock;
    clock.bind (repository);
    const auto scenario = std::get<1> (GetParam ());
    const auto old_owner = std::get<owner_lease_claimed_t> (
                             repository.claim_owner_lease ("reclaim-old", 60s).result ().value ())
                             .token;
    const auto new_owner = std::get<owner_lease_claimed_t> (
                             repository.claim_owner_lease ("reclaim-new", 60s).result ().value ())
                             .token;
    const auto publish = [&] (std::string rid, location_owner_token_t owner) {
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "reclaim-mesh";
        descriptor.rid = zlink::routing_id_t::from (rid);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.application_version = 1;
        descriptor.security_identity = rid;
        descriptor.entry_spot_id = "reclaim-entry-" + rid;
        descriptor.activation_concurrency.limit = 1;
        const auto policy =
          rid == "reclaim-target" && (scenario == "recreate" || scenario == "relocationRecreate")
            ? maintenance_policy_kind_t::recreate
            : maintenance_policy_kind_t::disabled;
        descriptor.object_capabilities.push_back (
          {placement_object_kind_t::actor, "player", policy, false, 0});
        descriptor.capacity.actors.limit = 1;
        EXPECT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
        return descriptor;
    };
    std::function<void ()> restore_memory_owner;
    if (!std::get<0> (GetParam ()))
        restore_memory_owner =
          in_memory_location_repository_test_access_t::save_owner (memory, old_owner);
    std::optional<store_found_t> saved_owner;
    if (std::get<0> (GetParam ()))
        saved_owner = std::get<store_found_t> (
          opaque.inner.read ({std::string ("owner-lease") + '\0' + old_owner.owner_id})
            .result ()
            .value ());
    const auto old_descriptor = publish ("reclaim-source", old_owner);
    publish ("reclaim-target", new_owner);
    repository.configure_reclaim_margin (5s);
    object_reserve_request_t request;
    request.key = {placement_object_kind_t::actor, "reclaim-actor"};
    request.intent.stable_type = "player";
    request.target = {"reclaim-mesh", node_rid_t::from_string ("reclaim-source"), 1, old_owner};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle.actor_slots = 1;
    task_completion_source_t<void> missing_read_held;
    task_completion_source_t<void> release_missing_read;
    std::optional<task_t<authority_read_result_t>> older_missing;
    if (scenario == "staleMissingRead") {
        opaque.after_read_key = {std::string ("authority") + '\0' + "actor" + '\0'
                                 + request.key.global_id};
        opaque.after_read = [&] () -> task_t<void> {
            missing_read_held.complete (result_t<void>::success ());
            co_await release_missing_read.task ();
        };
        older_missing.emplace (
          repository.read_authority (actor_authority_key (request.key.global_id)));
        missing_read_held.task ().result ().value ();
    }
    const auto first =
      std::get<object_reserved_t> (repository.reserve (request).result ().value ());
    auto ready_payload = encode_actor_authority_payload (actor_authority_payload_t{
      .state =
        scenario == "notReady" ? actor_authority_state_t::creating : actor_authority_state_t::ready,
      .stable_type = "player",
      .actor_id = "reclaim-actor",
      .current_spot_id = "reclaim-entry",
      .current_spot_generation = 1,
      .current_spot_kind = actor_authority_spot_kind_t::entry,
      .owner_id = old_owner.owner_id,
      .owner_lease_generation = static_cast<std::uint64_t> (old_owner.lease_generation),
      .mesh_name = "reclaim-mesh",
      .node_rid = request.target.node_rid,
      .node_generation = 1});
    if ((scenario == "relocation" || scenario == "relocationRecreate")) {
        auto canonical = encode_actor_authority_payload (actor_authority_payload_t{
          .stable_type = "player",
          .actor_id = "reclaim-actor",
          .current_spot_id = "reclaim-entry",
          .current_spot_generation = 1,
          .current_spot_kind = actor_authority_spot_kind_t::entry,
          .owner_id = old_owner.owner_id,
          .owner_lease_generation = static_cast<std::uint64_t> (old_owner.lease_generation),
          .mesh_name = "reclaim-mesh",
          .node_rid = request.target.node_rid,
          .node_generation = 1});
        std::ifstream fixture (ZLINK_AUTHORITY_RELOCATION_STATE_GOLDEN_PATH);
        const auto golden = nlohmann::json::parse (fixture);
        const auto slot =
          tests::from_hex (golden.at ("valid").at (0).at ("hex").get<std::string> ());
        ready_payload = tests::with_relocation_slot (canonical, {slot.begin () + 5, slot.end ()});
        const auto decoded = decode_direct_actor_authority_payload (ready_payload);
        ASSERT_TRUE (decoded && decoded->has_relocation_state);
    }
    ASSERT_TRUE (std::holds_alternative<object_committed_t> (
      repository.commit ({request.key, first.fence, ready_payload}).result ().value ()));
    const auto committed =
      repository.read_authority (actor_authority_key (request.key.global_id)).result ().value ();
    ASSERT_TRUE (std::holds_alternative<authority_snapshot_t> (committed));
    EXPECT_FALSE (std::get<authority_snapshot_t> (committed).pending_creation);
    if (scenario == "liveDescriptorGone")
        repository.remove_mesh_node ({old_descriptor.mesh_name, old_descriptor.rid}, old_owner)
          .result ()
          .value ();
    if (scenario == "staleLiveRead") {
        ASSERT_TRUE (std::get<0> (GetParam ()));
        task_completion_source_t<void> held;
        task_completion_source_t<void> release;
        opaque.after_read_key = {std::string ("owner-lease") + '\0' + old_owner.owner_id};
        opaque.after_read = [&] () -> task_t<void> {
            held.complete (result_t<void>::success ());
            co_await release.task ();
        };
        auto older = repository.reserve (request);
        held.task ().result ().value ();
        repository.release_owner_lease (old_owner).result ().value ();
        request.target = {"reclaim-mesh", node_rid_t::from_string ("reclaim-target"), 1, new_owner};
        const auto waiting = repository.reserve (request).result ();
        EXPECT_FALSE (waiting);
        EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
        clock.advance (15s - 1ms);
        release.complete (result_t<void>::success ());
        EXPECT_TRUE (older.result ());
        EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
        const auto before_boundary = repository.reserve (request).result ();
        EXPECT_FALSE (before_boundary);
        clock.advance (1ms);
        const auto at_boundary = repository.reserve (request).result ();
        ASSERT_TRUE (at_boundary);
        EXPECT_TRUE (std::holds_alternative<object_reserved_t> (at_boundary.value ()));
        return;
    }
    if (scenario != "live" && scenario != "liveDescriptorGone")
        repository.release_owner_lease (old_owner).result ().value ();
    request.target = {"reclaim-mesh", node_rid_t::from_string ("reclaim-target"), 1, new_owner};
    if (scenario == "concurrentReclaimDelete") {
        EXPECT_FALSE (repository.reserve (request).result ());
        EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
        clock.advance (15s);
        const auto &snapshot = std::get<authority_snapshot_t> (committed);
        const auto authority_key = actor_authority_key (request.key.global_id);
        task_completion_source_t<void> first_held;
        task_completion_source_t<void> release_first;
        opaque.before_write = [&] () -> task_t<void> {
            first_held.complete (result_t<void>::success ());
            co_await release_first.task ();
        };
        auto first =
          repository.release_ended_reservation (authority_key, snapshot.store_version, {}, request);
        first_held.task ().result ().value ();
        task_completion_source_t<void> second_held;
        task_completion_source_t<void> release_second;
        opaque.before_write = [&] () -> task_t<void> {
            second_held.complete (result_t<void>::success ());
            co_await release_second.task ();
        };
        auto second =
          repository.release_ended_reservation (authority_key, snapshot.store_version, {}, request);
        second_held.task ().result ().value ();
        EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
        release_first.complete (result_t<void>::success ());
        EXPECT_TRUE (first.result ().value ());
        EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
        release_second.complete (result_t<void>::success ());
        EXPECT_FALSE (second.result ().value ());
        EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
        return;
    }
    if (scenario == "delete" || scenario == "deleteMissing" || scenario == "lateDelete"
        || scenario == "staleMissingRead") {
        const auto waiting = repository.reserve (request).result ();
        EXPECT_FALSE (waiting);
        EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
        if (scenario == "delete" || scenario == "deleteMissing" || scenario == "lateDelete") {
            if (saved_owner) {
                const store_key_t owner_key{std::string ("owner-lease") + '\0'
                                            + old_owner.owner_id};
                opaque.inner.write ({{}, {store_put_t{owner_key, saved_owner->value.bytes, 60s}}})
                  .result ()
                  .value ();
            } else
                restore_memory_owner ();
            const auto &snapshot = std::get<authority_snapshot_t> (committed);
            const auto authority_key = actor_authority_key (request.key.global_id);
            task_completion_source_t<void> delete_held;
            task_completion_source_t<void> release_delete;
            if (scenario == "lateDelete") {
                opaque.after_write = [&] () -> task_t<void> {
                    delete_held.complete (result_t<void>::success ());
                    co_await release_delete.task ();
                };
            }
            provider_location_repository_t remote (opaque);
            auto &deleter = scenario == "deleteMissing"
                              ? static_cast<location_repository_t &> (remote)
                              : repository;
            auto deletion = deleter.compare_exchange_authority (
              authority_key, snapshot.store_version, authority_delete_t{});
            if (scenario == "lateDelete") {
                delete_held.task ().result ().value ();
                const auto next =
                  std::get<object_reserved_t> (repository.reserve (request).result ().value ());
                auto payload = *decode_direct_actor_authority_payload (ready_payload);
                payload.owner_id = new_owner.owner_id;
                payload.owner_lease_generation =
                  static_cast<std::uint64_t> (new_owner.lease_generation);
                payload.node_rid = request.target.node_rid;
                EXPECT_TRUE (std::holds_alternative<object_committed_t> (
                  repository
                    .commit ({request.key, next.fence, encode_actor_authority_payload (payload)})
                    .result ()
                    .value ()));
                repository.release_owner_lease (new_owner).result ().value ();
                request.target = {"reclaim-mesh", node_rid_t::from_string ("reclaim-source"), 1,
                                  old_owner};
                EXPECT_FALSE (repository.reserve (request).result ());
                release_delete.complete (result_t<void>::success ());
                EXPECT_TRUE (
                  std::holds_alternative<authority_deleted_t> (deletion.result ().value ()));
                EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 2u);
                clock.advance (15s);
                const auto reclaimed = repository.reserve (request).result ();
                ASSERT_TRUE (reclaimed);
                EXPECT_TRUE (std::holds_alternative<object_reserved_t> (reclaimed.value ()));
            } else {
                EXPECT_TRUE (
                  std::holds_alternative<authority_deleted_t> (deletion.result ().value ()));
                if (scenario == "deleteMissing") {
                    EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
                    EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (
                      repository
                        .compare_exchange_authority (authority_key, snapshot.store_version,
                                                     authority_delete_t{})
                        .result ()
                        .value ()));
                }
                EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
            }
        } else {
            clock.advance (15s - 1ms);
            release_missing_read.complete (result_t<void>::success ());
            EXPECT_TRUE (
              std::holds_alternative<authority_missing_t> (older_missing->result ().value ()));
            EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 1u);
            EXPECT_FALSE (repository.reserve (request).result ());
            clock.advance (1ms);
            const auto result = repository.reserve (request).result ();
            ASSERT_TRUE (result);
            EXPECT_TRUE (std::holds_alternative<object_reserved_t> (result.value ()));
        }
        return;
    }
    if (scenario == "typeMismatch")
        request.intent.stable_type = "different";
    if (scenario == "recreate" || scenario == "relocation" || scenario == "relocationRecreate"
        || scenario == "notReady") {
        const auto waiting = repository.reserve (request).result ();
        ASSERT_FALSE (waiting);
        EXPECT_EQ (waiting.error_kind (), framework_error_kind_t::unavailable);
        EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 0u);
        clock.advance (15s);
    }
    if (scenario == "observerRestart") {
        const auto before = std::get<authority_snapshot_t> (
          repository.read_authority (actor_authority_key (request.key.global_id))
            .result ()
            .value ());
        const auto waiting = repository.reserve (request).result ();
        ASSERT_FALSE (waiting);
        EXPECT_EQ (waiting.error_kind (), framework_error_kind_t::unavailable);
        clock.advance (15s - 1ms);
        // A provider repository can restart over the same Store. The in-memory fixture
        // keeps Store data and discards only the observer's local monotonic history.
        provider_location_repository_t restarted (opaque);
        restarted.configure_reclaim_margin (5s);
        location_repository_t &observer =
          std::get<0> (GetParam ()) ? static_cast<location_repository_t &> (restarted) : repository;
        if (!std::get<0> (GetParam ()))
            location_reclaim_test_access_t::restart_observer (observer);
        clock.bind (observer);
        const auto first_observation = observer.reserve (request).result ();
        ASSERT_FALSE (first_observation);
        EXPECT_EQ (first_observation.error_kind (), framework_error_kind_t::unavailable);
        clock.advance (1ms);
        const auto old_boundary = observer.reserve (request).result ();
        ASSERT_FALSE (old_boundary);
        EXPECT_EQ (old_boundary.error_kind (), framework_error_kind_t::unavailable);
        const auto retained = std::get<authority_snapshot_t> (
          observer.read_authority (actor_authority_key (request.key.global_id)).result ().value ());
        EXPECT_EQ (retained.store_version, before.store_version);
        EXPECT_EQ (retained.payload, before.payload);
        clock.advance (15s - 1ms);
        const auto reclaimed = observer.reserve (request).result ();
        ASSERT_TRUE (reclaimed);
        ASSERT_TRUE (std::holds_alternative<object_reserved_t> (reclaimed.value ()));
        EXPECT_GT (std::get<object_reserved_t> (reclaimed.value ()).creating.object_generation,
                   before.object_generation);
        return;
    }
    if (scenario == "expired" || scenario == "race") {
        const auto waiting = repository.reserve (request).result ();
        ASSERT_FALSE (waiting);
        EXPECT_EQ (waiting.error_kind (), framework_error_kind_t::unavailable);
        const auto unchanged = std::get<authority_snapshot_t> (
          repository.read_authority (actor_authority_key (request.key.global_id))
            .result ()
            .value ());
        EXPECT_EQ (unchanged.object_generation, first.creating.object_generation);
        clock.advance (15s - 1ms);
        const auto before_boundary = repository.reserve (request).result ();
        ASSERT_FALSE (before_boundary);
        EXPECT_EQ (before_boundary.error_kind (), framework_error_kind_t::unavailable);
        clock.advance (1ms);
    }
    if (scenario == "race") {
        task_completion_source_t<void> owner_read_held;
        task_completion_source_t<void> release_owner_read;
        if (std::get<0> (GetParam ())) {
            opaque.before_read_key = {std::string ("owner-lease") + '\0' + old_owner.owner_id};
            opaque.before_read = [&] () -> task_t<void> {
                owner_read_held.complete (result_t<void>::success ());
                co_await release_owner_read.task ();
            };
        }
        auto first_attempt =
          std::async (std::launch::async, [&] { return repository.reserve (request).result (); });
        struct release_on_exit_t
        {
            task_completion_source_t<void> &release;
            ~release_on_exit_t () { release.complete (result_t<void>::success ()); }
        } release_on_exit{release_owner_read};
        if (std::get<0> (GetParam ()))
            owner_read_held.task ().result ().value ();
        auto second_attempt =
          std::async (std::launch::async, [&] { return repository.reserve (request).result (); });
        const auto second_result = second_attempt.get ();
        release_owner_read.complete (result_t<void>::success ());
        const auto first_result = first_attempt.get ();
        ASSERT_TRUE (first_result)
          << (first_result.error () ? first_result.error ()->what () : "untyped failure");
        ASSERT_TRUE (second_result)
          << (second_result.error () ? second_result.error ()->what () : "untyped failure");
        EXPECT_EQ (
          static_cast<int> (std::holds_alternative<object_reserved_t> (first_result.value ()))
            + static_cast<int> (std::holds_alternative<object_reserved_t> (second_result.value ())),
          1);
        return;
    }
    if (scenario == "recreate" || scenario == "relocation" || scenario == "relocationRecreate"
        || scenario == "notReady") {
        const auto before = std::get<authority_snapshot_t> (
          repository.read_authority (actor_authority_key (request.key.global_id))
            .result ()
            .value ());
        const auto next = repository.reserve (request).result ();
        ASSERT_FALSE (next);
        EXPECT_EQ (next.error_kind (), framework_error_kind_t::unavailable);
        const auto after = std::get<authority_snapshot_t> (
          repository.read_authority (actor_authority_key (request.key.global_id))
            .result ()
            .value ());
        EXPECT_EQ (before.store_version, after.store_version);
        EXPECT_EQ (before.payload, after.payload);
        return;
    }
    const auto retained_before =
      repository.read_authority (actor_authority_key (request.key.global_id)).result ().value ();
    const auto next = repository.reserve (request).result ();
    ASSERT_TRUE (next);
    if (scenario == "live" || scenario == "liveDescriptorGone")
        EXPECT_TRUE (std::holds_alternative<object_already_exists_t> (next.value ()));
    else if (scenario == "typeMismatch")
        EXPECT_TRUE (std::holds_alternative<object_type_mismatch_t> (next.value ()));
    else {
        const auto *created = std::get_if<object_reserved_t> (&next.value ());
        ASSERT_NE (created, nullptr);
        EXPECT_GT (created->creating.object_generation, first.creating.object_generation);
        EXPECT_EQ (created->creating.payload, request.creating_payload);
    }
}

INSTANTIATE_TEST_SUITE_P (RepositoryImplementations,
                          ActiveActorReclaimTest,
                          ::testing::Combine (::testing::Bool (),
                                              ::testing::Values ("expired",
                                                                 "observerRestart",
                                                                 "live",
                                                                 "liveDescriptorGone",
                                                                 "recreate",
                                                                 "typeMismatch",
                                                                 "race",
                                                                 "relocation",
                                                                 "relocationRecreate",
                                                                 "notReady",
                                                                 "delete")));

INSTANTIATE_TEST_SUITE_P (ProviderReadOrder,
                          ActiveActorReclaimTest,
                          ::testing::Values (std::make_tuple (true, "staleLiveRead"),
                                             std::make_tuple (true, "staleMissingRead"),
                                             std::make_tuple (true, "deleteMissing"),
                                             std::make_tuple (true, "lateDelete"),
                                             std::make_tuple (true, "concurrentReclaimDelete")));

class EndedSpotReclaimTest
    : public ::testing::TestWithParam<std::tuple<bool, placement_object_kind_t, std::string>>
{
};

TEST_P (EndedSpotReclaimTest, NewIncarnationRequiresEndedOwnerAndDisabledRegistration)
{
    reclaim_scan_store_t opaque;
    provider_location_repository_t provider (opaque);
    provider.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    in_memory_location_repository_t memory;
    memory.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    location_repository_t &repository = std::get<0> (GetParam ())
                                          ? static_cast<location_repository_t &> (provider)
                                          : static_cast<location_repository_t &> (memory);
    const auto kind = std::get<1> (GetParam ());
    location_reclaim_test_access_t clock;
    clock.bind (repository);
    const auto scenario = std::get<2> (GetParam ());
    const auto old_owner =
      std::get<owner_lease_claimed_t> (
        repository.claim_owner_lease ("ended-spot-old", 60s).result ().value ())
        .token;
    const auto new_owner =
      std::get<owner_lease_claimed_t> (
        repository.claim_owner_lease ("ended-spot-new", 60s).result ().value ())
        .token;
    const auto publish = [&] (const std::string &rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "ended-spot-mesh";
        descriptor.rid = zlink::routing_id_t::from (rid);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.application_version = 1;
        descriptor.security_identity = rid;
        descriptor.activation_concurrency.limit = 1;
        descriptor.entry_spot_id = "entry-" + rid;
        descriptor.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                                   maintenance_policy_kind_t::disabled, false, 0});
        const bool check_target =
          kind == placement_object_kind_t::user_spot && rid == "ended-spot-target";
        if (!check_target || scenario != "unregistered") {
            const auto policy = check_target && scenario == "recreate"
                                  ? maintenance_policy_kind_t::recreate
                                  : maintenance_policy_kind_t::disabled;
            descriptor.object_capabilities.push_back ({kind, "room", policy, false, 0});
            descriptor.capacity.spot_types.push_back ({kind, "room", {1, 0, 0}});
        }
        descriptor.capacity.actors.limit = 2;
        descriptor.capacity.spots.limit = 1;
        ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
    };
    publish ("ended-spot-source", old_owner);
    publish ("ended-spot-target", new_owner);
    repository.configure_reclaim_margin (5s);
    object_reserve_request_t request;
    request.key = {kind, "ended-spot"};
    request.intent.stable_type = "room";
    request.target = {"ended-spot-mesh", node_rid_t::from_string ("ended-spot-source"), 1,
                      old_owner};
    request.creating_payload = bytes ("creating");
    request.capacity_bundle = {0, 1, spot_type_capacity_delta_t{kind, "room", 1}};
    if (kind == placement_object_kind_t::instance_spot)
        request.relocation_policy = detail::factory_relocation_kind_t::disabled;
    const auto first =
      std::get<object_reserved_t> (repository.reserve (request).result ().value ());
    const auto ready =
      kind == placement_object_kind_t::user_spot
        ? encode_user_spot_authority_payload (user_spot_authority_payload_t{
            .state = scenario == "closing" ? user_spot_authority_state_t::closing
                                           : user_spot_authority_state_t::ready,
            .stable_type = "room",
            .spot_id = "ended-spot",
            .owner_id = old_owner.owner_id,
            .owner_lease_generation = static_cast<std::uint64_t> (old_owner.lease_generation),
            .mesh_name = request.target.mesh_name,
            .node_rid = request.target.node_rid,
            .node_generation = 1})
        : encode_instance_spot_authority_payload (instance_spot_authority_payload_t{
            .state = scenario == "closing" ? instance_spot_authority_state_t::closing
                                           : instance_spot_authority_state_t::ready,
            .stable_type = "room",
            .spot_id = "ended-spot",
            .owner_id = old_owner.owner_id,
            .owner_lease_generation = static_cast<std::uint64_t> (old_owner.lease_generation),
            .mesh_name = request.target.mesh_name,
            .node_rid = request.target.node_rid,
            .node_generation = 1,
            .activation_recovery =
              scenario == "recovery"
                ? std::optional<activation_recovery_pointer_t>{{.reference = "old-root",
                                                                .encoded_size = 1,
                                                                .inbox_sequence = 1}}
                : std::nullopt});
    ASSERT_TRUE (std::holds_alternative<object_committed_t> (
      repository.commit ({request.key, first.fence, ready}).result ().value ()));
    const auto before = std::get<authority_snapshot_t> (
      repository.read_authority (spot_authority_key ("ended-spot")).result ().value ());
    if (scenario == "pageMember" || scenario == "snapshotExpired") {
        object_reserve_request_t decoy;
        decoy.key = {placement_object_kind_t::actor, "a-first-page"};
        decoy.intent.stable_type = "player";
        decoy.target = {"ended-spot-mesh", node_rid_t::from_string ("ended-spot-target"), 1,
                        new_owner};
        decoy.capacity_bundle.actor_slots = 1;
        const auto reserved =
          std::get<object_reserved_t> (repository.reserve (decoy).result ().value ());
        actor_authority_payload_t payload{.stable_type = "player",
                                          .actor_id = "a-first-page",
                                          .current_spot_id = "entry",
                                          .current_spot_generation = 1,
                                          .current_spot_kind = actor_authority_spot_kind_t::entry,
                                          .owner_id = new_owner.owner_id,
                                          .owner_lease_generation =
                                            static_cast<std::uint64_t> (new_owner.lease_generation),
                                          .mesh_name = decoy.target.mesh_name,
                                          .node_rid = decoy.target.node_rid,
                                          .node_generation = 1};
        ASSERT_TRUE (std::holds_alternative<object_committed_t> (
          repository.commit ({decoy.key, reserved.fence, encode_actor_authority_payload (payload)})
            .result ()
            .value ()));
    }
    std::optional<authority_snapshot_t> member_before;
    std::optional<object_reservation_fence_t> member_fence;
    std::vector<std::byte> joined_payload;
    if (scenario == "member" || scenario == "lateJoin" || scenario == "pageMember"
        || scenario == "lateJoinCommit" || scenario == "lateJoinVersion"
        || scenario == "initialLateJoin") {
        object_reserve_request_t member_request;
        member_request.key = {placement_object_kind_t::actor, "ended-spot-member"};
        member_request.intent.stable_type = "player";
        member_request.target = {"ended-spot-mesh", node_rid_t::from_string ("ended-spot-target"),
                                 1, new_owner};
        member_request.capacity_bundle.actor_slots = 1;
        const auto reserved =
          std::get<object_reserved_t> (repository.reserve (member_request).result ().value ());
        actor_authority_payload_t member{.stable_type = "player",
                                         .actor_id = "ended-spot-member",
                                         .current_spot_id = "entry",
                                         .current_spot_generation = 1,
                                         .current_spot_kind = actor_authority_spot_kind_t::entry,
                                         .owner_id = new_owner.owner_id,
                                         .owner_lease_generation =
                                           static_cast<std::uint64_t> (new_owner.lease_generation),
                                         .mesh_name = member_request.target.mesh_name,
                                         .node_rid = member_request.target.node_rid,
                                         .node_generation = 1};
        member_fence = reserved.fence;
        if (scenario != "initialLateJoin") {
            ASSERT_TRUE (std::holds_alternative<object_committed_t> (
              repository
                .commit (
                  {member_request.key, reserved.fence, encode_actor_authority_payload (member)})
                .result ()
                .value ()));
        }
        member.current_spot_id = "ended-spot";
        member.current_spot_generation = before.object_generation;
        member.current_spot_kind = actor_authority_spot_kind_t::user;
        joined_payload = encode_actor_authority_payload (member);
        member_before = std::get<authority_snapshot_t> (
          repository.read_authority (actor_authority_key (member.actor_id)).result ().value ());
        if (scenario == "member" || scenario == "pageMember") {
            ASSERT_TRUE (std::holds_alternative<authority_stored_t> (
              repository
                .compare_exchange_authority (actor_authority_key (member.actor_id),
                                             member_before->store_version,
                                             authority_put_t{joined_payload})
                .result ()
                .value ()));
        }
    }
    if (scenario == "aggregate") {
        aggregate_prepare_request_t aggregate;
        aggregate.aggregate_id.value[0] = std::byte{1};
        aggregate.aggregate_generation = 1;
        aggregate.participants = {{spot_authority_key ("ended-spot"),
                                   before.store_version,
                                   authority_generation_transition_t::new_owner,
                                   ready,
                                   {}}};
        aggregate.target_descriptor = {"ended-spot-mesh",
                                       zlink::routing_id_t::from ("ended-spot-target")};
        aggregate.target_descriptor_lifecycle_generation = 1;
        aggregate.target_owner = new_owner;
        aggregate.capacity_bundle = request.capacity_bundle;
        ASSERT_TRUE (std::holds_alternative<aggregate_prepared_t> (
          repository.prepare_aggregate (aggregate).result ().value ()));
    }
    if (scenario != "live" && scenario != "lateJoinCommit" && scenario != "lateJoinVersion")
        repository.release_owner_lease (old_owner).result ().value ();
    request.target = {"ended-spot-mesh", node_rid_t::from_string ("ended-spot-target"), 1,
                      new_owner};
    if (kind == placement_object_kind_t::instance_spot) {
        if (scenario == "recreate")
            request.relocation_policy = detail::factory_relocation_kind_t::recreate;
        if (scenario == "unregistered")
            request.relocation_policy.reset ();
    }
    if (scenario == "lateJoinCommit" || scenario == "lateJoinVersion") {
        opaque.before_member_write = [&] () -> task_t<void> {
            if (scenario == "lateJoinCommit")
                co_await repository.release_owner_lease (old_owner);
            else {
                auto closing = *decode_ready_user_spot_authority_payload (ready);
                closing.state = user_spot_authority_state_t::closing;
                const auto stored = co_await repository.compare_exchange_authority (
                  spot_authority_key ("ended-spot"), before.store_version,
                  authority_put_t{encode_user_spot_authority_payload (closing)});
                EXPECT_TRUE (std::holds_alternative<authority_stored_t> (stored));
            }
        };
    }
    if (scenario == "lateJoin" || scenario == "lateJoinCommit" || scenario == "lateJoinVersion") {
        const auto joined = repository
                              .compare_exchange_authority (
                                actor_authority_key ("ended-spot-member"),
                                member_before->store_version, authority_put_t{joined_payload})
                              .result ()
                              .value ();
        EXPECT_TRUE (std::holds_alternative<authority_conflict_t> (joined));
        const auto retained = std::get<authority_snapshot_t> (
          repository.read_authority (actor_authority_key ("ended-spot-member")).result ().value ());
        EXPECT_EQ (retained.store_version, member_before->store_version);
        EXPECT_EQ (retained.payload, member_before->payload);
        return;
    }
    if (scenario == "initialLateJoin") {
        const auto committed = repository
                                 .commit ({{placement_object_kind_t::actor, "ended-spot-member"},
                                           *member_fence,
                                           joined_payload})
                                 .result ()
                                 .value ();
        EXPECT_TRUE (std::holds_alternative<object_commit_conflict_t> (committed));
        const auto after = std::get<authority_snapshot_t> (
          repository.read_authority (actor_authority_key ("ended-spot-member")).result ().value ());
        EXPECT_EQ (after.store_version, member_before->store_version);
        EXPECT_EQ (after.payload, member_before->payload);
        EXPECT_EQ (after.allocation.state, placement_allocation_state_t::reserved);
        ASSERT_TRUE (after.pending_creation);
        EXPECT_EQ (after.pending_creation->reservation_id, member_fence->reservation_id);
        return;
    }
    if (scenario != "live" && scenario != "typeMismatch" && scenario != "kindMismatch") {
        const auto waiting = repository.reserve (request).result ();
        ASSERT_FALSE (waiting);
        EXPECT_EQ (waiting.error_kind (), framework_error_kind_t::unavailable);
        if (scenario == "closing" || scenario == "aggregate" || scenario == "recreate"
            || scenario == "unregistered"
            || (scenario == "recovery" && kind == placement_object_kind_t::instance_spot))
            EXPECT_EQ (location_reclaim_test_access_t::observations (repository), 0u);
        clock.advance (15s);
    }
    opaque.armed = true;
    opaque.fail_scan = scenario == "storeFailure";
    opaque.expire_next_page = scenario == "snapshotExpired";
    if (scenario == "typeMismatch") {
        request.intent.stable_type = "different";
        request.capacity_bundle.spot_type->stable_type = "different";
    }
    if (scenario == "kindMismatch")
        request.key.kind = kind == placement_object_kind_t::user_spot
                             ? placement_object_kind_t::instance_spot
                             : placement_object_kind_t::user_spot;
    if (scenario == "kindMismatch")
        request.capacity_bundle.spot_type->object_kind = request.key.kind;
    if (scenario == "typeMismatch" || scenario == "kindMismatch") {
        const auto stale =
          repository
            .release_ended_reservation (spot_authority_key ("ended-spot"),
                                        before.store_version + "-stale", {}, request)
            .result ();
        ASSERT_TRUE (stale);
        EXPECT_FALSE (stale.value ());
        const auto mismatched = repository
                                  .release_ended_reservation (spot_authority_key ("ended-spot"),
                                                              before.store_version, {}, request)
                                  .result ();
        ASSERT_FALSE (mismatched);
        EXPECT_EQ (framework_error_kind_t::type_mismatch, mismatched.error_kind ());
    } else if (scenario == "live") {
        const auto live = repository
                            .release_ended_reservation (spot_authority_key ("ended-spot"),
                                                        before.store_version, {}, request)
                            .result ();
        ASSERT_TRUE (live);
        EXPECT_FALSE (live.value ());
    } else if (scenario == "aggregate" || scenario == "recreate" || scenario == "unregistered"
               || scenario == "closing"
               || (scenario == "recovery" && kind == placement_object_kind_t::instance_spot)) {
        const auto denied = repository
                              .release_ended_reservation (spot_authority_key ("ended-spot"),
                                                          before.store_version, {}, request)
                              .result ();
        ASSERT_FALSE (denied);
        EXPECT_EQ (framework_error_kind_t::unavailable, denied.error_kind ());
    }
    const auto result = [&] {
        if (scenario != "race")
            return repository.reserve (request).result ();
        auto first =
          std::async (std::launch::async, [&] { return repository.reserve (request).result (); });
        auto second =
          std::async (std::launch::async, [&] { return repository.reserve (request).result (); });
        auto one = first.get ();
        auto two = second.get ();
        EXPECT_TRUE (one);
        EXPECT_TRUE (two);
        if (!one || !two)
            return one;
        const bool one_won = std::holds_alternative<object_reserved_t> (one.value ());
        const bool two_won = std::holds_alternative<object_reserved_t> (two.value ());
        EXPECT_EQ (static_cast<int> (one_won) + static_cast<int> (two_won), 1);
        return one_won ? one : two;
    }();
    if (scenario == "expired" || scenario == "snapshotExpired" || scenario == "race"
        || (scenario == "recovery" && kind == placement_object_kind_t::user_spot)) {
        ASSERT_TRUE (result);
        const auto *winner = std::get_if<object_reserved_t> (&result.value ());
        ASSERT_NE (winner, nullptr);
        EXPECT_GT (winner->creating.object_generation, before.object_generation);
        EXPECT_NE (winner->creating.payload, ready);
        EXPECT_EQ (winner->creating.owner.owner_id, new_owner.owner_id);
        const auto old_capacity = repository.list_mesh_nodes ("ended-spot-mesh").result ().value ();
        const auto &nodes = old_capacity.items;
        const auto source = std::find_if (nodes.begin (), nodes.end (), [] (const auto &node) {
            return node.rid.to_string () == "ended-spot-source";
        });
        ASSERT_NE (source, nodes.end ());
        EXPECT_EQ (source->capacity.spots.active, 0u);
        if (scenario == "snapshotExpired") {
            EXPECT_EQ (opaque.expired, 1u);
            EXPECT_GE (opaque.scans, 4u);
        }
    } else {
        if (scenario == "typeMismatch" || scenario == "kindMismatch") {
            ASSERT_TRUE (result);
            EXPECT_TRUE (std::holds_alternative<object_type_mismatch_t> (result.value ()));
        } else if (scenario == "live") {
            ASSERT_TRUE (result);
            EXPECT_TRUE (std::holds_alternative<object_already_exists_t> (result.value ()));
        } else {
            ASSERT_FALSE (result);
            EXPECT_EQ (result.error_kind (), framework_error_kind_t::unavailable);
        }
        const auto after = std::get<authority_snapshot_t> (
          repository.read_authority (spot_authority_key ("ended-spot")).result ().value ());
        EXPECT_EQ (after.store_version, before.store_version);
        EXPECT_EQ (after.payload, before.payload);
        if (scenario == "pageMember")
            EXPECT_GE (opaque.scans, 2u);
    }
}

INSTANTIATE_TEST_SUITE_P (
  ProviderAndInMemory, EndedSpotReclaimTest, ::testing::ValuesIn ([] {
      std::vector<std::tuple<bool, placement_object_kind_t, std::string>> inputs;
      for (const bool provider : {false, true}) {
          for (const auto kind :
               {placement_object_kind_t::user_spot, placement_object_kind_t::instance_spot})
              for (const auto scenario : {"expired", "live", "recreate", "unregistered", "closing",
                                          "recovery", "race", "typeMismatch", "kindMismatch"})
                  inputs.emplace_back (provider, kind, scenario);
          for (const auto scenario : {"member", "lateJoin", "initialLateJoin", "aggregate"})
              inputs.emplace_back (provider, placement_object_kind_t::user_spot, scenario);
      }
      for (const auto scenario :
           {"pageMember", "snapshotExpired", "storeFailure", "lateJoinCommit", "lateJoinVersion"})
          inputs.emplace_back (true, placement_object_kind_t::user_spot, scenario);
      return inputs;
  }()));

class EndedSpotMemberReleaseTest : public ::testing::TestWithParam<std::tuple<bool, std::string>>
{
};

// Location runtime §6.1: an ended User Spot whose remaining members all have ended owners is
// released together with those members; any other member keeps everything unchanged.
TEST_P (EndedSpotMemberReleaseTest, ReleasesOnlyEndedOwnerMembers)
{
    reclaim_scan_store_t opaque;
    provider_location_repository_t provider (opaque);
    provider.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    in_memory_location_repository_t memory;
    memory.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const bool use_provider = std::get<0> (GetParam ());
    location_repository_t &repository = use_provider
                                          ? static_cast<location_repository_t &> (provider)
                                          : static_cast<location_repository_t &> (memory);
    location_reclaim_test_access_t clock;
    clock.bind (repository);
    const auto scenario = std::get<1> (GetParam ());
    const bool member_only_race =
      scenario == "memberOnlyAggregatePrepared" || scenario == "memberOnlyAggregateReplaced";
    const bool aggregate_race = scenario == "memberAggregatePrepared" || member_only_race;
    const bool separate_aggregate_spot = scenario == "aggregate" || member_only_race;
    // Shared owner/node: Spot authority and lock, owner lease, descriptor and capacity,
    // plus each member's authority and lock. The nearest fitting batch has 2,047 keys;
    // one more member exceeds the Store's 2,048 unique-key limit (§4).
    const auto fitting_members = (location_store_limits::write_keys - 5u) / 2u;
    const std::size_t members_wanted = scenario == "batchFits"    ? fitting_members
                                       : scenario == "batchBound" ? fitting_members + 1u
                                                                  : 1u;
    const auto claim = [&] (const std::string &owner_id) {
        return std::get<owner_lease_claimed_t> (
                 repository.claim_owner_lease (owner_id, 60s).result ().value ())
          .token;
    };
    const auto old_owner = claim ("member-spot-old");
    const auto new_owner = claim ("member-spot-new");
    const auto publish = [&] (const std::string &rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "member-spot-mesh";
        descriptor.rid = zlink::routing_id_t::from (rid);
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7001";
        descriptor.owner_id = owner.owner_id;
        descriptor.lease_generation = owner.lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.application_version = 1;
        descriptor.security_identity = rid;
        descriptor.activation_concurrency.limit = 1;
        descriptor.entry_spot_id = "entry-" + rid;
        descriptor.object_capabilities.push_back ({placement_object_kind_t::actor, "player",
                                                   maintenance_policy_kind_t::disabled, false, 0});
        descriptor.object_capabilities.push_back ({placement_object_kind_t::user_spot, "room",
                                                   maintenance_policy_kind_t::disabled, false, 0});
        descriptor.capacity.actors.limit = static_cast<std::int32_t> (members_wanted + 2);
        descriptor.capacity.spots.limit = separate_aggregate_spot ? 2 : 1;
        descriptor.capacity.spot_types.push_back (
          {placement_object_kind_t::user_spot, "room", {separate_aggregate_spot ? 2u : 1u, 0, 0}});
        ASSERT_EQ (repository.update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
    };
    publish ("member-spot-source", old_owner);
    publish ("member-spot-target", new_owner);
    if (aggregate_race)
        publish ("member-spot-aggregate-target", claim ("member-aggregate-target"));
    const object_creation_target_t source{
      "member-spot-mesh", node_rid_t::from_string ("member-spot-source"), 1, old_owner};
    const object_creation_target_t target{
      "member-spot-mesh", node_rid_t::from_string ("member-spot-target"), 1, new_owner};
    repository.configure_reclaim_margin (5s);
    object_reserve_request_t request;
    request.key = {placement_object_kind_t::user_spot, "member-spot"};
    request.intent.stable_type = "room";
    request.target = source;
    request.creating_payload = bytes ("creating");
    request.capacity_bundle = {
      0, 1, spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1}};
    const auto first =
      std::get<object_reserved_t> (repository.reserve (request).result ().value ());
    ASSERT_TRUE (std::holds_alternative<object_committed_t> (
      repository
        .commit (
          {request.key, first.fence,
           encode_user_spot_authority_payload (user_spot_authority_payload_t{
             .state = user_spot_authority_state_t::ready,
             .stable_type = "room",
             .spot_id = "member-spot",
             .owner_id = old_owner.owner_id,
             .owner_lease_generation = static_cast<std::uint64_t> (old_owner.lease_generation),
             .mesh_name = source.mesh_name,
             .node_rid = source.node_rid,
             .node_generation = 1})})
        .result ()
        .value ()));
    const auto spot = std::get<authority_snapshot_t> (
      repository.read_authority (spot_authority_key ("member-spot")).result ().value ());
    const auto member_payload = [&] (const std::string &actor_id,
                                     const object_creation_target_t &placement,
                                     actor_authority_state_t state) {
        return encode_actor_authority_payload (actor_authority_payload_t{
          .state = state,
          .stable_type = "player",
          .actor_id = actor_id,
          .current_spot_id = "member-spot",
          .current_spot_generation = spot.object_generation,
          .current_spot_kind = actor_authority_spot_kind_t::user,
          .owner_id = placement.owner.owner_id,
          .owner_lease_generation = static_cast<std::uint64_t> (placement.owner.lease_generation),
          .mesh_name = placement.mesh_name,
          .node_rid = placement.node_rid,
          .node_generation = 1});
    };
    std::vector<std::string> member_ids;
    const auto add_member = [&] (const std::string &actor_id,
                                 const object_creation_target_t &placement, bool activated,
                                 bool relocating = false) {
        object_reserve_request_t member;
        member.key = {placement_object_kind_t::actor, actor_id};
        member.intent.stable_type = "player";
        member.target = placement;
        member.capacity_bundle.actor_slots = 1;
        member.creating_payload =
          activated ? bytes ("creating")
                    : member_payload (actor_id, placement, actor_authority_state_t::creating);
        const auto reserved =
          std::get<object_reserved_t> (repository.reserve (member).result ().value ());
        member_ids.push_back (actor_id);
        if (!activated)
            return;
        auto ready = member_payload (actor_id, placement, actor_authority_state_t::ready);
        if (relocating) {
            std::ifstream fixture (ZLINK_AUTHORITY_RELOCATION_STATE_GOLDEN_PATH);
            const auto golden = nlohmann::json::parse (fixture);
            const auto slot =
              tests::from_hex (golden.at ("valid").at (0).at ("hex").get<std::string> ());
            ready = tests::with_relocation_slot (ready, {slot.begin () + 5, slot.end ()});
        }
        ASSERT_TRUE (std::holds_alternative<object_committed_t> (
          repository.commit ({member.key, reserved.fence, ready}).result ().value ()));
    };
    add_member ("member-a", source, true);
    if (scenario == "live")
        add_member ("member-live", target, true);
    if (scenario == "moving")
        add_member ("member-moving", source, true, true);
    if (scenario == "unactivated")
        add_member ("member-unactivated", source, false);
    std::optional<authority_snapshot_t> separate_spot;
    if (separate_aggregate_spot) {
        // A member can be locked by an aggregate other than this Spot's aggregate.
        // Include a separate Spot so the member lock is the rejection being tested.
        auto aggregate_spot_request = request;
        aggregate_spot_request.key.global_id = "aggregate-spot";
        const auto reserved = std::get<object_reserved_t> (
          repository.reserve (aggregate_spot_request).result ().value ());
        auto aggregate_spot_payload = *decode_ready_user_spot_authority_payload (spot.payload);
        aggregate_spot_payload.spot_id = aggregate_spot_request.key.global_id;
        const auto committed = std::get<object_committed_t> (
          repository
            .commit ({aggregate_spot_request.key, reserved.fence,
                      encode_user_spot_authority_payload (aggregate_spot_payload)})
            .result ()
            .value ());
        separate_spot = committed.ready;
        if (scenario == "aggregate") {
            const auto member = std::get<authority_snapshot_t> (
              repository.read_authority (actor_authority_key ("member-a")).result ().value ());
            aggregate_prepare_request_t aggregate;
            aggregate.aggregate_id.value[0] = std::byte{2};
            aggregate.aggregate_generation = 1;
            aggregate.participants = {{actor_authority_key ("member-a"),
                                       member.store_version,
                                       authority_generation_transition_t::new_owner,
                                       member.payload,
                                       {}},
                                      {spot_authority_key (aggregate_spot_request.key.global_id),
                                       committed.ready.store_version,
                                       authority_generation_transition_t::new_owner,
                                       committed.ready.payload,
                                       {}}};
            aggregate.target_descriptor = {"member-spot-mesh",
                                           zlink::routing_id_t::from ("member-spot-target")};
            aggregate.target_descriptor_lifecycle_generation = 1;
            aggregate.target_owner = new_owner;
            aggregate.capacity_bundle.actor_slots = 1;
            aggregate.capacity_bundle.spot_slots = 1;
            aggregate.capacity_bundle.spot_type = request.capacity_bundle.spot_type;
            ASSERT_TRUE (std::holds_alternative<aggregate_prepared_t> (
              repository.prepare_aggregate (aggregate).result ().value ()));
        }
    }
    for (std::size_t index = 0; member_ids.size () < members_wanted; ++index)
        add_member ("member-bulk-" + std::to_string (index), source, true);
    const auto versions = [&] {
        std::vector<std::string> values;
        for (const auto &id : member_ids) {
            const auto read =
              repository.read_authority (actor_authority_key (id)).result ().value ();
            const auto *found = std::get_if<authority_snapshot_t> (&read);
            values.push_back (found ? found->store_version : std::string ("missing"));
        }
        return values;
    };
    const auto before = versions ();
    repository.release_owner_lease (old_owner).result ().value ();
    request.target = target;
    const auto waiting = repository.reserve (request).result ();
    ASSERT_FALSE (waiting);
    EXPECT_EQ (waiting.error_kind (), framework_error_kind_t::unavailable);
    EXPECT_EQ (before, versions ());
    clock.advance (15s);
    std::optional<object_reserved_t> independently_recreated;
    std::optional<store_found_t> corrupted_capacity;
    if (scenario == "memberReclaimed" || scenario == "memberCapacityCorrupt") {
        mesh_node_descriptor_t source_descriptor;
        source_descriptor.mesh_name = source.mesh_name;
        source_descriptor.rid = zlink::routing_id_t::from (std::string (source.node_rid.value ()));
        opaque.before_read_key = capacity_key (source_descriptor);
        if (scenario == "memberReclaimed") {
            const auto capacity = capacity_record (opaque.inner, source_descriptor);
            EXPECT_EQ (
              capacity.at (location_record_fields::active).at (location_record_fields::actors), 1);
            EXPECT_EQ (
              capacity.at (location_record_fields::active).at (location_record_fields::spots), 1);
            mesh_node_descriptor_t target_descriptor;
            target_descriptor.mesh_name = target.mesh_name;
            target_descriptor.rid =
              zlink::routing_id_t::from (std::string (target.node_rid.value ()));
            // No allocation has used this target. Missing capacity is zero pending usage
            // (provider read_capacity_async), before either new reservation is created.
            ASSERT_TRUE (std::holds_alternative<store_missing_t> (
              opaque.inner.read (capacity_key (target_descriptor)).result ().value ()));
        }
    }
    if (scenario == "memberCapacityCorrupt") {
        const auto current =
          std::get<store_found_t> (opaque.inner.read (opaque.before_read_key).result ().value ());
        auto capacity =
          nlohmann::json::parse (current.value.bytes.begin (), current.value.bytes.end ());
        capacity.at (location_record_fields::active).at (location_record_fields::actors) = 0;
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          opaque.inner
            .write ({{store_version_condition_t{opaque.before_read_key, current.value.version}},
                     {store_put_t{opaque.before_read_key, bytes (capacity.dump ()), std::nullopt}}})
            .result ()
            .value ()));
        corrupted_capacity =
          std::get<store_found_t> (opaque.inner.read (opaque.before_read_key).result ().value ());
    }
    if (scenario == "memberReclaimed") {
        // The Spot has already captured the old member in its snapshot when it reads
        // source capacity. Reclaim that member independently before returning capacity.
        opaque.before_read = [&] () -> task_t<void> {
            object_reserve_request_t concurrent;
            concurrent.key = {placement_object_kind_t::actor, "member-a"};
            concurrent.intent.stable_type = "player";
            concurrent.target = target;
            concurrent.creating_payload = bytes ("creating");
            concurrent.capacity_bundle.actor_slots = 1;
            independently_recreated =
              std::get<object_reserved_t> (co_await repository.reserve (concurrent));
        };
    }
    std::optional<aggregate_prepare_result_t> raced_aggregate;
    std::optional<store_found_t> source_capacity_before_aggregate;
    std::vector<std::pair<store_key_t, store_found_t>> aggregate_sources;
    if (aggregate_race) {
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = source.mesh_name;
        descriptor.rid = zlink::routing_id_t::from (std::string (source.node_rid.value ()));
        opaque.before_read_key = capacity_key (descriptor);
        source_capacity_before_aggregate =
          std::get<store_found_t> (opaque.inner.read (opaque.before_read_key).result ().value ());
        const auto member = std::get<authority_snapshot_t> (
          repository.read_authority (actor_authority_key ("member-a")).result ().value ());
        // Canonical authority preimages are public Store identity (§3.4).
        for (const auto &key :
             {store_key_t{std::string ("authority") + '\0' + "actor" + '\0' + "member-a"},
              store_key_t{std::string ("authority") + '\0' + "spot" + '\0' + "member-spot"}})
            aggregate_sources.emplace_back (
              key, std::get<store_found_t> (opaque.inner.read (key).result ().value ()));
        aggregate_prepare_request_t aggregate;
        aggregate.aggregate_id.value[0] = std::byte{3};
        aggregate.aggregate_generation = 1;
        aggregate.participants = {
          {actor_authority_key ("member-a"),
           member.store_version,
           authority_generation_transition_t::new_owner,
           member.payload,
           {}},
          {spot_authority_key (member_only_race ? "aggregate-spot" : "member-spot"),
           member_only_race ? separate_spot->store_version : spot.store_version,
           authority_generation_transition_t::new_owner,
           member_only_race ? separate_spot->payload : spot.payload,
           {}}};
        const auto nodes = repository.list_mesh_nodes (source.mesh_name).result ().value ().items;
        const auto node = std::find_if (nodes.begin (), nodes.end (), [] (const auto &value) {
            return value.rid.to_string () == "member-spot-aggregate-target";
        });
        ASSERT_NE (node, nodes.end ());
        aggregate.target_descriptor = {node->mesh_name, node->rid};
        aggregate.target_descriptor_lifecycle_generation = node->lifecycle_generation;
        aggregate.target_owner = {node->owner_id, node->lease_generation};
        aggregate.capacity_bundle = {
          1, 1, spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1}};
        if (scenario == "memberOnlyAggregateReplaced") {
            // Persisted committed aggregate locks may remain until a later prepare replaces
            // them. Build canonical records through prepare, release its pending capacity,
            // then seed that terminal Store snapshot without changing either authority.
            const auto prepared = repository.prepare_aggregate (aggregate).result ().value ();
            const auto *fence = std::get_if<aggregate_prepared_t> (&prepared);
            ASSERT_NE (fence, nullptr);
            std::vector<store_scan_item_t> terminal_records;
            for (const auto prefix : {"zlink:v11:aggregate:", "zlink:v11:aggregate-lock:"}) {
                const auto page = std::get<store_scan_page_t> (
                  opaque.inner.scan ({prefix, std::nullopt, location_page_item_limit})
                    .result ()
                    .value ());
                ASSERT_FALSE (page.next_cursor);
                terminal_records.insert (terminal_records.end (), page.items.begin (),
                                         page.items.end ());
            }
            ASSERT_EQ (terminal_records.size (), 3u);
            ASSERT_EQ (repository.abort_aggregate (fence->fence).result ().value (),
                       aggregate_abort_result_t::aborted);
            for (const auto &record : terminal_records) {
                auto terminal =
                  nlohmann::json::parse (record.value.bytes.begin (), record.value.bytes.end ());
                terminal[location_record_fields::status] = aggregate_status::committed;
                ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
                  opaque.inner
                    .write ({{}, {store_put_t{record.key, bytes (terminal.dump ()), std::nullopt}}})
                    .result ()
                    .value ()));
            }
            aggregate.aggregate_id.value[0] = std::byte{4};
        }
        // Reclaim has checked both locks before its first source-capacity read.
        // Preparing on a separate live target must preserve both source participants.
        opaque.before_read = [&, aggregate] () -> task_t<void> {
            raced_aggregate = co_await repository.prepare_aggregate (aggregate);
        };
    }
    opaque.armed = scenario == "deadPage" || scenario == "deadExpired";
    opaque.expire_next_page = scenario == "deadExpired";
    request.target = target;
    const auto result = repository.reserve (request).result ();
    if (aggregate_race) {
        ASSERT_TRUE (raced_aggregate);
        ASSERT_TRUE (std::holds_alternative<aggregate_prepared_t> (*raced_aggregate));
        EXPECT_FALSE (result);
        if (!result)
            EXPECT_EQ (result.error_kind (), framework_error_kind_t::unavailable);
        for (const auto &[key, previous] : aggregate_sources) {
            const auto read = opaque.inner.read (key).result ().value ();
            const auto *unchanged = std::get_if<store_found_t> (&read);
            EXPECT_NE (unchanged, nullptr);
            if (unchanged) {
                EXPECT_EQ (unchanged->value.version.value, previous.value.version.value);
                EXPECT_EQ (unchanged->value.bytes, previous.value.bytes);
            }
        }
        const auto capacity =
          std::get<store_found_t> (opaque.inner.read (opaque.before_read_key).result ().value ());
        EXPECT_EQ (capacity.value.version.value,
                   source_capacity_before_aggregate->value.version.value);
        EXPECT_EQ (capacity.value.bytes, source_capacity_before_aggregate->value.bytes);
        return;
    }
    if (scenario == "dead" || scenario == "deadPage" || scenario == "deadExpired"
        || scenario == "batchFits" || scenario == "memberReclaimed") {
        ASSERT_TRUE (result) << (result.error () ? result.error ()->what () : "untyped failure");
        const auto *winner = std::get_if<object_reserved_t> (&result.value ());
        ASSERT_NE (winner, nullptr);
        EXPECT_GT (winner->creating.object_generation, spot.object_generation);
        if (scenario != "memberReclaimed")
            for (const auto &version : versions ())
                EXPECT_EQ (version, "missing");
        const auto nodes = repository.list_mesh_nodes ("member-spot-mesh").result ().value ().items;
        const auto old_node = std::find_if (nodes.begin (), nodes.end (), [] (const auto &node) {
            return node.rid.to_string () == "member-spot-source";
        });
        ASSERT_NE (old_node, nodes.end ());
        if (use_provider) {
            const auto capacity = capacity_record (opaque.inner, *old_node);
            EXPECT_EQ (
              capacity.at (location_record_fields::active).at (location_record_fields::actors), 0);
            EXPECT_EQ (
              capacity.at (location_record_fields::active).at (location_record_fields::spots), 0);
            EXPECT_EQ (
              capacity.at (location_record_fields::pending).at (location_record_fields::actors), 0);
            EXPECT_EQ (
              capacity.at (location_record_fields::pending).at (location_record_fields::spots), 0);
        } else {
            EXPECT_EQ (old_node->capacity.actors.active, 0u);
            EXPECT_EQ (old_node->capacity.spots.active, 0u);
        }
        if (scenario == "memberReclaimed") {
            ASSERT_TRUE (independently_recreated);
            const auto member = std::get<authority_snapshot_t> (
              repository.read_authority (actor_authority_key ("member-a")).result ().value ());
            EXPECT_EQ (member.store_version, independently_recreated->creating.store_version);
            EXPECT_EQ (member.object_generation,
                       independently_recreated->creating.object_generation);
            const auto new_node =
              std::find_if (nodes.begin (), nodes.end (), [] (const auto &node) {
                  return node.rid.to_string () == "member-spot-target";
              });
            ASSERT_NE (new_node, nodes.end ());
            const auto capacity = capacity_record (opaque.inner, *new_node);
            EXPECT_EQ (
              capacity.at (location_record_fields::pending).at (location_record_fields::actors), 1);
            EXPECT_EQ (
              capacity.at (location_record_fields::pending).at (location_record_fields::spots), 1);
            EXPECT_EQ (
              capacity.at (location_record_fields::active).at (location_record_fields::actors), 0);
            EXPECT_EQ (
              capacity.at (location_record_fields::active).at (location_record_fields::spots), 0);
            return;
        }
        if (scenario == "deadExpired")
            EXPECT_EQ (opaque.expired, 1u);
        // A released member is Missing: its next creation starts a new incarnation.
        opaque.armed = false;
        object_reserve_request_t again;
        again.key = {placement_object_kind_t::actor, "member-a"};
        again.intent.stable_type = "player";
        again.target = target;
        again.capacity_bundle.actor_slots = 1;
        again.creating_payload = bytes ("creating");
        const auto recreated = repository.reserve (again).result ();
        ASSERT_TRUE (recreated);
        const auto *member = std::get_if<object_reserved_t> (&recreated.value ());
        ASSERT_NE (member, nullptr);
        EXPECT_GT (member->creating.object_generation, spot.object_generation);
        return;
    }
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), scenario == "memberCapacityCorrupt"
                                       ? framework_error_kind_t::data_lost
                                       : framework_error_kind_t::unavailable);
    const auto after = std::get<authority_snapshot_t> (
      repository.read_authority (spot_authority_key ("member-spot")).result ().value ());
    EXPECT_EQ (after.store_version, spot.store_version);
    EXPECT_EQ (versions (), before);
    if (corrupted_capacity) {
        const auto capacity =
          std::get<store_found_t> (opaque.inner.read (opaque.before_read_key).result ().value ());
        EXPECT_EQ (capacity.value.version.value, corrupted_capacity->value.version.value);
        EXPECT_EQ (capacity.value.bytes, corrupted_capacity->value.bytes);
    }
}

INSTANTIATE_TEST_SUITE_P (ProviderAndInMemory, EndedSpotMemberReleaseTest, ::testing::ValuesIn ([] {
                              std::vector<std::tuple<bool, std::string>> inputs;
                              for (const bool provider : {false, true})
                                  for (const auto scenario :
                                       {"dead", "live", "moving", "aggregate", "unactivated",
                                        "batchBound", "batchFits"})
                                      inputs.emplace_back (provider, scenario);
                              inputs.emplace_back (true, "deadPage");
                              inputs.emplace_back (true, "deadExpired");
                              inputs.emplace_back (true, "memberReclaimed");
                              inputs.emplace_back (true, "memberCapacityCorrupt");
                              inputs.emplace_back (true, "memberAggregatePrepared");
                              inputs.emplace_back (true, "memberOnlyAggregatePrepared");
                              inputs.emplace_back (true, "memberOnlyAggregateReplaced");
                              return inputs;
                          }()));
}
