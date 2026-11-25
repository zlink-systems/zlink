/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_observations.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <gtest/gtest.h>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
// Keep settlement work queued while the Actor FIFO is free to advance.
class held_join_settlement_executor_t final : public fw::runtime::offload_executor_t
{
  public:
    bool try_submit_internal (std::function<void ()> work) override
    {
        std::lock_guard lock (_mutex);
        _pending.push_back (std::move (work));
        return true;
    }

    void flush ()
    {
        std::vector<std::function<void ()>> pending;
        {
            std::lock_guard lock (_mutex);
            pending.swap (_pending);
        }
        for (auto &work : pending)
            work ();
    }

  private:
    std::mutex _mutex;
    std::vector<std::function<void ()>> _pending;
};

struct join_claim_fixture_t
{
    std::shared_ptr<fw::detail::spot_node_builder_state_t> node =
      fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> ("join-claim-node");
    std::shared_ptr<held_join_settlement_executor_t> settlement =
      std::make_shared<held_join_settlement_executor_t> ();
    std::shared_ptr<fw::runtime::offload_executor_t> executor =
      std::make_shared<fw::runtime::offload_executor_t> (1);
    std::shared_ptr<fw::detail::spot_context_state_t> context =
      std::make_shared<fw::detail::spot_context_state_t> ();
    fw::actor_ref_t actor = fw::detail::actor_ref_access_t::make (
      fw::node_rid_t::from_string ("join-claim-node"), "player", "claim-actor", 1);
    fw::detail::spot_node_runtime_t owner{node};
    std::shared_ptr<fw::runtime::serial_execution_queue_t> queue;

    join_claim_fixture_t ()
    {
        node->worker_executor = settlement;
        fw::service_collection_t services;
        node->root_services = services.build_provider ();
        context->node = node;
        context->node_rid = fw::node_rid_t::from_string ("join-claim-node");
        context->spot_id = fw::spot_id_t ("join-claim-spot");
        context->serial_executor = executor;
        context->serial_queue = std::make_shared<fw::runtime::serial_execution_queue_t> (
          *executor, fw::runtime::serial_execution_queue_options_t{},
          fw::runtime::serial_execution_queue_t::error_handler_t{},
          fw::runtime::serial_lane_policy_t::spot_wide ());
        const std::string key = "player:claim-actor";
        node->actor_spot_ids.emplace (key, context->spot_id);
        node->spot_contexts_by_id.emplace (context->spot_id,
                                           fw::detail::spot_context_access_t::create (context));
        queue = context->ensure_spot_serial_executor ()->actor_executor (key)->queue ();
    }

    ~join_claim_fixture_t ()
    {
        node->lane.run ([&] { node->spot_contexts_by_id.clear (); }).get ();
    }

    void drain ()
    {
        settlement->flush ();
        queue->drain ();
        executor->drain ();
    }
};

TEST (ActorMaintenanceJoinDeathTest, LocalHandoffSnapshotDoesNotWaitOnItsLaneExecutor)
{
    ASSERT_EXIT (
      {
          fw::runtime::install_host_context_hooks ();
          fw::runtime::configure_handler_coroutine_executor (1);
          join_claim_fixture_t fixture;
          auto release_workers = fw::test::reserve_single_infrastructure_worker (
            *fw::runtime::infrastructure_executor ());
          const auto claimed =
            fixture.node->actor_transfer_coordinator->try_begin_local ("player:claim-actor");
          if (!claimed)
              std::_Exit (2);
          auto completed = std::make_shared<std::promise<bool>> ();
          auto terminal = completed->get_future ();
          std::promise<void> entered;
          std::promise<void> release;
          auto released = release.get_future ();
          fixture.node->lane.try_post ([&] {
              entered.set_value ();
              released.wait ();
          });
          entered.get_future ().get ();
          auto owner = std::make_shared<fw::detail::spot_node_runtime_t> (fixture.node);
          auto opened = owner->open_local_actor_handoff (fixture.actor);
          fw::detail::observe_task_completion (
            opened, [owner, completed] (const fw::result_t<fw::result_t<void>> &result) {
                completed->set_value (result && result.value ());
            });
          // The snapshot queues its completion behind this executor turn. This
          // turn claims the next node drain behind that already queued completion.
          ::zlink::framework::runtime::infrastructure_executor ()->submit (
            [state = fixture.node] { state->lane.try_post ([] {}); });
          release.set_value ();
          if (terminal.wait_for (wait_budget) != std::future_status::ready)
              std::_Exit (3);
          std::_Exit (terminal.get () ? 0 : 4);
      },
      ::testing::ExitedWithCode (0), "");
}

TEST (ActorMaintenanceJoin, LocalHandoffCancellationRetainsQueuedLaneInputs)
{
    join_claim_fixture_t fixture;
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ();
    ASSERT_TRUE (fixture.node->lane.try_post ([&] {
        entered.set_value ();
        released.wait ();
    }));
    entered.get_future ().get ();
    std::stop_source cancellation;
    auto owner = std::make_unique<fw::detail::spot_node_runtime_t> (fixture.node);
    auto pending = [&] {
        fw::detail::ambient_context_scope_t scope (nullptr, nullptr, cancellation.get_token (),
                                                   false);
        return owner->open_local_actor_handoff (fixture.actor);
    }();
    cancellation.request_stop ();
    const auto stopped = fw::detail::observe_task_result_for (pending, wait_budget, {});
    std::promise<void> drained;
    auto lane_drained = drained.get_future ();
    EXPECT_TRUE (fixture.node->lane.try_post ([&] { drained.set_value (); }));
    owner.reset ();
    release.set_value ();
    lane_drained.get ();
    fixture.node->lane.run ([] {}).get ();
    fixture.drain ();
    ASSERT_TRUE (stopped);
    EXPECT_FALSE (*stopped);
    ASSERT_TRUE (stopped->error ());
    EXPECT_EQ (stopped->error ()->kind (), fw::framework_error_kind_t::shutting_down);
}

TEST (ActorMaintenanceJoin, DeferredJoinReleasesClaimBeforeNextActorTurn)
{
    join_claim_fixture_t fixture;
    auto reserved = fixture.owner.reserve_actor_join_barrier (fixture.actor);
    ASSERT_TRUE (reserved);
    std::promise<fw::detail::deferred_barrier_t::async_completion_t> callback;
    ASSERT_TRUE (reserved.value ()->activate_async (
      [&] (auto complete) { callback.set_value (std::move (complete)); }));
    auto complete = callback.get_future ().get ();
    EXPECT_FALSE (fixture.owner.reserve_actor_join_barrier (fixture.actor));
    std::promise<bool> next;
    EXPECT_TRUE (fixture.queue->try_post ("next-join-claim", [&] {
        auto claim = fixture.owner.reserve_actor_join_barrier (fixture.actor);
        if (claim)
            claim.value ()->cancel ();
        next.set_value (static_cast<bool> (claim));
    }));
    std::promise<void> lane_entered;
    std::promise<void> release_lane;
    auto lane_release = release_lane.get_future ().share ();
    ASSERT_TRUE (fixture.node->lane.try_post ([&] {
        lane_entered.set_value ();
        lane_release.wait ();
    }));
    lane_entered.get_future ().get ();
    auto finished =
      std::async (std::launch::async, [&] { complete (fw::result_t<void>::success ()); });
    // Settlement must yield to the occupied node lane while retaining the
    // Actor FIFO: neither this caller nor the next Actor turn may wait in it.
    const auto returned = finished.wait_for (wait_budget);
    auto next_claim = next.get_future ();
    const auto next_while_lane_held = next_claim.wait_for (0s);
    release_lane.set_value ();
    finished.get ();
    EXPECT_EQ (returned, std::future_status::ready);
    EXPECT_EQ (next_while_lane_held, std::future_status::timeout);
    const auto accepted = next_claim.get ();
    fixture.drain ();
    EXPECT_TRUE (accepted);
}

TEST (ActorMaintenanceJoin, DeferredJoinCompletionKeepsSuccessorClaim)
{
    join_claim_fixture_t fixture;
    auto reserved = fixture.owner.reserve_actor_join_barrier (fixture.actor);
    ASSERT_TRUE (reserved);
    std::promise<fw::detail::deferred_barrier_t::async_completion_t> callback;
    ASSERT_TRUE (reserved.value ()->activate_async (
      [&] (auto complete) { callback.set_value (std::move (complete)); }));
    auto complete = callback.get_future ().get ();
    // The local completion path has already handed its backlog to the FIFO,
    // while the enclosing deferred barrier has not published its terminal yet.
    EXPECT_TRUE (fixture.owner.open_local_actor_handoff (fixture.actor).result ().value ());
    auto successor = fixture.owner.reserve_actor_join_barrier (fixture.actor);
    EXPECT_TRUE (successor);
    complete (fw::result_t<void>::success ());
    auto unexpected = fixture.owner.reserve_actor_join_barrier (fixture.actor);
    const auto retained = !unexpected;
    if (unexpected)
        unexpected.value ()->cancel ();
    if (successor)
        successor.value ()->cancel ();
    fixture.drain ();
    EXPECT_TRUE (retained);
}

TEST (ActorMaintenanceJoin, DeferredJoinDiscardKeepsSuccessorClaim)
{
    join_claim_fixture_t fixture;
    auto reserved = fixture.owner.reserve_actor_join_barrier (fixture.actor);
    ASSERT_TRUE (reserved);
    EXPECT_TRUE (fixture.owner.open_local_actor_handoff (fixture.actor).result ().value ());
    auto successor = fixture.owner.reserve_actor_join_barrier (fixture.actor);
    EXPECT_TRUE (successor);
    reserved.value ()->cancel ();
    auto unexpected = fixture.owner.reserve_actor_join_barrier (fixture.actor);
    const auto retained = !unexpected;
    if (unexpected)
        unexpected.value ()->cancel ();
    if (successor)
        successor.value ()->cancel ();
    fixture.drain ();
    EXPECT_TRUE (retained);
}
} // namespace zlink::framework::test::actor_maintenance_join
