/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_observations.hpp"
#include "../support/actor_maintenance_join_host_access.hpp"
#include "../support/stateful_object_lane_test_access.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
using zlink::framework::test::begin_reserved_object;

// Occupy the object owner, then observe its mailbox before probing the index.
// The mailbox observation establishes ordering without a sleep or scheduler assumption.
using authority_interleaving_t =
  fw::runtime::host::public_host_runtime_test_access_t::authority_interleaving_t;

void verify_actor_authority_turns (bool adopt, authority_interleaving_t change)
{
    namespace host = fw::runtime::host;
    namespace sf = fw::runtime::stateful;
    using access_t = host::public_host_runtime_test_access_t;
    const auto rid = zlink::routing_id_t::from (first_rid);
    auto node = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      fw::test::runtime_failure_options (fw::runtime::mesh::raw_mesh_node_options_t{
        .descriptor = {mesh_name,
                       rid.to_bytes (),
                       1,
                       1,
                       "tcp://127.0.0.1:0",
                       {},
                       fw::runtime::mesh::service_node_state_t::preparing}})});
    node->start ();
    const sf::object_ref_t original{sf::object_kind_t::actor, actor_id, 1, 1, mesh_name,
                                    rid.to_string ()};
    ASSERT_TRUE (node->create_reserved_actor_async (actor_type, original).result ());
    auto committed = original;
    ++committed.authority_owner_generation;
    if (adopt)
        committed.node_id = second_rid;
    const bool erased = change == authority_interleaving_t::erased;
    const bool stale = change == authority_interleaving_t::superseded;
    const bool conflict = erased || stale;
    auto replacement = committed;
    if (stale)
        ++replacement.authority_owner_generation;

    auto release = access_t::hold_objects (*node);
    auto operation = std::async (std::launch::async, [&] {
        if (adopt) {
            const auto result = node->create_reserved_actor_async (actor_type, committed).result ();
            EXPECT_EQ (static_cast<bool> (result), !conflict);
            if (conflict && !result) {
                EXPECT_EQ (result.error ()->kind (), fw::framework_error_kind_t::internal_failure);
                EXPECT_STREQ (result.error ()->what (),
                              "reserved Actor generation does not match the local Actor");
            }
        } else {
            const auto result = access_t::advance_authority (*node, committed).result ();
            EXPECT_TRUE (result);
            if (result)
                EXPECT_EQ (result.value (), erased  ? sf::stateful_error_t::not_found
                                            : stale ? sf::stateful_error_t::generation_stale
                                                    : sf::stateful_error_t::none);
        }
    });
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    while (!sf::stateful_object_runtime_test_access_t::pending_work (node->objects ())
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    EXPECT_TRUE (sf::stateful_object_runtime_test_access_t::pending_work (node->objects ()));
    auto probe = access_t::probe_actor_index (*node, replacement, change);
    EXPECT_EQ (probe.wait_for (wait_budget), std::future_status::ready)
      << "Actor-index turn must end before waiting for the object owner";
    release.set_value ();
    const auto observed = probe.get ();
    if (erased)
        EXPECT_FALSE (observed);
    else
        EXPECT_EQ (observed,
                   change == authority_interleaving_t::unchanged ? original : replacement);
    operation.get ();
    const auto published =
      access_t::probe_actor_index (*node, replacement, authority_interleaving_t::unchanged).get ();
    if (erased)
        EXPECT_FALSE (published);
    else
        EXPECT_EQ (published, stale ? replacement : committed);
    node->close ();
}

TEST (ActorMaintenanceJoin, AuthorityAdoptionReleasesIndexWhileObjectLaneIsBusy)
{
    verify_actor_authority_turns (true, authority_interleaving_t::unchanged);
}

TEST (ActorMaintenanceJoin, AuthorityAdvancementReleasesIndexWhileObjectLaneIsBusy)
{
    verify_actor_authority_turns (false, authority_interleaving_t::unchanged);
}

TEST (ActorMaintenanceJoin, AuthorityAdoptionRejectsSupersededIndexGeneration)
{
    verify_actor_authority_turns (true, authority_interleaving_t::superseded);
}

TEST (ActorMaintenanceJoin, AuthorityAdvancementRejectsSupersededIndexGeneration)
{
    verify_actor_authority_turns (false, authority_interleaving_t::superseded);
}

TEST (ActorMaintenanceJoin, AuthorityAdoptionDoesNotReinsertErasedIdentity)
{
    verify_actor_authority_turns (true, authority_interleaving_t::erased);
}

TEST (ActorMaintenanceJoin, AuthorityAdvancementDoesNotReinsertErasedIdentity)
{
    verify_actor_authority_turns (false, authority_interleaving_t::erased);
}

TEST (ActorMaintenanceJoin, AuthorityAdoptionAcceptsDuplicatePublication)
{
    verify_actor_authority_turns (true, authority_interleaving_t::duplicate);
}

TEST (ActorMaintenanceJoin, AuthorityAdvancementAcceptsDuplicatePublication)
{
    verify_actor_authority_turns (false, authority_interleaving_t::duplicate);
}

TEST (ActorMaintenanceJoin, DestroyStatefulActorYieldsWhileObjectsLaneIsBusy)
{
    namespace host = fw::runtime::host;
    namespace sf = fw::runtime::stateful;
    using access_t = host::public_host_runtime_test_access_t;
    const auto rid = zlink::routing_id_t::from (first_rid);
    auto node = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      fw::test::runtime_failure_options (fw::runtime::mesh::raw_mesh_node_options_t{
        .descriptor = {mesh_name,
                       rid.to_bytes (),
                       1,
                       1,
                       "tcp://127.0.0.1:0",
                       {},
                       fw::runtime::mesh::service_node_state_t::preparing}})});
    node->start ();
    (void) node
      ->create_reserved_actor_async (
        actor_type, {sf::object_kind_t::actor, actor_id, 1, 1, mesh_name, rid.to_string ()})
      .result ()
      .value ();
    auto release = access_t::hold_objects (*node);
    fw::runtime::offload_executor_t executor (1, "destroy-submission");
    fw::runtime::state_lane_t source (executor);
    std::optional<fw::task_t<sf::stateful_error_t>> destroying;
    std::promise<void> yielded;
    auto submitted = yielded.get_future ();
    EXPECT_TRUE (source.try_post ([&] {
        destroying.emplace (node->destroy_application_actor_async (actor_id, 1));
        yielded.set_value ();
    }));
    submitted.get ();
    EXPECT_TRUE (destroying.has_value ());
    EXPECT_FALSE (destroying->await_ready ());
    release.set_value ();
    EXPECT_EQ (destroying->result ().value (), sf::stateful_error_t::none);
    node->close ();
}

enum class join_held_lane_t
{
    objects,
    dispatch,
    closing,
    stale_target
};
class ActorLocalJoinLane : public ::testing::TestWithParam<join_held_lane_t>
{
};

TEST_P (ActorLocalJoinLane, PendingSubmissionDoesNotHoldLifecycleTurn)
{
    namespace host = fw::runtime::host;
    using access_t = host::public_host_runtime_test_access_t;
    const auto rid = zlink::routing_id_t::from (first_rid);
    auto node = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      fw::test::runtime_failure_options (fw::runtime::mesh::raw_mesh_node_options_t{
        .descriptor = {mesh_name,
                       rid.to_bytes (),
                       1,
                       1,
                       "tcp://127.0.0.1:0",
                       {},
                       fw::runtime::mesh::service_node_state_t::preparing}})});
    node->start ();
    auto actor =
      node
        ->create_reserved_actor_async (actor_type, {fw::runtime::stateful::object_kind_t::actor,
                                                    actor_id, 1, 1, mesh_name, rid.to_string ()})
        .result ()
        .value ();
    const fw::runtime::stateful::object_ref_t target{
      fw::runtime::stateful::object_kind_t::user_spot,
      maintenance_spot,
      1,
      1,
      mesh_name,
      rid.to_string ()};
    const auto created = begin_reserved_object (node->objects (), target, room_type, {});
    ASSERT_EQ (created.error, fw::runtime::stateful::stateful_error_t::none);
    ASSERT_EQ (node->objects ().commit_create (created.attempt),
               fw::runtime::stateful::stateful_error_t::none);
    auto release = GetParam () == join_held_lane_t::dispatch ? access_t::hold_dispatch (*node)
                                                             : access_t::hold_objects (*node);
    host::pending_operation_t operation;
    auto generation = target.object_generation;
    if (GetParam () == join_held_lane_t::stale_target)
        ++generation;
    auto joining = access_t::join (actor, rid, target.key, generation, operation, wait_budget);
    EXPECT_FALSE (joining.await_ready ());
    // A pending lookup or dispatch must leave lifecycle progress available.
    // The fixture releases the occupied lane without a timer or another worker.
    if (GetParam () == join_held_lane_t::closing)
        node->close ();
    access_t::lifecycle_turn (*node, [&] { release.set_value (); });
    ASSERT_EQ (joining.result ().value (), GetParam () == join_held_lane_t::closing
                                             ? zlink::submit_result_t::terminated
                                             : zlink::submit_result_t::ok);
    if (GetParam () == join_held_lane_t::stale_target) {
        const auto completed = operation.completion->task ().result ();
        ASSERT_TRUE (completed);
        EXPECT_EQ (completed.value ().record.terminal_result,
                   static_cast<int> (fw::runtime::protocol::request_terminal_result::conflict));
        EXPECT_EQ (
          completed.value ().record.failure_errno,
          static_cast<int> (fw::runtime::protocol::framework_error_code::spotGenerationStale));
    }
    if (GetParam () == join_held_lane_t::closing)
        EXPECT_TRUE (node->objects ().find (fw::runtime::stateful::object_kind_t::actor, actor_id,
                                            fw::runtime::stateful::object_state_t::ready));
    node->close ();
}

INSTANTIATE_TEST_SUITE_P (OccupiedLane,
                          ActorLocalJoinLane,
                          ::testing::Values (join_held_lane_t::objects,
                                             join_held_lane_t::dispatch,
                                             join_held_lane_t::closing,
                                             join_held_lane_t::stale_target));

TEST (ActorMaintenanceJoin, LocalDirectAdmissionUsesCurrentActorId)
{
    namespace host = fw::runtime::host;
    namespace mesh = fw::runtime::mesh;
    namespace stateful = fw::runtime::stateful;
    constexpr std::uint64_t current_generation = 2;
    constexpr std::uint64_t stale_generation = 1;
    const auto rid = zlink::routing_id_t::from (first_rid);
    auto node = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{fw::test::runtime_failure_options (
        mesh::raw_mesh_node_options_t{.descriptor = {mesh_name,
                                                     rid.to_bytes (),
                                                     1,
                                                     1,
                                                     "tcp://127.0.0.1:0",
                                                     {},
                                                     mesh::service_node_state_t::preparing}})});
    node->start ();
    auto actor = node
                   ->create_reserved_actor_async (
                     actor_type, stateful::object_ref_t{stateful::object_kind_t::actor, actor_id,
                                                        current_generation, current_generation,
                                                        mesh_name, rid.to_string ()})
                   .result ()
                   .value ();
    const auto stale =
      host::public_host_runtime_t::remote_actor_ref (rid, actor_id, stale_generation);
    EXPECT_FALSE (node->resolve_actor_async (stale).result ().value ());
    const std::vector<zlink::message_t> parts{zlink::message_t::from ("local-admission")};
    const auto sent = node->send_to_actor (stale, parts).result ();
    ASSERT_TRUE (sent);
    ASSERT_EQ (sent.value (), zlink::submit_result_t::ok);
    bool delivered = false;
    ASSERT_TRUE (
      node
        ->dispatch_ready ([&] (const host::ready_record_t &owner, const host::receive_record_t &,
                               std::vector<zlink::message_t> received, auto) {
            ASSERT_TRUE (owner.actor);
            EXPECT_EQ (owner.actor->object_generation (), actor.ref ().object_generation ());
            ASSERT_EQ (received.size (), 1);
            EXPECT_EQ (received.front ().to_string (), "local-admission");
            delivered = true;
        })
        .result ());
    EXPECT_TRUE (delivered);
    const auto absent =
      host::public_host_runtime_t::remote_actor_ref (rid, "absent", stale_generation);
    EXPECT_EQ (node->send_to_actor (absent, parts).result ().value (),
               zlink::submit_result_t::not_found);
    node->close ();
}
} // namespace zlink::framework::test::actor_maintenance_join
