/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_actors.hpp"
#include "../support/actor_maintenance_join_stream_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
constexpr auto source_spot = "source";

TEST (ActorMaintenanceJoin, RejectsWhilePreviousSourceLeaveIsPending)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_callbacks_t release{evidence};
    configure (first, evidence_owner, first_rid, store, relocations);
    first.start ();
    ASSERT_TRUE (ready (first));
    auto services = first.app.advanced ().services ().build_provider ();
    auto &spots = services.get_required<fw::spot_manager_t> ();
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (previous_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (maintenance_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    auto *sender = &client;
    ASSERT_TRUE (move_to (*sender, evidence, previous_spot, 1));
    configure (second, evidence_owner, second_rid, store, relocations);
    second.start ();
    ASSERT_TRUE (ready (second));
    const auto target_state =
      fw::detail::mesh_node_runtime_t::registrations (second.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    fw::detail::spot_node_runtime_t (target_state)
      .native_node ()
      ->objects ()
      .configure_relocation_materialization (
        [target_state] (const auto &frozen, const auto &object, const auto &spot,
                        std::stop_token cancellation) {
            return fw::detail::spot_node_runtime_t (target_state)
              .materialize_relocation_state (frozen, object, spot, cancellation);
        },
        [target_state, &evidence] (const auto &objects) {
            // Admit local work after identity publication, before lifecycle opens.
            std::unique_lock lock (evidence.mutex);
            evidence.materialization_entered = true;
            evidence.changed.notify_all ();
            evidence.changed.wait (
              lock, [&] { return evidence.release_completion.task ().await_ready (); });
            lock.unlock ();
            return fw::detail::spot_node_runtime_t (target_state)
              .commit_relocation_materialization (objects);
        },
        [target_state] (const auto &objects) {
            return fw::detail::spot_node_runtime_t (target_state)
              .abort_relocation_materialization_async (objects);
        });
    auto &routes = services.get_required<fw::route_mesh_runtime_t> ();
    ASSERT_TRUE (peer_ready (routes));
    // First node cannot place this new room; placement is selected by the public weight API.
    auto second_services = second.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (set_published_placement_weight (services, 0, &second_services));
    const auto created = second_services.get_required<fw::spot_manager_t> ()
                           .get_or_create (fw::spot_id_t (current_spot), room_type)
                           .timeout (wait_budget)
                           .async ()
                           .result ();
    ASSERT_TRUE (created) << (created.error () ? created.error ()->what () : "no error");
    ASSERT_TRUE (sender->send (fw::actor_id_t (actor_id), move_t{current_spot}).async ().result ());
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return evidence.materialization_entered; }));
        EXPECT_EQ (evidence.completions.size (), 1);
    }
    sender = &second_services.get_required<fw::actor_client_t> ();
    auto submission = std::async (std::launch::async, [&] {
        return sender->send (fw::actor_id_t (actor_id), move_t{maintenance_spot})
          .async ()
          .result ();
    });
    const auto admitted_while_closed =
      submission.wait_for (wait_budget) == std::future_status::ready;
    {
        std::lock_guard lock (evidence.mutex);
        EXPECT_EQ (evidence.completions.size (), 1);
    }
    evidence.release_completion.complete (fw::result_t<void>::success ());
    {
        std::lock_guard lock (evidence.mutex);
        evidence.changed.notify_all ();
    }
    const auto submitted = submission.get ();
    ASSERT_TRUE (admitted_while_closed);
    ASSERT_TRUE (submitted) << (submitted.error () ? submitted.error ()->what () : "no error");
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return evidence.completions.size () >= 3; }));
    }
    {
        std::lock_guard lock (evidence.mutex);
        ASSERT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (1)));
        EXPECT_TRUE (
          std::holds_alternative<fw::actor_join_rejected_t> (evidence.completions.at (2)))
          << (std::get_if<fw::actor_join_failed_t> (&evidence.completions.at (2))
                ? static_cast<int> (
                    std::get<fw::actor_join_failed_t> (evidence.completions.at (2)).error_kind)
                : -1);
        EXPECT_EQ (evidence.completion_owners.at (2), second_rid);
    }
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (
          evidence.changed.wait_for (lock, wait_budget, [&] { return evidence.leave_entered; }));
    }
    auto reply = sender->request (fw::actor_id_t (actor_id), probe_t{17})
                   .timeout (wait_budget)
                   .async<probe_t> ()
                   .result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 17);
}
class ActorRestoreCommitFailure : public ::testing::TestWithParam<bool>
{
};

TEST_P (ActorRestoreCommitFailure, DiscardsPublishedIdentity)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    std::optional<bool> published;
    std::optional<fw::actor_ref_t> candidate;
    bool discarded = false;
    bool identity_removed = false;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_callbacks_t release{evidence};
    configure (first, evidence_owner, first_rid, store, relocations);
    first.start ();
    ASSERT_TRUE (ready (first));
    auto services = first.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (second, evidence_owner, second_rid, store, relocations);
    second.start ();
    ASSERT_TRUE (ready (second));
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (second.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto target = fw::detail::spot_node_runtime_t (state).native_node ();
    const auto weak_target = std::weak_ptr (target);
    const auto throw_failure = GetParam ();
    target->objects ().configure_relocation_materialization (
      [state] (const auto &frozen, const auto &object, const auto &spot,
               std::stop_token cancellation) {
          return fw::detail::spot_node_runtime_t (state).materialize_relocation_state (
            frozen, object, spot, cancellation);
      },
      [&, state, weak_target, throw_failure] (const auto &objects) {
          const auto node = weak_target.lock ();
          if (!node)
              return false;
          const auto ref = fw::runtime::host::public_host_runtime_t::remote_actor_ref (
            zlink::routing_id_t::from (objects.front ().node_id), objects.front ().key,
            objects.front ().object_generation);
          const auto current = node->resolve_actor_async (ref).result ().value ();
          {
              std::lock_guard lock (evidence.mutex);
              candidate = ref;
              published = current && *current == objects.front ();
          }
          evidence.changed.notify_all ();
          // On the old implementation, let its unpublished target finish so
          // the publication assertion fails without stranding its terminal.
          if (!current)
              return fw::detail::spot_node_runtime_t (state).commit_relocation_materialization (
                objects);
          if (throw_failure)
              throw std::runtime_error ("injected restore commit failure");
          return false;
      },
      [&, state, weak_target] (const auto &objects) -> fw::task_t<void> {
          const auto node = weak_target.lock ();
          const auto removed =
            !candidate || !node || !(co_await node->resolve_actor_async (*candidate));
          co_await fw::detail::spot_node_runtime_t (state).abort_relocation_materialization_async (
            objects);
          {
              std::lock_guard lock (evidence.mutex);
              discarded = true;
              identity_removed = removed;
          }
          evidence.changed.notify_all ();
      });
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    auto second_services = second.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (set_published_placement_weight (services, 0, &second_services));
    ASSERT_TRUE (second_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (current_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (services.get_required<fw::actor_client_t> ()
                   .send (fw::actor_id_t (actor_id), move_t{current_spot})
                   .async ()
                   .result ());
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (
          evidence.changed.wait_for (lock, wait_budget, [&] { return published.has_value (); }));
        ASSERT_TRUE (*published);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget, [&] { return discarded; }));
        EXPECT_TRUE (identity_removed);
    }
    target->seal_relocation_targets ().result ().value ();
    EXPECT_FALSE (target->resolve_actor_async (*candidate).result ().value ());
    EXPECT_FALSE (target->objects ().find (fw::runtime::stateful::object_kind_t::actor, actor_id));
    const std::vector<zlink::message_t> parts{zlink::message_t::from ("failed-restore")};
    EXPECT_EQ (target->send_to_actor (*candidate, parts).result ().value (),
               zlink::submit_result_t::not_found);
}

INSTANTIATE_TEST_SUITE_P (ReturnOrThrow, ActorRestoreCommitFailure, ::testing::Bool ());

TEST (ActorMaintenanceJoin, PreservesEarlierIdentityWhenRestoreIsRejected)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_callbacks_t release{evidence};
    configure (first, evidence_owner, first_rid, store, relocations);
    first.start ();
    ASSERT_TRUE (ready (first));
    auto services = first.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (second, evidence_owner, second_rid, store, relocations);
    second.start ();
    ASSERT_TRUE (ready (second));
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (second.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto target = fw::detail::spot_node_runtime_t (state).native_node ();
    constexpr std::uint64_t previous_generation = 2;
    const fw::runtime::stateful::object_ref_t previous{
      fw::runtime::stateful::object_kind_t::actor,
      actor_id,
      previous_generation,
      previous_generation,
      mesh_name,
      zlink::routing_id_t::from (second_rid).to_string ()};
    auto previous_actor =
      target->create_reserved_actor_async (actor_type, previous).result ().value ();
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    auto second_services = second.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (set_published_placement_weight (services, 0, &second_services));
    ASSERT_TRUE (second_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (current_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (move_to (services.get_required<fw::actor_client_t> (), evidence, current_spot, 1));
    {
        std::lock_guard lock (evidence.mutex);
        EXPECT_TRUE (
          std::holds_alternative<fw::actor_join_failed_t> (evidence.completions.front ()));
    }
    const auto current = target->resolve_actor_async (previous_actor.ref ()).result ().value ();
    ASSERT_TRUE (current);
    EXPECT_EQ (*current, previous);
}

TEST (ActorMaintenanceJoin, RemoteJoinReturnsToPreviousSourceNode)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_callbacks_t release{evidence};
    configure (first, evidence_owner, first_rid, store, relocations);
    first.start ();
    ASSERT_TRUE (ready (first));
    auto services = first.app.advanced ().services ().build_provider ();
    auto &spots = services.get_required<fw::spot_manager_t> ();
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (local_spot), room_type).async ().result ());
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (source_spot), room_type).async ().result ());
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (move_to (client, evidence, local_spot, 1));
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (0)));
    EXPECT_EQ (evidence.completion_spots.at (0), fw::spot_id_t (local_spot));
    configure (second, evidence_owner, second_rid, store, relocations);
    second.start ();
    ASSERT_TRUE (ready (second));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    auto second_services = second.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (set_published_placement_weight (services, 0, &second_services));
    ASSERT_TRUE (second_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (current_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (move_to (client, evidence, current_spot, 2));
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (1)));
    EXPECT_EQ (evidence.completion_owners.at (1), second_rid);
    EXPECT_EQ (evidence.completion_spots.at (1), fw::spot_id_t (current_spot));
    // Return through the public Deferred Join immediately after Accepted.
    // Source OnLeave and stale instance destruction remain independent.
    ASSERT_TRUE (
      move_to (second_services.get_required<fw::actor_client_t> (), evidence, source_spot, 3));
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (2)))
      << (std::get_if<fw::actor_join_failed_t> (&evidence.completions.at (2))
            ? static_cast<int> (
                std::get<fw::actor_join_failed_t> (evidence.completions.at (2)).error_kind)
            : -1);
    EXPECT_EQ (evidence.completion_owners.at (2), first_rid);
    EXPECT_EQ (evidence.completion_spots.at (2), fw::spot_id_t (source_spot));
}

// Session–Actor binding §6/§8: a returning Actor carries its current binding,
// even when this node last hosted it on a different physical Session.
TEST (ActorMaintenanceJoin, ReboundActorReturnsWithCurrentSession)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second, session;
    release_callbacks_t release{evidence};
    const auto configure_public = [&] (host_t &host, const char *rid) {
        auto &options = host.app.add_zlink_framework ();
        options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
        options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
        options.add_location_store (store);
        options.add_relocation_store (relocations);
        host.app.logging ().use_file (std::string (rid) + ".flow");
        options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::detailed);
        auto mesh = options.add_route_mesh (mesh_name);
        mesh.set_object_role (fw::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (rid))
          .listen ("tcp://127.0.0.1:0");
        mesh.add_entry_spot<entry_t> (
          [evidence = evidence_owner] (fw::entry_spot_context_t context) {
              return std::make_shared<entry_t> (std::move (context), evidence);
          });
        mesh
          .add_spot_factory<room_t> (
            room_type,
            [evidence = evidence_owner] (fw::spot_context_t context) {
                return std::make_shared<room_t> (std::move (context), evidence);
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_actor_factory<actor_t, factory_t> (
            actor_type, std::make_shared<factory_t> (evidence_owner),
            [] (auto &factory) { factory.recreate_on_relocation (); });
    };
    configure_public (first, first_rid);
    first.start ();
    ASSERT_TRUE (ready (first));
    auto first_services = first.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (first_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (first_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (source_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (set_published_placement_weight (first_services, 0));
    configure_public (second, second_rid);
    second.start ();
    ASSERT_TRUE (ready (second));
    auto second_services = second.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (second_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (current_spot), room_type)
                   .async ()
                   .result ());
    configure_session (session, evidence_owner, store, relocations);
    session.start ();
    ASSERT_TRUE (ready (session));
    auto session_services = session.app.advanced ().services ().build_provider ();
    auto &routes = session_services.get_required<fw::route_mesh_runtime_t> ();
    ASSERT_TRUE (peer_ready (routes, first_rid));
    ASSERT_TRUE (peer_ready (routes, second_rid));
    ASSERT_TRUE (
      peer_ready (first_services.get_required<fw::route_mesh_runtime_t> (), session_rid));
    ASSERT_TRUE (
      peer_ready (second_services.get_required<fw::route_mesh_runtime_t> (), session_rid));
    const auto endpoint = session_services.get_required<fw::framework_runtime_t> ()
                            .listener_status (fw::listener_kind_t::stream, stream_name)
                            .endpoint;
    view_client_t previous (endpoint);
    ASSERT_TRUE (previous.connect ());
    ASSERT_EQ (previous.view (), std::optional<int>{1});
    ASSERT_TRUE (
      move_to (first_services.get_required<fw::actor_client_t> (), evidence, current_spot, 1));
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (0)));
    ASSERT_EQ (previous.view (), std::optional<int>{2});
    // A reply on the new connection proves its bind terminal and Actor handler
    // finished before the return Join. No timer or private scheduling hook is used.
    view_client_t current (endpoint);
    ASSERT_TRUE (current.connect ());
    ASSERT_EQ (current.view (), std::optional<int>{2});
    ASSERT_TRUE (
      move_to (second_services.get_required<fw::actor_client_t> (), evidence, source_spot, 2));
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (1)));
    EXPECT_EQ (evidence.completion_owners.at (1), first_rid);
    EXPECT_EQ (current.view (), std::optional<int>{3});
}

TEST (ActorMaintenanceJoin, ReturnAdmissionWaitsOutsideTargetLifecycleForSourceTerminal)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    host_t host;
    release_callbacks_t release{evidence};
    configure (host, evidence_owner, first_rid,
               std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    const auto ingress_ready = services.get_required<fw::actor_client_t> ()
                                 .request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                                 .async<probe_t> ()
                                 .result ();
    ASSERT_TRUE (ingress_ready);
    const auto created = services.get_required<fw::spot_manager_t> ()
                           .get_or_create (fw::spot_id_t (source_spot), room_type)
                           .async ()
                           .result ();
    ASSERT_TRUE (created);
    const auto state =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ())
        .front ()
        ->spot_state;
    const auto key = std::string (actor_type) + ":" + actor_id;
    ASSERT_TRUE (
      state->actor_transfer_coordinator->try_begin_source_remote (key, "previous-source"));
    struct finish_source_t
    {
        std::shared_ptr<fw::detail::spot_node_builder_state_t> state;
        std::string key;
        ~finish_source_t () { state->actor_transfer_coordinator->complete_move (key); }
    };
    auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
    auto admission = admitted->task ();
    finish_source_t finish{state, key};
    // Only the coordinator state is fixed here. The target lifecycle and the
    // source-terminal observer use their production dispatch paths.
    fw::detail::spot_node_runtime_t (state).dispatch_wire_actor_join_admission (
      fw::spot_id_t (source_spot), actor_id,
      [admitted] () -> fw::task_t<void> {
          admitted->complete (fw::result_t<void>::success ());
          co_return;
      },
      [admitted] (fw::result_t<void>) {
          admitted->complete (fw::result_t<void>::failure (fw::framework_error_kind_t::unavailable,
                                                           "return admission was rejected"));
      });
    auto target = state->lane
                    .run ([&] {
                        return fw::detail::spot_context_access_t::state (
                          state->spot_contexts_by_id.at (source_spot));
                    })
                    .get ();
    ASSERT_TRUE (
      target->run_serial_task ("source-terminal-probe", [] () -> fw::task_t<void> { co_return; })
        .result ());
    EXPECT_FALSE (admission.await_ready ())
      << "return admission must remain outside the target lifecycle until the old source ends";
    EXPECT_TRUE (state->actor_transfer_coordinator->finish_move_replay (key).completed);
    EXPECT_TRUE (admission.result ());

    // A target may close while admission is still waiting outside its turn.
    // The same lifecycle submission must reject after source completion.
    ASSERT_TRUE (
      state->actor_transfer_coordinator->try_begin_source_remote (key, "closing-source"));
    auto after_close = std::make_shared<fw::task_completion_source_t<void>> ();
    auto closing_admission = after_close->task ();
    fw::detail::spot_node_runtime_t (state).dispatch_wire_actor_join_admission (
      fw::spot_id_t (source_spot), actor_id,
      [after_close] () -> fw::task_t<void> {
          after_close->complete (fw::result_t<void>::success ());
          co_return;
      },
      [after_close] (fw::result_t<void>) {
          after_close->complete (fw::result_t<void>::failure (
            fw::framework_error_kind_t::unavailable, "return target closed"));
      });
    ASSERT_TRUE (
      services.get_required<fw::spot_manager_t> ().close (created.value ().spot).result ());
    EXPECT_FALSE (closing_admission.await_ready ());
    EXPECT_TRUE (state->actor_transfer_coordinator->finish_move_replay (key).completed);
    const auto closed = closing_admission.result ();
    ASSERT_FALSE (closed);
    EXPECT_EQ (closed.error_kind (), fw::framework_error_kind_t::unavailable);
}

TEST (ActorMaintenanceJoin, SameNodeMembershipPreservesReturnedActorUntilSourceCleanup)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    evidence.completion_to_hold = 3;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_callbacks_t release{evidence};
    configure (first, evidence_owner, first_rid, store, relocations, source_follow_duration, 2);
    first.start ();
    ASSERT_TRUE (ready (first));
    auto services = first.app.advanced ().services ().build_provider ();
    auto &spots = services.get_required<fw::spot_manager_t> ();
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (source_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (local_spot), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    auto *sender = &client;
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    ASSERT_TRUE (move_to (*sender, evidence, source_spot, 1));

    configure (second, evidence_owner, second_rid, store, relocations, source_follow_duration);
    second.start ();
    ASSERT_TRUE (ready (second));
    auto &routes = services.get_required<fw::route_mesh_runtime_t> ();
    ASSERT_TRUE (peer_ready (routes));
    auto second_services = second.app.advanced ().services ().build_provider ();
    const auto created = second_services.get_required<fw::spot_manager_t> ()
                           .get_or_create (fw::spot_id_t (current_spot), room_type)
                           .timeout (wait_budget)
                           .async ()
                           .result ();
    ASSERT_TRUE (created) << (created.error () ? created.error ()->what () : "no error");
    auto source_instance = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                             .timeout (wait_budget)
                             .async<probe_t> ()
                             .result ();
    ASSERT_TRUE (source_instance);
    ASSERT_TRUE (move_to (*sender, evidence, current_spot, 2));
    // Release the first source before creating a newer source-follow route.
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget, [&] {
            return std::find (evidence.destroyed.begin (), evidence.destroyed.end (),
                              source_instance.value ().value)
                   != evidence.destroyed.end ();
        }));
    }
    sender = &second_services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (move_to (*sender, evidence, local_spot, 3));
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (2)))
      << (std::get_if<fw::actor_join_failed_t> (&evidence.completions.at (2))
            ? static_cast<int> (
                std::get<fw::actor_join_failed_t> (evidence.completions.at (2)).error_kind)
            : -1);
    sender = &client;
    // Submit while the returning Actor's completion still owns its execution
    // turn. Admission must park the payload until that callback has completed.
    const auto submitted =
      sender->send (fw::actor_id_t (actor_id), move_t{source_spot}).async ().result ();
    evidence.release_completion.complete (fw::result_t<void>::success ());
    ASSERT_TRUE (submitted) << (submitted.error () ? submitted.error ()->what () : "no error");
    {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget,
                                                [&] { return evidence.completions.size () >= 4; }));
    }
    ASSERT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.at (3)));
    auto initial = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                     .timeout (wait_budget)
                     .async<probe_t> ()
                     .result ();
    ASSERT_TRUE (initial);
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    do {
        auto current = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                         .timeout (wait_budget)
                         .async<probe_t> ()
                         .result ();
        ASSERT_TRUE (current);
        ASSERT_EQ (current.value ().value, initial.value ().value)
          << "source cleanup replaced the current membership's Actor instance";
        std::this_thread::yield ();
    } while (std::chrono::steady_clock::now () < deadline);
}
} // namespace zlink::framework::test::actor_maintenance_join
