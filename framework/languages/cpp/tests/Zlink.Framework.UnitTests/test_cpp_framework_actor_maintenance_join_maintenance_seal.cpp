/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_observations.hpp"
#include "../support/actor_maintenance_join_native_fixture.hpp"
#include "../support/actor_maintenance_join_suffix_fixture.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>

namespace zlink::framework::test::actor_maintenance_join
{
using zlink::framework::test::begin_reserved_object;

TEST (ActorMaintenanceJoin, MaintenanceSealPreservesLocalAndRemoteClaimErrorKinds)
{
    for (const bool remote : {false, true}) {
        SCOPED_TRACE (remote ? "remote" : "local");
        fw::detail::actor_transfer_coordinator_t transfers;
        const auto begin = [&] (const std::string &key) {
            return remote ? transfers.try_begin_source_remote (key)
                          : transfers.try_begin_local (key);
        };
        ASSERT_TRUE (begin ("existing"));
        const auto conflict = begin ("existing");
        EXPECT_FALSE (conflict);
        EXPECT_EQ (fw::framework_error_kind_t::rejected, conflict.error_kind ());
        transfers.cancel_move ("existing");
        ASSERT_TRUE (transfers.try_seal_maintenance_inventory ().result ().value ());
        const auto sealed = begin ("late");
        EXPECT_FALSE (sealed);
        EXPECT_EQ (fw::framework_error_kind_t::unavailable, sealed.error_kind ());
        transfers.end_maintenance_inventory ().result ().value ();
        ASSERT_TRUE (transfers.try_reserve_source ("prior"));
        auto waiting = transfers.try_seal_maintenance_inventory ();
        EXPECT_FALSE (waiting.await_ready ());
        EXPECT_TRUE (begin ("prior"));
        transfers.cancel_move ("prior");
        ASSERT_TRUE (waiting.result ().value ());
        transfers.end_maintenance_inventory ().result ().value ();
    }
}

TEST (NativeIntake, CoordinatorAdmissionReturnsNativeClaimWithoutLaneReentry)
{
    namespace rt = fw::runtime;
    namespace st = rt::stateful;
    native_intake_fixture_t fixture;
    auto &objects = fixture.host->objects ();
    const st::object_ref_t object{st::object_kind_t::actor,
                                  actor_id,
                                  1,
                                  1,
                                  mesh_name,
                                  zlink::routing_id_t::from (fixture.rid).to_string ()};
    const auto created = begin_reserved_object (objects, object, actor_type, {});
    ASSERT_EQ (st::stateful_error_t::none, created.error);
    ASSERT_EQ (st::stateful_error_t::none, objects.commit_create (created.attempt));
    st::raw_stateful_dispatch_t raw (objects, fixture.host->transport (), fixture.resolver);
    ASSERT_EQ (st::stateful_error_t::none,
               raw
                 .ingest_claimed (object,
                                  fw::test::publish_stateful_claim (
                                    fixture.host->transport (), object, fixture.actor_record (1)),
                                  fw::test::retained_ingress_snapshot (objects, object))
                 .result ()
                 .value ());
    auto [claim_error, delivery] = raw.try_claim (object);
    ASSERT_EQ (st::stateful_error_t::none, claim_error);
    ASSERT_TRUE (delivery);
    fw::test::manual_internal_scheduler_t worker;
    auto queue = std::make_shared<rt::serial_execution_queue_t> (worker);
    auto record = std::make_shared<rt::serial_application_record_t> ();
    record->transferred = [&] {
        EXPECT_EQ (st::stateful_error_t::none,
                   raw.transfer_application_claim (object, delivery->turn.sequence));
    };
    rt::serial_work_options_t options;
    options.application_record = record;
    EXPECT_NO_THROW ({
        const auto admitted = objects.actor_transfers ()->admit_dispatch_packet (
          "member:member", nullptr, true, {},
          [&] { return queue->try_post ("native-claim", [] {}, options); });
        EXPECT_EQ (std::optional<bool> (true), admitted.queue_submission);
    });
    // On the red input the callback threw before releasing the stateful claim.
    (void) objects.complete_claim (object, st::turn_domain_t::application);
    EXPECT_TRUE (queue->try_post ("after-native-claim", [] {}));
    worker.run_all ();
    queue->close ();
    worker.run_all ();
    queue->drain ();
    worker.retire ();
    fixture.host->close ();
}

TEST (ActorMaintenanceJoin, MaintenanceSealRejectsClaimAfterPriorSourceCompletion)
{
    native_intake_fixture_t fixture;
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (mesh_name);
    fw::detail::spot_node_runtime_t (state).attach_native_node (fixture.host);
    const std::string key = "member:prior";
    ASSERT_TRUE (state->actor_transfer_coordinator->try_reserve_source (key));
    auto completion = state->actor_transfer_coordinator->wait_for_source_completion (key);
    EXPECT_FALSE (completion.await_ready ());
    state->actor_transfer_coordinator->cancel_move (key);
    ASSERT_TRUE (completion.result ());
    auto inventory = fixture.host->objects ().try_begin_maintenance_inventory ();
    ASSERT_TRUE (inventory.result ().value ());
    const auto late = state->actor_transfer_coordinator->try_reserve_source ("member:late");
    EXPECT_FALSE (late);
    EXPECT_EQ (fw::framework_error_kind_t::unavailable, late.error_kind ());
    state->actor_transfer_coordinator->cancel_move ("member:late");
    fixture.host->objects ().end_maintenance_inventory ().result ().value ();
    fixture.host->close ();
}

void claimed_join_native_admission (bool remote)
{
    namespace sf = fw::runtime::stateful;
    native_intake_fixture_t fixture;
    auto state =
      fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (mesh_name);
    fw::detail::spot_node_runtime_t (state).attach_native_node (fixture.host);
    auto &objects = fixture.host->objects ();
    objects.replace_placement_candidates (
      {{mesh_name, "intake-boundary", {actor_type}, 100, 16, 0, 4, 0}});
    const auto created = objects.begin_create (
      {sf::object_kind_t::actor, actor_id, actor_type, std::string (mesh_name), {}, false, false});
    ASSERT_EQ (sf::create_status_t::reserved, created.status);
    ASSERT_EQ (sf::stateful_error_t::none, objects.commit_create (created.attempt));
    const std::string key = "member:member";
    ASSERT_TRUE (state->actor_transfer_coordinator->try_reserve_source (key));
    if (remote)
        ASSERT_TRUE (state->actor_transfer_coordinator->try_begin_source_remote (key));
    else
        ASSERT_TRUE (state->actor_transfer_coordinator->try_begin_local (key));
    const auto spot = objects.begin_create ({sf::object_kind_t::user_spot,
                                             "local-target",
                                             actor_type,
                                             std::string (mesh_name),
                                             {},
                                             false,
                                             false});
    ASSERT_EQ (sf::create_status_t::reserved, spot.status);
    ASSERT_EQ (sf::stateful_error_t::none, objects.commit_create (spot.attempt));
    auto inventory = objects.try_begin_maintenance_inventory ();
    const sf::object_ref_t target{
      sf::object_kind_t::user_spot, "target", 1, 1, mesh_name, "remote"};
    const auto [error, token] =
      remote
        ? objects.begin_remote_membership_move_async (created.object, target).result ().value ()
        : objects.begin_membership_move_async (created.object, spot.object).result ().value ();
    EXPECT_EQ (sf::stateful_error_t::none, error);
    EXPECT_FALSE (inventory.await_ready ());
    const auto late = state->actor_transfer_coordinator->try_reserve_source ("member:late");
    EXPECT_FALSE (late);
    EXPECT_EQ (fw::framework_error_kind_t::unavailable, late.error_kind ());
    if (error == sf::stateful_error_t::none)
        objects.abort_membership_move_async (token).result ().value ();
    state->actor_transfer_coordinator->cancel_move (key);
    state->actor_transfer_coordinator->cancel_move ("member:late");
    ASSERT_TRUE (inventory.result ().value ());
    objects.end_maintenance_inventory ().result ().value ();
    fixture.host->close ();
}
TEST (ActorMaintenanceJoin, ClaimedJoinKeepsNativeAdmissionWhileMaintenanceSeals)
{
    claimed_join_native_admission (true);
}

TEST (ActorMaintenanceJoin, ClaimedLocalJoinKeepsNativeAdmissionWhileMaintenanceSeals)
{
    claimed_join_native_admission (false);
}

TEST (ActorMaintenanceJoin, ObjectSealCapturesChangesAfterJoinSealAndRejectsLaterChanges)
{
    namespace st = fw::runtime::stateful;
    st::stateful_object_runtime_t objects;
    objects.replace_placement_candidates (
      {{mesh_name, first_rid, {actor_type}, 100, 16, 0, 16, 0}});
    const auto create = [&] (st::object_kind_t kind, const std::string &key) {
        return objects.begin_create (
          {kind, key, actor_type, std::string (mesh_name), {}, false, false});
    };
    const auto old = create (st::object_kind_t::actor, "old");
    ASSERT_EQ (st::create_status_t::reserved, old.status);
    ASSERT_EQ (st::stateful_error_t::none, objects.commit_create (old.attempt));
    const auto spot = create (st::object_kind_t::user_spot, "old-spot");
    ASSERT_EQ (st::create_status_t::reserved, spot.status);
    ASSERT_EQ (st::stateful_error_t::none, objects.commit_create (spot.attempt));
    auto transfers = objects.actor_transfers ();
    ASSERT_TRUE (transfers->try_reserve_source ("prior"));
    auto inventory = objects.try_begin_maintenance_inventory ();
    ASSERT_FALSE (inventory.await_ready ());
    EXPECT_FALSE (transfers->try_reserve_source ("late"));
    // Only Join claims are sealed while the preceding source finishes.
    EXPECT_EQ (st::stateful_error_t::none, objects.destroy_actor (old.object));
    EXPECT_EQ (st::stateful_error_t::none, objects.close_spot (spot.object).first);
    const auto added = create (st::object_kind_t::actor, "added");
    const auto added_commit = objects.commit_create (added.attempt);
    const st::object_ref_t reserved{
      st::object_kind_t::user_spot, "reserved", 1, 1, mesh_name, first_rid};
    const auto reserved_create = begin_reserved_object (objects, reserved, actor_type, {});
    const auto reserved_commit = objects.commit_create (reserved_create.attempt);
    transfers->cancel_move ("prior");
    const auto snapshot = inventory.result ().value ();
    ASSERT_EQ (st::create_status_t::reserved, added.status);
    ASSERT_EQ (st::stateful_error_t::none, added_commit);
    ASSERT_EQ (st::stateful_error_t::none, reserved_create.error);
    ASSERT_EQ (st::stateful_error_t::none, reserved_commit);
    ASSERT_TRUE (snapshot);
    ASSERT_EQ (2u, snapshot->size ());
    EXPECT_TRUE (std::any_of (snapshot->begin (), snapshot->end (),
                              [&] (const auto &entry) { return entry.owner == added.object; }));
    EXPECT_TRUE (std::any_of (snapshot->begin (), snapshot->end (),
                              [&] (const auto &entry) { return entry.owner == reserved; }));
    EXPECT_EQ (st::stateful_error_t::moving, create (st::object_kind_t::actor, "after").error);
    auto late_reserved = reserved;
    late_reserved.key = "after-reserved";
    EXPECT_EQ (st::stateful_error_t::moving,
               begin_reserved_object (objects, late_reserved, actor_type, {}).error);
    EXPECT_EQ (st::stateful_error_t::moving, objects.destroy_actor (added.object));
    EXPECT_EQ (st::stateful_error_t::moving, objects.close_spot (reserved).first);
    objects.end_maintenance_inventory ().result ().value ();
    EXPECT_TRUE (transfers->try_begin_local ("released"));
    transfers->cancel_move ("released");
    EXPECT_EQ (st::stateful_error_t::none, objects.destroy_actor (added.object));
    EXPECT_EQ (st::stateful_error_t::none, objects.close_spot (reserved).first);
}

TEST (ActorMaintenanceJoin, LocalNextJoinPreservesClaimedNativeSuffixBeforePrivateAdmission)
{
    next_join_suffix (false, false, false, false, true);
}
TEST (ActorMaintenanceJoin, RemoteNextJoinPreservesClaimedNativeSuffixBeforePrivateAdmission)
{
    next_join_suffix (true, false, false, false, true);
}
} // namespace zlink::framework::test::actor_maintenance_join
