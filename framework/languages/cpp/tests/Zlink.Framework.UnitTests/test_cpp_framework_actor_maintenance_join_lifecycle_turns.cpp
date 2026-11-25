/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_observations.hpp"
#include "../support/actor_maintenance_join_host_access.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/actors/actor_manager_access.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
using zlink::framework::test::begin_reserved_object;

// Drain the queue before the locals its work items reference are destroyed, including on
// early ASSERT exits.
struct queue_drain_guard_t
{
    fw::runtime::serial_execution_queue_t &queue;
    std::shared_ptr<fw::detail::deferred_barrier_t> *barrier = nullptr;
    bool finished = false;
    ~queue_drain_guard_t ()
    {
        if (!finished && barrier && *barrier)
            (*barrier)->cancel ();
        queue.drain ();
    }
};

TEST (ActorMaintenanceJoin, ReservedLifecycleCaptureKeepsTurn)
{
    for (const bool early : {true, false}) {
        fw::runtime::offload_executor_t executor (2);
        fw::runtime::serial_execution_queue_t queue (executor, {});
        std::shared_ptr<fw::detail::deferred_barrier_t> barrier;
        std::optional<fw::task_t<void>> phase;
        std::shared_ptr<fw::detail::serial_turn_t> capture_turn;
        std::vector<int> order;
        fw::task_completion_source_t<void> initialized;
        queue_drain_guard_t guard{queue, &barrier};
        const auto capture = [&] () -> fw::task_t<void> {
            capture_turn = fw::detail::capture_current_serial_turn ();
            EXPECT_TRUE (capture_turn && capture_turn->belongs_to (&queue));
            order.push_back (1);
            co_return;
        };
        queue.post ("reserve-capture", [&] {
            barrier = queue.reserve_barrier_next ("relocation-ready", true).value ();
            if (early)
                phase = barrier->run (capture);
            initialized.complete (fw::result_t<void>::success ());
        });
        ASSERT_TRUE (initialized.task ().result_for (wait_budget));
        if (!early)
            phase = barrier->run (capture);
        const auto captured = phase->result_for (wait_budget);
        if (!captured || !*captured)
            barrier->cancel ();
        ASSERT_TRUE (captured && *captured);
        queue.post ("next-lifecycle", [&] { order.push_back (3); },
                    {fw::runtime::serial_work_lane_t::lifecycle});
        const auto resumed =
          barrier
            ->run ([&] () -> fw::task_t<void> {
                EXPECT_EQ (fw::detail::capture_current_serial_turn (), capture_turn);
                order.push_back (2);
                co_return;
            })
            .result_for (wait_budget);
        if (!resumed || !*resumed)
            barrier->cancel ();
        ASSERT_TRUE (resumed && *resumed);
        ASSERT_TRUE (barrier->activate_async ([&] (auto release) {
            EXPECT_EQ (fw::detail::capture_current_serial_turn (), capture_turn);
            release (fw::result_t<void>::success ());
        }));
        queue.drain ();
        guard.finished = true;
        EXPECT_EQ (order, (std::vector<int>{1, 2, 3}));
    }
}

TEST (ActorMaintenanceJoin, DeferredTaskRetainsLifecyclePositionUntilCompletion)
{
    fw::runtime::offload_executor_t executor (1);
    fw::runtime::serial_execution_queue_t queue (executor, {});
    queue_drain_guard_t guard{queue};
    fw::task_completion_source_t<void> release;
    std::promise<void> inspected;
    auto inspection = inspected.get_future ();
    std::atomic_bool next_ran{false};
    std::vector<int> order;
    queue.post ("handler", [&] {
        order.push_back (1);
        EXPECT_TRUE (
          queue.try_post_deferred_task ("destroy-after-handler", [&] () -> fw::task_t<void> {
              order.push_back (2);
              // Two FIFO worker markers also cover a next turn dispatched at this
              // callback's return; the held task must keep that turn unavailable.
              executor.submit ([&] { executor.submit ([&] { inspected.set_value (); }); });
              co_await release.task ();
              order.push_back (3);
          }));
        queue.post (
          "next-lifecycle",
          [&] {
              next_ran.store (true);
              order.push_back (4);
          },
          fw::runtime::serial_work_options_t{fw::runtime::serial_work_lane_t::lifecycle});
    });
    inspection.get ();
    EXPECT_FALSE (next_ran.load ());
    release.complete (fw::result_t<void>::success ());
    queue.drain ();
    EXPECT_EQ (order, (std::vector<int>{1, 2, 3, 4}));
}

TEST (ActorMaintenanceJoin, DeferredTaskKeepsInlineCallableCapturesAcrossSuspension)
{
    // A one-reference capture is small and trivially copyable, so std::function
    // keeps the closure in its own buffer on every supported standard library.
    // The coroutine frame refers to that closure by address after it resumes.
    struct deferred_state_t
    {
        fw::runtime::offload_executor_t &executor;
        fw::task_completion_source_t<void> release;
        std::promise<void> suspended;
        int marker = 7;
        int observed = 0;
    };
    fw::runtime::offload_executor_t executor (1);
    deferred_state_t state{executor};
    fw::runtime::serial_execution_queue_t queue (executor, {});
    queue_drain_guard_t guard{queue};
    auto suspension = state.suspended.get_future ();
    queue.post ("handler", [&] {
        EXPECT_TRUE (queue.try_post_deferred_task (
          "read-capture-after-resume", [&state] () -> fw::task_t<void> {
              // The nested FIFO markers run after the worker leaves the call that
              // started this task, so the release below resumes a suspended frame.
              auto &worker = state.executor;
              worker.submit (
                [&worker, &state] { worker.submit ([&state] { state.suspended.set_value (); }); });
              co_await state.release.task ();
              state.observed = state.marker;
          }));
    });
    suspension.get ();
    state.release.complete (fw::result_t<void>::success ());
    queue.drain ();
    EXPECT_EQ (state.observed, 7);
}

TEST (ActorMaintenanceJoin, CancelledReservedCaptureFinishes)
{
    fw::runtime::offload_executor_t executor (2);
    fw::runtime::serial_execution_queue_t queue (executor, {});
    std::optional<fw::task_t<void>> phase;
    bool ran = false;
    fw::task_completion_source_t<void> initialized;
    queue_drain_guard_t guard{queue};
    queue.post ("cancel-capture", [&] {
        const auto barrier = queue.reserve_barrier_next ("relocation-ready", true).value ();
        phase = barrier->run ([&] () -> fw::task_t<void> {
            ran = true;
            co_return;
        });
        barrier->cancel ();
        initialized.complete (fw::result_t<void>::success ());
    });
    ASSERT_TRUE (initialized.task ().result_for (wait_budget));
    const auto result = phase->result_for (wait_budget);
    ASSERT_TRUE (result.has_value ());
    EXPECT_FALSE (*result);
    EXPECT_FALSE (ran);
    queue.drain ();
}

TEST (ActorMaintenanceJoin, AsyncReplyRetainsOneTerminalWhilePreparationIsPending)
{
    namespace foundation = fw::runtime::foundation;
    foundation::operation_registry_t operations;
    const foundation::call_id_t operation{1, 1};
    std::promise<foundation::operation_terminal_t> completed;
    auto terminal = completed.get_future ();
    ASSERT_TRUE (operations.register_operation (
      operation, foundation::operation_registry_t::clock_t::now () + wait_budget,
      [&] (foundation::operation_terminal_t result,
           ::zlink::framework::result_t<zlink::message_t>) { completed.set_value (result); }));
    fw::task_completion_source_t<void> preparation;
    auto prepare = [&] () -> fw::task_t<void> { co_await preparation.task (); };
    ASSERT_TRUE (operations.complete_async (operation, prepare));
    EXPECT_FALSE (operations.complete_async (operation, prepare));
    EXPECT_FALSE (operations.cancel (operation));
    preparation.complete (fw::result_t<void>::success ());
    EXPECT_EQ (terminal.get (), foundation::operation_terminal_t::completed);
}

TEST (ActorMaintenanceJoin, CreateEntryJoinWaitsOutsideConfigurationTurn)
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
    auto actor = node
                   ->create_reserved_actor_async (actor_type, {sf::object_kind_t::actor, actor_id,
                                                               1, 1, mesh_name, rid.to_string ()})
                   .result ()
                   .value ();
    const sf::object_ref_t target{
      sf::object_kind_t::user_spot, access_t::entry_id (*node), 1, 1, mesh_name, rid.to_string ()};
    auto created = begin_reserved_object (node->objects (), target, room_type, {});
    ASSERT_EQ (node->objects ().commit_create (created.attempt), sf::stateful_error_t::none);
    host::pending_operation_t operation;
    auto manager = fw::detail::actor_manager_access_t::create (
      [&] (bool, fw::actor_id_t, std::string, std::optional<std::string>,
           std::optional<fw::message_t>, std::chrono::milliseconds timeout,
           fw::creation_operation_id_t) -> fw::task_t<fw::actor_create_result_t> {
          EXPECT_EQ (fw::runtime::state_lane_t::current (), nullptr);
          const auto submitted = co_await actor.join_entry_spot (rid, {}, operation, timeout);
          EXPECT_EQ (submitted, zlink::submit_result_t::ok);
          co_return fw::actor_create_rejected_t{};
      },
      {}, {}, {});
    auto release = access_t::hold_actor_index (*node);
    auto creating = manager.create (fw::actor_id_t (actor_id), actor_type).async ();
    EXPECT_FALSE (creating.await_ready ());
    release.set_value ();
    ASSERT_TRUE (creating.result ());
    node->close ();
}
} // namespace zlink::framework::test::actor_maintenance_join
