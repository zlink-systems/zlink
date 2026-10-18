/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/state_lane.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/manual_internal_scheduler.hpp"
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
#include "runtime/execution/infrastructure_wait_guard.hpp"
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace zlink::framework::detail
{
// The baseline adapter calls this immediately before its pending synchronous
// wait. The production implementation has no dependency on this observer.
std::function<void ()> suffix_blocking_wait_observer;
void observe_suffix_blocking_wait ()
{
    if (suffix_blocking_wait_observer)
        suffix_blocking_wait_observer ();
}
}

namespace
{

using zlink::framework::runtime::offload_executor_t;
using zlink::framework::runtime::state_lane_t;

struct move_constructible_snapshot_t
{
    explicit move_constructible_snapshot_t (int value_) : value (value_) {}
    move_constructible_snapshot_t (move_constructible_snapshot_t &&) noexcept = default;
    move_constructible_snapshot_t &operator= (move_constructible_snapshot_t &&) = delete;
    move_constructible_snapshot_t (const move_constructible_snapshot_t &) = delete;
    move_constructible_snapshot_t &operator= (const move_constructible_snapshot_t &) = delete;

    int value;
};

struct int_work_t
{
    int operator() ();
};

struct void_work_t
{
    void operator() ();
};

struct reference_work_t
{
    int &operator() ();
};

static_assert (
  std::is_same_v<decltype (std::declval<state_lane_t &> ().run (int_work_t{})), std::future<int>>);
static_assert (std::is_same_v<decltype (std::declval<state_lane_t &> ().run (void_work_t{})),
                              std::future<void>>);
static_assert (std::is_same_v<decltype (std::declval<state_lane_t &> ().run (reference_work_t{})),
                              std::future<int &>>);

TEST (ZLinkStateLane, RunReturnsTheResultOfTheWork)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);

    EXPECT_EQ (42, lane.run ([] { return 42; }).get ());
}

TEST (ZLinkStateLaneDeathTest, CleanupContinuationReleasesWorkerForPendingLaneAndAttachment)
{
    // A failed self-wait cannot unwind the executor. Isolate it so the bounded
    // observation reports a failure without joining the blocked worker.
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT (
      ([] {
          using namespace zlink::framework;
          using namespace zlink::framework::detail;
          constexpr auto completion_timeout = std::chrono::seconds (2);
          auto node = test::runtime_failure_fixture<spot_node_builder_state_t> ("cleanup-lane");
          spot_node_runtime_t spots (node);
          node->spot_factories.emplace ("attachment", std::type_index (typeid (void)));
          std::atomic<int> settled{0};
          node->pending_remote_actor_leaves.push_back (
            spot_node_builder_state_t::pending_remote_actor_leave_t{
              .transfer_id = "abandoned-transfer",
              .source_actor = actor_ref_access_t::make (node_rid_t::from_string ("cleanup-lane"),
                                                        "player", "actor", 1),
              .source_spot_id = spot_id_t ("source"),
              .target_spot_id = spot_id_t ("target"),
              .target_fence = {},
              .transfer_owner_reservation = [&] { ++settled; }});
          std::promise<void> snapshot_taken;
          auto snapshot = snapshot_taken.get_future ();
          std::promise<void> attachment_started;
          auto attaching = attachment_started.get_future ();
          std::promise<std::size_t> cleaned;
          auto cleanup = cleaned.get_future ();
          node->lane_executor.submit ([&] {
              auto pending = spots.cleanup_expired_actor_admissions_at_async (
                std::chrono::steady_clock::now (), [&] {
                    // The drain is queued behind this sole worker's continuation.
                    if (!node->lane.try_post ([] {}))
                        std::_Exit (2);
                    snapshot_taken.set_value ();
                    attaching.wait ();
                });
              observe_task_completion (pending, [&] (const result_t<std::size_t> &result) {
                  if (!result)
                      std::_Exit (3);
                  cleaned.set_value (result.value ());
              });
          });
          if (snapshot.wait_for (completion_timeout) != std::future_status::ready)
              std::_Exit (4);
          auto attachment = std::async (std::launch::async, [&] {
              attachment_started.set_value ();
              return spots.get_or_create_spot ("attachment", spot_id_t ("attachment"));
          });
          if (cleanup.wait_for (completion_timeout) != std::future_status::ready
              || attachment.wait_for (completion_timeout) != std::future_status::ready) {
              std::fputs ("cleanup/attachment blocked on the state lane\n", stderr);
              std::_Exit (5);
          }
          if (cleanup.get () != 1 || settled != 1)
              std::_Exit (6);
          if (attachment.get ().state != spot_create_state_t::created)
              std::_Exit (7);
          node->lane_executor.drain ();
          std::_Exit (0);
      }()),
      ::testing::ExitedWithCode (0), "");
}

TEST (ZLinkStateLane, ActorTransferUsesTheNodeOwnerForQueriesAndAsyncMutations)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    test::manual_internal_scheduler_t scheduler;
    runtime::state_lane_t node_lane (scheduler);
    actor_transfer_coordinator_t coordinator (scheduler, node_lane);
    int activity = 0;
    bool activity_on_node = false;
    coordinator.set_activity_handler ([&] {
        ++activity;
        activity_on_node = runtime::state_lane_t::current () == &node_lane;
    });
    auto turn = node_lane.run ([&] {
        EXPECT_TRUE (coordinator.try_reserve_source ("player:coowner", "coowner-transfer"));
        EXPECT_FALSE (coordinator.blocks_dispatch ("player:coowner"));
        EXPECT_TRUE (coordinator.try_begin_local ("player:coowner"));
        EXPECT_TRUE (coordinator.blocks_dispatch ("player:coowner"));
        EXPECT_EQ (coordinator.phase ("player:coowner"), actor_move_phase_t::local);
        std::vector<handoff_packet_t> prefix (1);
        prefix.front ().packet_name = "coowner-prefix";
        auto prepended = coordinator.prepend_source_suffix ("player:coowner", std::move (prefix));
        EXPECT_TRUE (prepended.await_ready ());
        EXPECT_TRUE (prepended.result ());
        auto cleanup =
          coordinator.cleanup_expired_async (std::chrono::steady_clock::now (), {"player:coowner"});
        EXPECT_TRUE (cleanup.await_ready ());
        ASSERT_EQ (cleanup.result ().value ().blocked_dispatch_keys.size (), 1u);
        auto backlog = coordinator.take_backlog_async ("player:coowner");
        EXPECT_TRUE (backlog.await_ready ());
        ASSERT_EQ (backlog.result ().value ().size (), 1u);
        EXPECT_EQ (backlog.result ().value ().front ().packet_name, "coowner-prefix");
        auto no_source_wait = coordinator.wait_for_source_completion ("player:coowner");
        EXPECT_TRUE (no_source_wait.await_ready ());
        EXPECT_TRUE (no_source_wait.result ());
        coordinator.cancel_move ("player:coowner");
        EXPECT_FALSE (coordinator.blocks_dispatch ("player:coowner"));
    });
    EXPECT_NO_THROW (turn.get ());
    EXPECT_EQ (activity, 1);
    EXPECT_TRUE (activity_on_node);
    scheduler.retire ();
}

TEST (ZLinkStateLane, ActorSuffixReturnsPendingBeforeTheOwnerTurnIsReleased)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    zlink::framework::runtime::offload_executor_t coordinator_executor;
    zlink::framework::runtime::state_lane_t coordinator_lane (coordinator_executor);
    actor_transfer_coordinator_t coordinator (coordinator_executor, coordinator_lane);
    ASSERT_TRUE (coordinator.try_begin_local ("player:actor"));
    handoff_packet_t tail;
    tail.packet_name = "tail";
    ASSERT_EQ (handoff_append_result_t::appended,
               coordinator.try_append_backlog ("player:actor", tail));
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ().share ();
    coordinator.set_activity_handler ([&] {
        entered.set_value ();
        released.wait ();
    });
    std::thread holder (
      [&] { coordinator.mark_reconcile ("holder", std::chrono::steady_clock::duration::zero ()); });
    entered.get_future ().get ();

    std::promise<bool> first_observation;
    auto observed = first_observation.get_future ();
    std::atomic_bool observation_sent{false};
    const auto observe = [&] (bool returned_pending) {
        if (!observation_sent.exchange (true))
            first_observation.set_value (returned_pending);
    };
    suffix_blocking_wait_observer = [&] { observe (false); };
    std::promise<bool> completed;
    auto terminal = completed.get_future ();
    std::thread caller ([&] {
        std::vector<handoff_packet_t> suffix (2);
        suffix[0].packet_name = "first";
        suffix[1].packet_name = "second";
        auto pending = coordinator.prepend_source_suffix ("player:actor", std::move (suffix));
        observe (!pending.await_ready ());
        completed.set_value (static_cast<bool> (pending.result ()));
    });
    const bool returned_before_release = observed.get ();
    release.set_value ();
    holder.join ();
    caller.join ();
    suffix_blocking_wait_observer = {};
    EXPECT_TRUE (returned_before_release);
    EXPECT_TRUE (terminal.get ());
    const auto backlog = coordinator.take_backlog ("player:actor");
    ASSERT_EQ (3U, backlog.size ());
    EXPECT_EQ ("first", backlog[0].packet_name);
    EXPECT_EQ ("second", backlog[1].packet_name);
    EXPECT_EQ ("tail", backlog[2].packet_name);
}

TEST (ZLinkStateLane, ActorMailboxLookupAndReadyReleaseTheOnlyLaneWorker)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    auto node = test::runtime_failure_fixture<spot_node_builder_state_t> ("actor-ready-lane");
    spot_node_runtime_t spots (node);
    std::promise<bool> lookup_completed;
    std::promise<bool> ready_completed;
    auto lookup_result = lookup_completed.get_future ();
    auto ready_result = ready_completed.get_future ();
    std::atomic<int> takes{0};
    node->lane_executor.submit ([&] {
        // The only lane worker still owns this callback. Its posted predecessor
        // cannot drain until both asynchronous lookups have returned to it.
        if (!node->lane.try_post ([] {})) {
            lookup_completed.set_value (false);
            ready_completed.set_value (false);
            return;
        }
        auto lookup = std::make_shared<task_t<std::shared_ptr<runtime::serial_execution_queue_t>>> (
          spots.actor_application_queue ("missing"));
        const bool lookup_pending = !lookup->await_ready ();
        observe_task_completion (
          *lookup, [lookup, lookup_pending, &lookup_completed] (const auto &terminal) {
              lookup_completed.set_value (lookup_pending && terminal && !terminal.value ());
          });
        auto ready = std::make_shared<task_t<bool>> (spots.notify_actor_application_ready (
          "missing", [&] () -> std::optional<runtime::serial_prefix_work_t> {
              ++takes;
              return std::nullopt;
          }));
        const bool ready_pending = !ready->await_ready ();
        observe_task_completion (
          *ready, [ready, ready_pending, &ready_completed] (const result_t<bool> &terminal) {
              ready_completed.set_value (ready_pending && terminal && !terminal.value ());
          });
    });
    EXPECT_TRUE (lookup_result.get ());
    EXPECT_TRUE (ready_result.get ());
    node->lane_executor.drain ();
    EXPECT_EQ (0, takes.load ());
}

TEST (ZLinkStateLane, ActorMailboxTasksRetainSubmittedHandleStateAndActorId)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    auto node = test::runtime_failure_fixture<spot_node_builder_state_t> ("mailbox-lifetime");
    auto replacement =
      test::runtime_failure_fixture<spot_node_builder_state_t> ("replacement-lifetime");
    auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
    scheduler->publication_fence (
      test::state_lane_publication_fence (node->lane, node->lane_executor));
    node->worker_executor = scheduler;
    auto spot = std::make_shared<spot_context_state_t> ();
    spot->lane_owner = node;
    spot->spot_id = spot_id_t ("mailbox-spot");
    node->actor_types_by_id.emplace ("actor", "player");
    node->actor_spot_ids.emplace ("player:actor", spot->spot_id);
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));
    auto expected_queue =
      node->lane
        .run ([&] {
            return spot->ensure_spot_serial_executor ()->actor_executor ("player:actor")->queue ();
        })
        .get ();
    scheduler->run_all ();

    spot_node_runtime_t lookup_handle (node);
    spot_node_runtime_t ready_handle (node);
    std::string actor_id = "actor";
    std::promise<bool> lookup_completed;
    std::promise<bool> ready_completed;
    auto lookup_result = lookup_completed.get_future ();
    auto ready_result = ready_completed.get_future ();
    std::atomic<int> takes{0};
    node->lane_executor.submit ([&] {
        if (!node->lane.try_post ([] {})) {
            lookup_completed.set_value (false);
            ready_completed.set_value (false);
            return;
        }
        auto lookup = std::make_shared<task_t<std::shared_ptr<runtime::serial_execution_queue_t>>> (
          lookup_handle.actor_application_queue (actor_id));
        const bool lookup_pending = !lookup->await_ready ();
        observe_task_completion (*lookup, [lookup, lookup_pending, expected_queue,
                                           &lookup_completed] (const auto &terminal) {
            lookup_completed.set_value (lookup_pending && terminal
                                        && terminal.value () == expected_queue);
        });
        auto ready = std::make_shared<task_t<bool>> (ready_handle.notify_actor_application_ready (
          "actor", [&] () -> std::optional<runtime::serial_prefix_work_t> {
              ++takes;
              return std::nullopt;
          }));
        const bool ready_pending = !ready->await_ready ();
        observe_task_completion (
          *ready, [ready, ready_pending, &ready_completed] (const result_t<bool> &terminal) {
              ready_completed.set_value (ready_pending && terminal && terminal.value ());
          });
        // These are still-live handles. Reassignment and a same-size ID mutation
        // expose borrowed inputs without dereferencing a destroyed object.
        lookup_handle = spot_node_runtime_t (replacement);
        ready_handle = spot_node_runtime_t (replacement);
        actor_id.assign ("other");
    });
    EXPECT_TRUE (lookup_result.get ());
    EXPECT_TRUE (ready_result.get ());
    scheduler->run_all ();
    EXPECT_EQ (1, takes.load ());
    expected_queue.reset ();
    node->lane.run ([&] { node->spot_contexts_by_id.clear (); }).get ();
    spot.reset ();
    scheduler->retire ();
    node->lane_executor.drain ();
}

TEST (ZLinkStateLane, ActorReadyRevalidationFailureReachesRuntimeCollector)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    auto node = test::runtime_failure_fixture<spot_node_builder_state_t> ("actor-ready-failure");
    auto scheduler = std::make_shared<test::manual_internal_scheduler_t> ();
    node->worker_executor = scheduler;
    auto spot = std::make_shared<spot_context_state_t> ();
    spot->lane_owner = node;
    spot->spot_id = spot_id_t ("ready-failure-spot");
    node->actor_types_by_id.emplace ("actor", "player");
    node->actor_spot_ids.emplace ("player:actor", spot->spot_id);
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));
    auto serial = node->lane.run ([&] { return spot->ensure_spot_serial_executor (); }).get ();
    scheduler->run_all ();
    spot_node_runtime_t spots (node);
    unsigned takes = 0;
    auto ready = spots.notify_actor_application_ready (
      "actor", [&] () -> std::optional<runtime::serial_prefix_work_t> {
          ++takes;
          return std::nullopt;
      });
    ASSERT_TRUE (ready.result ().value ());
    EXPECT_EQ (0u, takes);
    auto queue =
      node->lane.run ([&] { return serial->actor_executor ("player:actor")->queue (); }).get ();
    auto root = serial->spot_queue ();

    // Publication succeeds before the lane closes. The queued actor-ready
    // control must report its failed membership revalidation without terminating.
    node->lane.close ();
    EXPECT_TRUE (scheduler->run_one ());
    scheduler->run_all ();
    EXPECT_EQ (0u, takes);
    try {
        node->runtime_failures ()->rethrow_if_failed ();
        ADD_FAILURE () << "actor-ready revalidation failure was not reported";
    }
    catch (const std::exception &error) {
        EXPECT_STREQ ("state lane is closed", error.what ());
    }

    // The group close needs its state lane. This fixture deliberately closed
    // that lane first, so finish the already-retained queue resources directly.
    queue->close ();
    root->close ();
    queue->cancel_pending ();
    root->cancel_pending ();
    scheduler->run_all ();
    queue->drain ();
    root->drain ();
    node->spot_contexts_by_id.clear ();
    spot.reset ();
    serial.reset ();
    scheduler->retire ();
    node->lane_executor.drain ();
}

TEST (ZLinkStateLane, CallerDrainedTaskDoesNotWaitForUnrelatedExecutorWork)
{
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ();
    executor.submit ([&] {
        entered.set_value ();
        released.wait ();
    });
    entered.get_future ().wait ();
    auto task = lane.run_task ([&] {
        EXPECT_TRUE (lane.is_on_lane ());
        return 42;
    });
    const auto ready = task.result_for (std::chrono::milliseconds::zero ());
    release.set_value ();
    ASSERT_TRUE (ready.has_value ());
    EXPECT_EQ (42, ready->value ());
    EXPECT_EQ (nullptr, state_lane_t::current ());
}

zlink::framework::task_t<int> wrap_completion_task (zlink::framework::task_t<int> task)
{
    co_return co_await task;
}

class stopped_completion_executor_t final : public offload_executor_t
{
  public:
    bool try_submit_internal (std::function<void ()> work) override
    {
        if (_drain)
            return false;
        _drain = std::move (work);
        return true;
    }

    void finish_accepted_drain () { _drain (); }

  private:
    std::function<void ()> _drain;
};

TEST (ZLinkStateLane, RejectedCompletionDoesNotResumeCallerInsideTheLane)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    configure_handler_coroutine_executor (1);
    install_host_context_hooks ();
    stopped_completion_executor_t executor;
    state_lane_t lane (executor);
    ASSERT_TRUE (lane.try_post ([] {}));
    auto pending = lane.run_task ([] { return 42; });
    auto caller = wrap_completion_task (std::move (pending));
    bool resumed_inside_lane = false;
    detail::observe_task_terminal (
      caller, [&] (const auto &) { resumed_inside_lane = state_lane_t::current () != nullptr; });
    executor.finish_accepted_drain ();
    shutdown_handler_coroutine_executor ();
    EXPECT_FALSE (resumed_inside_lane);
    EXPECT_EQ (framework_error_kind_t::shutting_down, caller.result ().error_kind ());
}

TEST (ZLinkStateLane, PendingResultOutsideLaneDoesNotWaitForAnotherExecutor)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    configure_handler_coroutine_executor (1);
    install_host_context_hooks ();
    std::promise<void> host_entered;
    std::promise<void> host_release;
    auto host_released = host_release.get_future ();
    handler_coroutine_executor ().post_native_continuation ([&] {
        host_entered.set_value ();
        host_released.wait ();
    });
    host_entered.get_future ().wait ();
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    std::promise<void> lane_entered;
    std::promise<void> lane_release;
    auto lane_released = lane_release.get_future ();
    ASSERT_TRUE (lane.try_post ([&] {
        lane_entered.set_value ();
        lane_released.wait ();
    }));
    lane_entered.get_future ().wait ();
    auto caller = wrap_completion_task (lane.run_task ([] { return 42; }));
    lane_release.set_value ();
    executor.drain ();
    const auto ready = caller.result_for (std::chrono::milliseconds::zero ());
    host_release.set_value ();
    shutdown_handler_coroutine_executor ();
    ASSERT_TRUE (ready.has_value ());
    EXPECT_EQ (42, ready->value ());
}

TEST (ZLinkStateLane, OperationDispatcherCompletesInternalWrappersWithoutAnotherTurn)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    configure_handler_coroutine_executor (1);
    install_host_context_hooks ();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ();
    handler_coroutine_executor ().post_native_continuation ([&] {
        entered.set_value ();
        released.wait ();
    });
    entered.get_future ().wait ();
    foundation::operation_registry_t registry;
    task_completion_source_t<int> source;
    auto task = wrap_completion_task (wrap_completion_task (source.task ()));
    std::thread::id dispatcher_thread;
    std::thread::id caller_thread;
    detail::observe_task_terminal (task, [&] (const auto &) {
        EXPECT_EQ (nullptr, state_lane_t::current ());
        caller_thread = std::this_thread::get_id ();
    });
    const call_id_t id{1, 1};
    ASSERT_TRUE (registry.register_operation (
      id, foundation::operation_registry_t::clock_t::time_point::max (), [&] (auto, auto) {
          dispatcher_thread = std::this_thread::get_id ();
          source.complete (result_t<int>::success (42));
      }));
    ASSERT_TRUE (registry.complete (id, {}));
    registry.shutdown ();
    const auto ready = task.result_for (std::chrono::milliseconds::zero ());
    release.set_value ();
    shutdown_handler_coroutine_executor ();
    ASSERT_TRUE (ready.has_value ());
    EXPECT_EQ (42, ready->value ());
    EXPECT_EQ (dispatcher_thread, caller_thread);
}

TEST (ZLinkStateLane, CallerDrainedVoidAndFailureTasksLeaveTheLaneBeforeCompletion)
{
    using namespace zlink::framework;
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    EXPECT_TRUE (lane.run_task ([] {}).result ().has_value ());
    EXPECT_EQ (nullptr, state_lane_t::current ());
    auto failed = lane.run_task ([] () -> int {
        throw framework_exception_t (framework_error_kind_t::not_configured, "not configured");
    });
    EXPECT_EQ (framework_error_kind_t::not_configured, failed.result ().error_kind ());
    EXPECT_EQ (nullptr, state_lane_t::current ());
    detail::observe_task_completion (failed, [&] (const auto &) {
        EXPECT_EQ (42, lane.run_task ([] { return 42; }).result ().value ());
    });
}

TEST (ZLinkStateLane, StandaloneTaskCompletionAllowsLaneReentry)
{
    constexpr auto completion_timeout = std::chrono::seconds (5);
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ();
    lane.try_post ([&] {
        entered.set_value ();
        released.wait ();
    });
    entered.get_future ().wait ();
    auto task = lane.run_task ([] { return 42; });
    std::promise<int> observed;
    auto result = observed.get_future ();
    zlink::framework::detail::observe_task_completion (
      task, [&] (const zlink::framework::result_t<int> &value) {
          EXPECT_EQ (nullptr, state_lane_t::current ());
          observed.set_value (lane.run ([&] { return value.value (); }).get ());
      });
    release.set_value ();
    ASSERT_EQ (std::future_status::ready, result.wait_for (completion_timeout));
    EXPECT_EQ (42, result.get ());
}

#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
TEST (ZLinkStateLane, DebugWaitRejectsPendingOwnerCompletion)
{
    ASSERT_DEATH (
      {
          offload_executor_t executor (1);
          state_lane_t lane (executor);
          std::promise<void> entered;
          std::promise<void> release;
          auto released = release.get_future ();
          lane.try_post ([&] {
              entered.set_value ();
              released.wait ();
          });
          entered.get_future ().wait ();
          zlink::framework::runtime::infrastructure_wait_guard::infrastructure_scope_t scope (
            &lane);
          lane.run_checked ([] { return 7; }).get ();
      },
      "infrastructure wait guard: state-lane/get");
}

TEST (ZLinkStateLane, DebugWaitAcceptsReadyOwnerCompletion)
{
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    EXPECT_EQ (7, lane.run_checked ([] { return 7; }).get ());
}

TEST (ZLinkStateLane, DebugWaitRejectsPendingCompletionFromCurrentLane)
{
    ASSERT_DEATH (
      {
          offload_executor_t executor (1);
          state_lane_t lane (executor);
          std::promise<void> pending;
          auto future = pending.get_future ();
          lane
            .run ([&] {
                zlink::framework::runtime::state_lane_internal::wait (future,
                                                                      "state-lane/pending-test");
            })
            .get ();
      },
      "infrastructure wait guard: state-lane/pending-test");
}
#endif

TEST (ZLinkStateLane, AnAvailableLaneDoesNotWaitForUnrelatedExecutorWork)
{
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ();
    executor.submit ([&] {
        entered.set_value ();
        released.wait ();
    });
    entered.get_future ().wait ();

    auto result = lane.run ([&] {
        EXPECT_TRUE (lane.is_on_lane ());
        return 42;
    });
    const auto ready = result.wait_for (std::chrono::milliseconds::zero ());
    release.set_value ();

    EXPECT_EQ (std::future_status::ready, ready);
    EXPECT_EQ (42, result.get ());
    EXPECT_EQ (nullptr, state_lane_t::current ());
}

TEST (ZLinkStateLane, ConcurrentSubmissionsKeepTheirPlaceBehindTheCurrentTurn)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ();
    std::vector<int> order;
    std::thread owner ([&] {
        lane
          .run ([&] {
              entered.set_value ();
              released.wait ();
              order.push_back (0);
          })
          .get ();
    });
    entered.get_future ().wait ();
    auto second = lane.run ([&] { order.push_back (1); });
    auto third = lane.run ([&] { order.push_back (2); });
    release.set_value ();
    second.get ();
    third.get ();
    owner.join ();

    EXPECT_EQ ((std::vector<int>{0, 1, 2}), order);
    EXPECT_EQ (nullptr, state_lane_t::current ());
}

TEST (ZLinkStateLane, TryPostKeepsItsNoWaitExecutionContract)
{
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ();
    executor.submit ([&] {
        entered.set_value ();
        released.wait ();
    });
    entered.get_future ().wait ();
    std::atomic_bool called = false;

    EXPECT_TRUE (lane.try_post ([&] { called = true; }));
    const auto called_before_release = called.load ();
    release.set_value ();
    lane.close ();

    EXPECT_FALSE (called_before_release);
    EXPECT_TRUE (called.load ());
}

TEST (ZLinkStateLane, RunTransfersAMoveConstructibleNonAssignableSnapshot)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);

    auto snapshot = lane.run ([] { return move_constructible_snapshot_t (17); }).get ();

    EXPECT_EQ (17, snapshot.value);
}

TEST (ZLinkStateLane, RunSurfacesAFailureToItsOwnCaller)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);

    auto result = lane.run ([] () -> int { throw std::invalid_argument ("boom"); });
    EXPECT_THROW (result.get (), std::invalid_argument);
}

TEST (ZLinkStateLane, RunKeepsServingAfterAWorkItemThrows)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);

    auto failed = lane.run ([] () -> int { throw std::invalid_argument ("boom"); });
    EXPECT_THROW (failed.get (), std::invalid_argument);
    EXPECT_EQ (7, lane.run ([] { return 7; }).get ());
}

TEST (ZLinkStateLane, ConcurrentCallersMutateUnsynchronizedStateWithoutLosingUpdates)
{
    offload_executor_t executor (4);
    state_lane_t lane (executor);
    std::map<int, int> state;
    constexpr int callers = 32;
    constexpr int per_caller = 50;
    std::vector<std::thread> threads;
    threads.reserve (callers);

    for (int caller = 0; caller < callers; ++caller) {
        threads.emplace_back ([&lane, &state, caller] {
            for (int index = 0; index < per_caller; ++index) {
                const auto key = (caller * per_caller) + index;
                lane.run ([&state, key] { state[key] = key; }).get ();
            }
        });
    }
    for (auto &thread : threads) {
        thread.join ();
    }

    EXPECT_EQ (callers * per_caller, lane.run ([&state] { return state.size (); }).get ());
}

TEST (ZLinkStateLane, WorkItemsNeverOverlap)
{
    offload_executor_t executor (4);
    state_lane_t lane (executor);
    std::atomic_int in_flight = 0;
    std::atomic_bool observed_overlap = false;
    std::vector<std::thread> threads;

    for (int index = 0; index < 64; ++index) {
        threads.emplace_back ([&] {
            lane
              .run ([&] {
                  if (++in_flight != 1) {
                      observed_overlap = true;
                  }
                  std::this_thread::yield ();
                  --in_flight;
              })
              .get ();
        });
    }
    for (auto &thread : threads) {
        thread.join ();
    }

    EXPECT_FALSE (observed_overlap.load ());
}

TEST (ZLinkStateLane, PostsFromOneCallerRunInPostOrder)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);
    std::vector<int> order;

    for (int value = 0; value < 100; ++value) {
        EXPECT_TRUE (lane.try_post ([&order, value] { order.push_back (value); }));
    }

    std::vector<int> expected (100);
    std::iota (expected.begin (), expected.end (), 0);
    EXPECT_EQ (expected, lane.run ([&order] { return order; }).get ());
}

TEST (ZLinkStateLane, DrainingMoreThanOneBatchStillRunsEveryItem)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);
    int count = 0;

    for (int index = 0; index < 250; ++index) {
        ASSERT_TRUE (lane.try_post ([&count] { ++count; }));
    }

    EXPECT_EQ (250, lane.run ([&count] { return count; }).get ());
}

TEST (ZLinkStateLane, ReenteringTheSameLaneFailsInsteadOfHanging)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);

    const auto message = lane
                           .run ([&lane] {
                               try {
                                   (void) lane.run ([] { return 1; });
                               }
                               catch (const std::logic_error &error) {
                                   return std::string (error.what ());
                               }
                               return std::string ();
                           })
                           .get ();

    EXPECT_NE (std::string::npos, message.find ("already runs on the state lane"));
}

TEST (ZLinkStateLane, IsOnLaneIsTrueOnlyInsideATurn)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);

    EXPECT_FALSE (lane.is_on_lane ());
    EXPECT_TRUE (lane.run ([&lane] { return lane.is_on_lane (); }).get ());
    EXPECT_FALSE (lane.is_on_lane ());
}

TEST (ZLinkStateLane, ADifferentLaneIsEnterableFromInsideATurn)
{
    offload_executor_t executor (2);
    state_lane_t outer (executor);
    state_lane_t inner (executor);

    EXPECT_EQ (5, outer.run ([&inner] { return inner.run ([] { return 5; }).get (); }).get ());
}

TEST (ZLinkStateLane, CloseWaitsForQueuedWork)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);
    int completed = 0;

    for (int index = 0; index < 200; ++index) {
        ASSERT_TRUE (lane.try_post ([&completed] { ++completed; }));
    }
    lane.close ();

    EXPECT_EQ (200, completed);
}

TEST (ZLinkStateLane, CloseRejectsNewWorkWhileDrainingAcceptedTurns)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ();
    std::vector<int> order;
    std::thread owner ([&] {
        lane
          .run ([&] {
              entered.set_value ();
              released.wait ();
              order.push_back (0);
          })
          .get ();
    });
    entered.get_future ().wait ();
    EXPECT_TRUE (lane.try_post ([&] { order.push_back (1); }));
    std::thread closer ([&] { lane.close (); });
    while (!lane.closed ()) {
        std::this_thread::yield ();
    }
    EXPECT_FALSE (lane.try_post ([&] { order.push_back (2); }));
    release.set_value ();
    owner.join ();
    closer.join ();

    EXPECT_EQ ((std::vector<int>{0, 1}), order);
}

TEST (ZLinkStateLane, RunAfterCloseThrows)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);
    lane.close ();

    EXPECT_THROW ((void) lane.run ([] { return 1; }), std::runtime_error);
}

TEST (ZLinkStateLane, TryPostAfterCloseReportsRefusalInsteadOfThrowing)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);
    lane.close ();

    EXPECT_FALSE (lane.try_post ([] {}));
}

TEST (ZLinkStateLane, CloseIsIdempotent)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);

    lane.close ();
    EXPECT_NO_THROW (lane.close ());
}

} // namespace

TEST (ZLinkStateLane, AsyncPrefixRetainsMailboxUntilFactoryAndMutationFinish)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    test::manual_internal_scheduler_t scheduler;
    auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
    task_completion_source_t<void> release;
    std::vector<int> order;
    auto adoption =
      queue->adopt_application_prefix ([&] () -> task_t<std::vector<serial_prefix_work_t>> {
          co_await release.task ();
          serial_prefix_work_t item;
          item.name = "async-prefix";
          item.work = [&] (auto complete) { complete ([&] { order.push_back (1); }); };
          std::vector<serial_prefix_work_t> prefix;
          prefix.push_back (std::move (item));
          co_return prefix;
      });
    EXPECT_FALSE (adoption.await_ready ());
    queue->post ("ordinary-suffix", [&] { order.push_back (2); });
    scheduler.run_all ();
    EXPECT_TRUE (order.empty ());
    release.complete (result_t<void>::success ());
    scheduler.run_until (adoption);
    ASSERT_TRUE (adoption.result ());
    EXPECT_TRUE (adoption.result ().value ());
    scheduler.run_all ();
    EXPECT_EQ (order, (std::vector<int>{1, 2}));
    queue->close ();
    queue->drain ();
    scheduler.retire ();
}

TEST (ZLinkStateLane, AsyncPrefixCloseOrCancelSettlesOnlyItsRejectedAcceptedRecords)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    for (const bool cancel : {false, true}) {
        test::manual_internal_scheduler_t scheduler;
        auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
        task_completion_source_t<void> release;
        int initial_returns = 0, retired = 0, prefix_cancelled = 0, native_terminals = 0;
        int suffix_ran = 0, suffix_cancelled = 0;
        auto record = std::make_shared<serial_application_record_t> ();
        record->admission.retain_initial ([&] { ++initial_returns; });
        record->retire_invocation = [&] (const result_t<void> &outcome) {
            EXPECT_FALSE (outcome);
            ++retired;
        };
        record->native_reply = [&] (auto) {
            ++native_terminals;
            return true;
        };
        auto adoption =
          queue->adopt_application_prefix ([&] () -> task_t<std::vector<serial_prefix_work_t>> {
              co_await release.task ();
              serial_prefix_work_t item;
              item.name = "closing-prefix";
              item.options.application_record = record;
              item.work = [] (auto complete) { complete ([] {}); };
              item.cancel = [&] {
                  ++prefix_cancelled;
                  (void) record->native_reply (std::nullopt);
              };
              std::vector<serial_prefix_work_t> prefix;
              prefix.push_back (std::move (item));
              co_return prefix;
          });
        ASSERT_TRUE (queue->try_post_cancellable_async (
          "existing-suffix", [&] (auto complete) { complete ([&] { ++suffix_ran; }); },
          [&] { ++suffix_cancelled; }));
        scheduler.run_all ();
        EXPECT_EQ (suffix_ran, 0);
        if (cancel) {
            queue->cancel_pending ();
            scheduler.run_all ();
            queue->drain ();
        } else {
            queue->close ();
        }
        release.complete (result_t<void>::success ());
        scheduler.run_until (adoption);
        ASSERT_TRUE (adoption.result ());
        EXPECT_FALSE (adoption.result ().value ());
        scheduler.run_all ();
        EXPECT_EQ (initial_returns, 1);
        EXPECT_EQ (retired, 1);
        EXPECT_EQ (prefix_cancelled, 1);
        EXPECT_EQ (native_terminals, 1);
        EXPECT_EQ (suffix_ran, cancel ? 0 : 1);
        EXPECT_EQ (suffix_cancelled, cancel ? 1 : 0);
        queue->drain ();
        scheduler.retire ();
    }
}

TEST (ZLinkStateLane, AsyncPrefixFactoryFailureReleasesMailboxAndPreservesError)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    test::manual_internal_scheduler_t scheduler;
    auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
    task_completion_source_t<void> release;
    int suffix = 0;
    auto adoption =
      queue->adopt_application_prefix ([&] () -> task_t<std::vector<serial_prefix_work_t>> {
          co_await release.task ();
          throw std::runtime_error ("prefix factory failed");
          co_return std::vector<serial_prefix_work_t>{};
      });
    queue->post ("after-prefix-failure", [&] { ++suffix; });
    scheduler.run_all ();
    EXPECT_EQ (suffix, 0);
    release.complete (result_t<void>::success ());
    scheduler.run_until (adoption);
    EXPECT_FALSE (adoption.result ());
    try {
        adoption.result ().value ();
    }
    catch (const std::runtime_error &error) {
        EXPECT_STREQ (error.what (), "prefix factory failed");
    }
    scheduler.run_all ();
    EXPECT_EQ (suffix, 1);
    queue->close ();
    queue->drain ();
    scheduler.retire ();
}

TEST (ZLinkStateLane, AsyncPrefixRetiredWorkerPreservesRejectedAdmission)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    test::manual_internal_scheduler_t scheduler;
    auto jobs = std::make_shared<application_job_queue_t> (
      application_job_queue_configuration_t{application_job_queue_profile_t::balanced, 1u, 1, 1});
    auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
    scheduler.retire ();
    auto record = std::make_shared<serial_application_record_t> ();
    record->payload.emplace ();
    record->admission.jobs = jobs;
    int cancelled = 0, returned = 0;
    record->admission.retain_initial ([&] { ++returned; });
    auto adoption =
      queue->adopt_application_prefix ([&] () -> task_t<std::vector<serial_prefix_work_t>> {
          serial_prefix_work_t item;
          item.name = "retired-prefix";
          item.options.application_record = record;
          item.work = [] (auto complete) { complete ([] {}); };
          item.cancel = [&] { ++cancelled; };
          std::vector<serial_prefix_work_t> prefix;
          prefix.push_back (std::move (item));
          co_return prefix;
      });
    scheduler.run_until (adoption);
    ASSERT_TRUE (adoption.result ());
    EXPECT_FALSE (adoption.result ().value ());
    EXPECT_EQ (cancelled, 1);
    EXPECT_EQ (returned, 1);
    EXPECT_EQ (queue->pending_count (), 0u);
    jobs->stop ();
    queue->cancel_pending ();
    scheduler.run_all ();
    queue->drain ();
}

TEST (ZLinkStateLane, ExternalPrefixAdmissionDoesNotWaitForAnExistingHandlerTerminal)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    test::manual_internal_scheduler_t scheduler;
    auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
    serial_execution_queue_t::async_completion_t finish_handler;
    std::vector<int> order;
    queue->post_async ("existing-handler",
                       [&] (auto complete) { finish_handler = std::move (complete); });
    scheduler.run_all ();
    ASSERT_TRUE (finish_handler);
    queue->post ("existing-suffix", [&] { order.push_back (2); });
    auto adoption =
      queue->adopt_application_prefix ([&] () -> task_t<std::vector<serial_prefix_work_t>> {
          serial_prefix_work_t item;
          item.name = "external-prefix";
          item.work = [&] (auto complete) { complete ([&] { order.push_back (1); }); };
          std::vector<serial_prefix_work_t> prefix;
          prefix.push_back (std::move (item));
          co_return prefix;
      });
    // The existing runner owns its own future M; this admission must finish
    // while that handler is still deliberately held by the fixture.
    EXPECT_TRUE (adoption.await_ready ());
    EXPECT_TRUE (order.empty ());
    finish_handler ([] {});
    scheduler.run_until (adoption);
    ASSERT_TRUE (adoption.result ());
    EXPECT_TRUE (adoption.result ().value ());
    scheduler.run_all ();
    EXPECT_EQ (order, (std::vector<int>{1, 2}));
    queue->close ();
    queue->drain ();
    scheduler.retire ();
}

TEST (ZLinkStateLane, AsyncSuffixSealKeepsTheSameMailboxAcrossAllExecutionPolicies)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    for (const int policy : {0, 1, 2}) {
        test::manual_internal_scheduler_t scheduler;
        std::shared_ptr<serial_execution_queue_t> gate;
        if (policy != 0) {
            gate = std::make_shared<serial_execution_queue_t> (
              scheduler, serial_execution_queue_options_t{},
              serial_execution_queue_t::error_handler_t{},
              policy == 1 ? serial_lane_policy_t::spot_wide ()
                          : serial_lane_policy_t::per_actor_spot ());
        }
        auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
        if (gate)
            queue->attach_spot_gate (gate);
        task_completion_source_t<void> seal;
        auto exported =
          queue->take_application_suffix ([&] () -> task_t<void> { co_await seal.task (); });
        auto record = std::make_shared<serial_application_record_t> ();
        record->payload.emplace ();
        int handlers = 0, native_returns = 0;
        record->transferred = [&] { ++native_returns; };
        serial_work_options_t options;
        options.application_record = record;
        ASSERT_TRUE (queue->try_post_async (
          "retained-suffix", [&] (auto complete) { complete ([&] { ++handlers; }); },
          std::move (options)));
        scheduler.run_all ();
        EXPECT_EQ (handlers, 0);
        EXPECT_FALSE (exported.await_ready ());
        seal.complete (result_t<void>::success ());
        scheduler.run_until (exported);
        ASSERT_TRUE (exported.result ());
        ASSERT_EQ (exported.result ().value ().size (), 1u);
        EXPECT_EQ (exported.result ().value ().front (), record);
        scheduler.run_all ();
        EXPECT_EQ (handlers, 0);
        EXPECT_EQ (native_returns, 1);
        EXPECT_EQ (queue->pending_count (), 0u);
        queue->close ();
        if (gate)
            gate->close ();
        queue->drain ();
        if (gate)
            gate->drain ();
        scheduler.retire ();
    }
}

TEST (ZLinkStateLane, DeferredTaskKeepsItsAcceptedPositionUntilItsTerminal)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    test::manual_internal_scheduler_t scheduler;
    auto queue = std::make_shared<serial_execution_queue_t> (scheduler);
    task_completion_source_t<void> release;
    int entered = 0, terminal = 0, suffix = 0;
    queue->post ("handler", [&] {
        EXPECT_TRUE (queue->try_post_deferred_task ("after-handler-task", [&] () -> task_t<void> {
            ++entered;
            co_await release.task ();
            ++terminal;
        }));
    });
    queue->post ("after-deferred-task", [&] { ++suffix; });
    scheduler.run_all ();
    EXPECT_EQ (entered, 1);
    EXPECT_EQ (terminal, 0);
    EXPECT_EQ (suffix, 0);
    release.complete (result_t<void>::success ());
    scheduler.run_until ([&] { return suffix == 1; });
    EXPECT_EQ (terminal, 1);
    EXPECT_EQ (suffix, 1);
    queue->close ();
    scheduler.run_all ();
    queue->drain ();
    scheduler.retire ();
}
