/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <zlink/framework/contracts/locations/runtime_query.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <variant>

#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
namespace zlink::framework::test::actor_maintenance_join
{
TEST (RemoteEntryJoinReturn, WaitsForPreviousSourceBeforeApprovalAndPrepare)
{
    using stage_t = fw::detail::spot_node_builder_state_t::remote_entry_join_stage_t;
    auto evidence = std::make_shared<observations_t> ();
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    fw::task_completion_source_t<void> replay_held, return_dispatched;
    std::atomic<int> approvals{0}, prepares{0};
    host_t first, second;
    release_callbacks_t release{*evidence};
    const auto state_of = [] (host_t &host) {
        return fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
          .front ()
          ->spot_state;
    };

    configure (first, evidence, first_rid, store, relocations);
    const auto first_state = state_of (first);
    first_state->before_remote_source_replay_for_tests = [&] {
        replay_held.complete (fw::result_t<void>::success ());
        return evidence->release_completion.task ();
    };
    first_state->remote_entry_join_for_tests = [&] (stage_t stage) {
        if (stage == stage_t::approved)
            ++approvals;
        if (stage == stage_t::dispatched)
            return_dispatched.complete (fw::result_t<void>::success ());
    };
    first.start ();
    ASSERT_TRUE (ready (first));
    auto first_services = first.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (first_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());

    configure (second, evidence, second_rid, store, relocations, default_follow_duration, 0, false,
               entry_kind_t::none);
    state_of (second)->remote_entry_join_for_tests = [&] (stage_t stage) {
        if (stage == stage_t::prepare)
            ++prepares;
    };
    second.start ();
    ASSERT_TRUE (ready (second));
    auto second_services = second.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (peer_ready (first_services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    ASSERT_TRUE (peer_ready (second_services.get_required<fw::route_mesh_runtime_t> (), first_rid));
    ASSERT_TRUE (set_published_placement_weight (first_services, 0, &second_services));
    ASSERT_TRUE (second_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (current_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (set_published_placement_weight (first_services, 1, &second_services));

    auto &first_client = first_services.get_required<fw::actor_client_t> ();
    auto &second_client = second_services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (move_to (first_client, *evidence, current_spot, 1));
    {
        std::lock_guard lock (evidence->mutex);
        ASSERT_EQ (evidence->completions.size (), 1u);
        const auto *outbound = std::get_if<fw::actor_join_accepted_t> (&evidence->completions[0]);
        ASSERT_NE (outbound, nullptr);
        ASSERT_EQ (outbound->actor.node_rid ().value (), second_rid);
    }
    const auto held = replay_held.task ().result_for (wait_budget);
    ASSERT_TRUE (held && *held);
    const auto first_spots = fw::detail::spot_node_runtime_t (first_state);
    ASSERT_EQ (first_spots.resolve_actor_type (actor_id), actor_type);
    ASSERT_EQ (
      first_state->actor_transfer_coordinator->phase (std::string (actor_type) + ":" + actor_id),
      fw::detail::actor_move_phase_t::source_remote);

    const auto requested =
      second_client
        .request (fw::actor_id_t (actor_id), entry_join_t{static_cast<int> (wait_budget / 1ms)})
        .async<probe_t> ()
        .result_for (wait_budget);
    ASSERT_TRUE (requested && *requested);
    const auto dispatched = return_dispatched.task ().result_for (wait_budget);
    ASSERT_TRUE (dispatched && *dispatched);
    const auto entry_id = first_spots.resolve_entry_spot_id ();
    ASSERT_TRUE (entry_id);
    const auto target = first_state->lane
                          .run ([&] {
                              return fw::detail::spot_context_access_t::state (
                                first_state->spot_contexts_by_id.at (std::string (*entry_id)));
                          })
                          .get ();
    const auto lifecycle_probe =
      target->run_serial_task ("entry-return-probe", [] () -> fw::task_t<void> { co_return; })
        .result_for (wait_budget);
    ASSERT_TRUE (lifecycle_probe && *lifecycle_probe);
    EXPECT_EQ (approvals.load (), 0);
    EXPECT_EQ (prepares.load (), 0);

    evidence->release_completion.complete (fw::result_t<void>::success ());
    {
        std::unique_lock lock (evidence->mutex);
        ASSERT_TRUE (evidence->changed.wait_for (
          lock, wait_budget, [&] { return evidence->completions.size () >= 2; }));
        ASSERT_EQ (evidence->completions.size (), 2u);
        const auto &terminal = evidence->completions.back ();
        const auto *returned = std::get_if<fw::actor_join_accepted_t> (&terminal);
        const auto *failed = std::get_if<fw::actor_join_failed_t> (&terminal);
        ASSERT_NE (returned, nullptr)
          << (failed && failed->error_kind == fw::framework_error_kind_t::unavailable
                ? "remote Entry Join completed with Unavailable"
                : "remote Entry Join did not complete with Accepted");
        EXPECT_EQ (returned->actor.actor_id ().value (), actor_id);
        EXPECT_EQ (returned->actor.node_rid ().value (), first_rid);
        EXPECT_EQ (evidence->entry_joined, 1);
    }
    EXPECT_EQ (approvals.load (), 1);
    EXPECT_EQ (prepares.load (), 1);
    const auto location = first_services.get_required<fw::location_runtime_query_t> ()
                            .find_actor_location (fw::actor_id_t (actor_id))
                            .result ();
    ASSERT_TRUE (location && location.value ());
    EXPECT_EQ (location.value ()->node_rid.to_string (), first_rid);
    const auto owner_probe =
      second_client.request (fw::actor_id_t (actor_id), probe_t{17}).async<probe_t> ().result ();
    ASSERT_TRUE (owner_probe) << owner_probe.error ()->what ();
    EXPECT_EQ (owner_probe.value ().value, 17);
}
} // namespace zlink::framework::test::actor_maintenance_join
#endif
