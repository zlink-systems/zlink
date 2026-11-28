/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_access.hpp"
#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/scope_exit.hpp"
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

namespace zlink::framework::test::actor_maintenance_join
{
TEST (RemoteEntryJoinReturn, WaitsForPreviousSourceBeforeApprovalAndPrepare)
{
    using join_result_t = fw::runtime::protocol::actor_join_result_t;
    struct wire_observation_t
    {
        fw::task_completion_source_t<void> dispatched;
        std::atomic<int> replies{0};
        std::atomic<int> approvals{0};
        std::atomic<bool> prepare_seen{false};
    };
    auto wire = std::make_shared<wire_observation_t> ();
    auto evidence = std::make_shared<observations_t> ();
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_callbacks_t release{*evidence};

    configure (first, evidence, first_rid, store, relocations);
    first.start ();
    ASSERT_TRUE (ready (first));
    auto first_services = first.app.advanced ().services ().build_provider ();
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (first.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const fw::detail::spot_node_runtime_t first_spots (state);
    const auto native = first_spots.native_node ();
    ASSERT_TRUE (native);
    fw::runtime::host::public_host_runtime_test_access_t::observe_entry_join (
      *native, actor_id, [wire] { wire->dispatched.complete (fw::result_t<void>::success ()); },
      [wire] (const auto &reply) {
          if (reply.join_result == join_result_t::accepted)
              ++wire->approvals;
          ++wire->replies;
      },
      [wire] { wire->prepare_seen.store (true); });

    ASSERT_TRUE (first_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (second, evidence, second_rid, store, relocations, default_follow_duration, 0, false,
               entry_kind_t::none);
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
    const auto coordinator = state->actor_transfer_coordinator;
    const auto key = std::string (actor_type) + ":" + actor_id;
    const auto drained = coordinator->wait_for_source_completion (key).result_for (wait_budget);
    ASSERT_TRUE (drained && *drained);
    ASSERT_EQ (first_spots.resolve_actor_type (actor_id), actor_type);
    // Pin the retained source phase after the real outbound move. The return
    // Join uses the actual transport, dispatcher and lifecycle on both nodes.
    ASSERT_TRUE (coordinator->try_begin_source_remote (key, "previous-source"));
    scope_exit_t finish_previous (
      [&] { EXPECT_TRUE (coordinator->finish_move_replay (key).completed); });

    const auto requested =
      second_client.send (fw::actor_id_t (actor_id), move_t{entry_target}).async ().result ();
    ASSERT_TRUE (requested);
    const auto dispatched = wire->dispatched.task ().result_for (wait_budget);
    ASSERT_TRUE (dispatched && *dispatched);
    const auto entry_id = first_spots.resolve_entry_spot_id ();
    ASSERT_TRUE (entry_id);
    const auto entry = state->lane
                         .run ([&] {
                             return fw::detail::spot_context_access_t::state (
                               state->spot_contexts_by_id.at (std::string (*entry_id)));
                         })
                         .get ();
    const auto probe =
      entry->run_serial_task ("entry-return-probe", [] () -> fw::task_t<void> { co_return; })
        .result_for (wait_budget);
    ASSERT_TRUE (probe && *probe);
    EXPECT_EQ (wire->replies.load (), 0) << "admission replied during previous source cleanup";
    EXPECT_FALSE (wire->prepare_seen.load ());

    finish_previous.run ();
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
    EXPECT_EQ (wire->replies.load (), 1);
    EXPECT_EQ (wire->approvals.load (), 1);
    EXPECT_TRUE (wire->prepare_seen.load ());
    const auto location = first_services.get_required<fw::location_runtime_query_t> ()
                            .find_actor_location (fw::actor_id_t (actor_id))
                            .result ();
    ASSERT_TRUE (location && location.value ());
    EXPECT_EQ (location.value ()->node_rid.to_string (), first_rid);
}
} // namespace zlink::framework::test::actor_maintenance_join
