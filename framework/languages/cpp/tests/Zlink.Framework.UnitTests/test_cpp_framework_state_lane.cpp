/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/state_lane.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "../support/scope_exit.hpp"
#include "../support/allocation_counter.hpp"
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
#include "runtime/execution/infrastructure_wait_guard.hpp"
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

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

using zlink::framework::test::scope_exit_t;

static_assert (
  std::is_same_v<decltype (std::declval<state_lane_t &> ().run (int_work_t{})), std::future<int>>);
static_assert (std::is_same_v<decltype (std::declval<state_lane_t &> ().run (void_work_t{})),
                              std::future<void>>);
static_assert (std::is_same_v<decltype (std::declval<state_lane_t &> ().run (reference_work_t{})),
                              std::future<int &>>);

TEST (ZLinkStateLane, MaintenanceInventoryAwaitPreservesSealAndReacquisition)
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{"mesh", "node", {"player"}, 100, 16, 0, 4, 0}});
    const stateful::create_request_t request{
      stateful::object_kind_t::actor, "actor", "player", std::string ("mesh"), {}, false, false};
    const auto created = objects.begin_create (request);
    ASSERT_EQ (stateful::create_status_t::reserved, created.status);
    ASSERT_EQ (stateful::stateful_error_t::none, objects.commit_create (created.attempt));

    const auto exercise = [&] () -> task_t<void> {
        const auto inventory = co_await objects.try_begin_maintenance_inventory ();
        EXPECT_TRUE (inventory);
        if (inventory) {
            EXPECT_EQ (1u, inventory->size ());
            if (!inventory->empty ())
                EXPECT_EQ (created.object, inventory->front ().owner);
        }
        EXPECT_FALSE (co_await objects.try_begin_maintenance_inventory ());
        EXPECT_EQ (stateful::stateful_error_t::moving, objects.begin_create (request).error);
        co_await objects.end_maintenance_inventory ();
        EXPECT_EQ (stateful::create_status_t::existing, objects.begin_create (request).status);
        EXPECT_TRUE (co_await objects.try_begin_maintenance_inventory ());
        co_await objects.end_maintenance_inventory ();
    };
    exercise ().result ().value ();
}

TEST (ZLinkStateLane, RunReturnsTheResultOfTheWork)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);

    EXPECT_EQ (42, lane.run ([] { return 42; }).get ());
}

TEST (ZLinkStateLane, UncontendedRunOnlyAllocatesItsReturnedFuture)
{
    using zlink::framework::tests::count_allocations;
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    struct work_t
    {
        std::array<int, 128> values{};
        int operator() () const { return std::accumulate (values.begin (), values.end (), 0); }
    };
    work_t work;
    work.values.front () = 42;
    EXPECT_EQ (0u, count_allocations ([] {}));
    EXPECT_EQ (1u, count_allocations ([] {
                   auto *allocation = ::operator new (64);
                   ::operator delete (allocation);
               }));
    const auto returned_future_allocations = count_allocations ([] {
        std::promise<int> completion;
        auto result = completion.get_future ();
        completion.set_value (42);
        (void) result.get ();
    });
    int observed = 0;
    const auto submission_allocations =
      count_allocations ([&] { observed = lane.run (work).get (); });
    EXPECT_EQ (42, observed);
    EXPECT_EQ (returned_future_allocations, submission_allocations);
}

TEST (ZLinkStateLane, IdleRunPreservesVoidReferenceAndOriginalException)
{
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    bool called = false;
    lane.run ([&] { called = true; }).get ();
    EXPECT_TRUE (called);
    int value = 42;
    EXPECT_EQ (&value, &lane.run ([&] () -> int & { return value; }).get ());
    struct original_error_t : std::runtime_error
    {
        original_error_t () : std::runtime_error ("original lane error") {}
    };
    auto failed = lane.run ([] () -> int { throw original_error_t{}; });
    EXPECT_THROW (failed.get (), original_error_t);
    EXPECT_EQ (42, lane.run ([] { return 42; }).get ());
}

TEST (ZLinkStateLane, FailedFutureAllocationDoesNotClaimTheLane)
{
    using namespace zlink::framework::tests;
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    const auto future_allocations = count_allocations ([] {
        std::promise<int> completion;
        auto result = completion.get_future ();
        completion.set_value (42);
        (void) result.get ();
    });
    for (std::size_t admitted_allocations = 0; admitted_allocations < future_allocations;
         ++admitted_allocations) {
        bool called = false;
        bool failed = false;
        allocations_before_failure = admitted_allocations;
        try {
            (void) lane.run ([&] {
                called = true;
                return 42;
            });
        }
        catch (const std::bad_alloc &) {
            failed = true;
        }
        allocations_before_failure.reset ();
        EXPECT_TRUE (failed);
        EXPECT_FALSE (called);
        EXPECT_EQ (42, lane.run ([] { return 42; }).get ());
    }
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
    auto takes = std::make_shared<std::atomic<int>> (0);
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
          "missing", [takes] () -> std::optional<runtime::serial_prefix_work_t> {
              ++*takes;
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
    EXPECT_EQ (0, takes->load ());
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
    auto takes = std::make_shared<std::atomic<int>> (0);
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
          "actor", [takes] () -> std::optional<runtime::serial_prefix_work_t> {
              ++*takes;
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
    EXPECT_EQ (1, takes->load ());
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
    auto takes = std::make_shared<std::atomic<unsigned>> (0);
    auto ready = spots.notify_actor_application_ready (
      "actor", [takes] () -> std::optional<runtime::serial_prefix_work_t> {
          ++*takes;
          return std::nullopt;
      });
    ASSERT_TRUE (ready.result ().value ());
    EXPECT_EQ (0u, takes->load ());
    auto queue =
      node->lane.run ([&] { return serial->actor_executor ("player:actor")->queue (); }).get ();
    auto root = serial->spot_queue ();

    // Publication succeeds before the lane closes. The queued actor-ready
    // control must report its failed membership revalidation without terminating.
    node->lane.close ();
    EXPECT_TRUE (scheduler->run_one ());
    scheduler->run_all ();
    EXPECT_EQ (0u, takes->load ());
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
    auto released = release.get_future ().share ();
    executor.submit ([&entered, released] {
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
    std::promise<void> lane_entered;
    std::promise<void> lane_release;
    auto lane_released = lane_release.get_future ();
    handler_coroutine_executor ().post_native_continuation ([&] {
        host_entered.set_value ();
        host_released.wait ();
    });
    scope_exit_t host_pool ([&] {
        host_release.set_value ();
        shutdown_handler_coroutine_executor ();
    });
    host_entered.get_future ().wait ();
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    ASSERT_TRUE (lane.try_post ([&] {
        lane_entered.set_value ();
        lane_released.wait ();
    }));
    lane_entered.get_future ().wait ();
    auto caller = wrap_completion_task (lane.run_task ([] { return 42; }));
    lane_release.set_value ();
    executor.drain ();
    const auto ready = caller.result_for (std::chrono::milliseconds::zero ());
    host_pool.run ();
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
    // Declared after every local the pool work touches, so it joins first.
    scope_exit_t host_pool ([&] {
        release.set_value ();
        shutdown_handler_coroutine_executor ();
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
    host_pool.run ();
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

TEST (ZLinkStateLane, RunTaskPreservesFrameworkFailureFacets)
{
    using namespace zlink::framework;
    for (const bool asynchronous : {false, true}) {
        SCOPED_TRACE (asynchronous);
        offload_executor_t executor (1);
        state_lane_t lane (executor);
        std::promise<void> entered, release;
        auto held = release.get_future ().share ();
        if (asynchronous) {
            ASSERT_TRUE (lane.try_post ([&] {
                entered.set_value ();
                held.wait ();
            }));
            entered.get_future ().wait ();
        }
        const auto code =
          static_cast<std::uint32_t> (runtime::protocol::framework_error_code::spotMoving);
        auto failed = lane.run_task ([code] () -> int {
            throw detail::with_failure_code (
              detail::make_framework_origin_exception (framework_error_kind_t::unavailable,
                                                       "state owner refused the operation"),
              code);
        });
        release.set_value ();
        const auto result = failed.result ();
        ASSERT_FALSE (result);
        ASSERT_NE (result.error (), nullptr);
        EXPECT_EQ (result.error_kind (), framework_error_kind_t::unavailable);
        EXPECT_EQ (detail::error_origin (*result.error ()), detail::error_origin_t::framework);
        EXPECT_EQ (detail::failure_code (*result.error ()), code);
    }
}

TEST (ZLinkStateLane, StandaloneTaskCompletionAllowsLaneReentry)
{
    constexpr auto completion_timeout = std::chrono::seconds (5);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ().share ();
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    lane.try_post ([&entered, released] {
        entered.set_value ();
        released.wait ();
    });
    entered.get_future ().wait ();
    auto task = lane.run_task ([] { return 42; });
    // The observer owns the promise so a late delivery never touches a dead local.
    auto observed = std::make_shared<std::promise<int>> ();
    auto result = observed->get_future ().share ();
    bool lane_open = false;
    const auto open_lane = [&] {
        if (!lane_open) {
            lane_open = true;
            release.set_value ();
        }
    };
    // The observer calls lane.run; wait for it before lane and executor unwind.
    scope_exit_t observer_done ([&] {
        open_lane ();
        result.wait ();
    });
    zlink::framework::detail::observe_task_completion (
      task, [&lane, observed] (const zlink::framework::result_t<int> &value) {
          EXPECT_EQ (nullptr, state_lane_t::current ());
          observed->set_value (lane.run ([&] { return value.value (); }).get ());
      });
    open_lane ();
    ASSERT_EQ (std::future_status::ready, result.wait_for (completion_timeout));
    EXPECT_EQ (42, result.get ());
}

#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
// The worker is outside every lane scope while its target drain is queued
// behind it. Only executor identity can diagnose this pending dependency.
template <typename Wait> void same_executor_pending_wait (Wait wait)
{
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ();
    std::promise<void> done;
    auto completed = done.get_future ();
    executor.submit ([&] {
        entered.set_value ();
        released.wait ();
        EXPECT_EQ (nullptr, state_lane_t::current ());
        EXPECT_EQ (nullptr, zlink::framework::detail::current_infrastructure_wait_owner);
        auto future = lane.run_checked ([] { return 7; });
        wait (future);
        done.set_value ();
    });
    entered.get_future ().wait ();
    lane.try_post ([] {});
    release.set_value ();
    completed.wait ();
    executor.drain ();
}

TEST (ZLinkStateLane, DebugWaitRejectsSameExecutorPendingGetOutsideLane)
{
    ASSERT_DEATH (same_executor_pending_wait ([] (auto &future) { future.get (); }),
                  "infrastructure wait guard: state-lane/get");
}

TEST (ZLinkStateLane, DebugWaitRejectsSameExecutorPendingWaitOutsideLane)
{
    ASSERT_DEATH (same_executor_pending_wait ([] (auto &future) { future.wait (); }),
                  "infrastructure wait guard: state-lane/wait");
}

TEST (ZLinkStateLane, DebugWaitRejectsSameExecutorPendingWaitForOutsideLane)
{
    ASSERT_DEATH (same_executor_pending_wait (
                    [] (auto &future) { future.wait_for (std::chrono::seconds (1)); }),
                  "infrastructure wait guard: state-lane/wait_for");
}

TEST (ZLinkStateLane, DebugWaitRejectsSameExecutorPendingWaitUntilOutsideLane)
{
    ASSERT_DEATH (same_executor_pending_wait ([] (auto &future) {
                      future.wait_until (std::chrono::steady_clock::time_point::max ());
                  }),
                  "infrastructure wait guard: state-lane/wait_until");
}

TEST (ZLinkStateLane, DebugWaitAllowsSameExecutorZeroDurationPoll)
{
    same_executor_pending_wait ([] (auto &future) {
        EXPECT_EQ (std::future_status::timeout, future.wait_for (std::chrono::seconds (0)));
        EXPECT_EQ (std::future_status::timeout,
                   future.wait_until (std::chrono::steady_clock::time_point::min ()));
    });
}

TEST (ZLinkStateLane, DebugWaitAllowsIndependentExecutorPendingCompletionFromInfrastructure)
{
    offload_executor_t caller (1);
    offload_executor_t target (1);
    state_lane_t lane (target);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ();
    lane.try_post ([&] {
        entered.set_value ();
        released.wait ();
    });
    entered.get_future ().wait ();
    std::promise<void> checked;
    auto observed = checked.get_future ();
    std::promise<void> done;
    auto completed = done.get_future ();
    caller.submit ([&] {
        zlink::framework::runtime::infrastructure_wait_guard::infrastructure_scope_t scope (
          &caller);
        auto future = lane.run_checked ([] { return 7; });
        EXPECT_EQ (std::future_status::timeout, future.wait_for (std::chrono::seconds (0)));
        EXPECT_FALSE (zlink::framework::runtime::state_lane_internal::pending_wait_is_unsafe (
          std::future_status::timeout, &target));
        checked.set_value ();
        EXPECT_EQ (7, future.get ());
        done.set_value ();
    });
    observed.wait ();
    release.set_value ();
    completed.wait ();
    caller.drain ();
    target.drain ();
}

struct release_after_pending_probe_t
{
    std::future<int> future;
    std::promise<void> &release;

    bool valid () const { return future.valid (); }
    template <typename Rep, typename Period>
    std::future_status wait_for (std::chrono::duration<Rep, Period> duration) const
    {
        const auto status = future.wait_for (duration);
        EXPECT_EQ (std::future_status::timeout, status);
        release.set_value ();
        return status;
    }
    int get () { return future.get (); }
};

TEST (ZLinkStateLane, DebugWaitAllowsIndependentExecutorAtThePendingProbe)
{
    offload_executor_t caller (1);
    offload_executor_t target (1);
    state_lane_t lane (target);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ();
    std::thread owner ([&] {
        lane
          .run ([&] {
              entered.set_value ();
              released.wait ();
          })
          .get ();
    });
    entered.get_future ().wait ();
    auto pending = lane.run ([] { return 7; });
    std::promise<void> done;
    auto completed = done.get_future ();
    caller.submit ([&] {
        zlink::framework::runtime::infrastructure_wait_guard::infrastructure_scope_t scope (
          &caller);
        zlink::framework::runtime::state_lane_internal::checked_future_t<
          release_after_pending_probe_t>
          checked (release_after_pending_probe_t{std::move (pending), release}, &target);
        EXPECT_EQ (7, checked.get ());
        done.set_value ();
    });
    completed.wait ();
    owner.join ();
    caller.drain ();
    target.drain ();
}

TEST (ZLinkStateLane, DebugWaitBoxedFutureUsesTargetExecutorOnce)
{
    using namespace zlink::framework::runtime::state_lane_internal;
    offload_executor_t caller (1);
    offload_executor_t target (1);
    std::promise<std::unique_ptr<move_constructible_snapshot_t>> completion;
    checked_future_t<boxed_future_t<move_constructible_snapshot_t>> future (
      boxed_future_t<move_constructible_snapshot_t> (completion.get_future ()), &target);
    std::promise<void> checked;
    auto observed = checked.get_future ();
    std::promise<void> done;
    auto completed = done.get_future ();
    caller.submit ([&] {
        zlink::framework::runtime::infrastructure_wait_guard::infrastructure_scope_t scope (
          &caller);
        EXPECT_EQ (std::future_status::timeout, future.wait_for (std::chrono::seconds (0)));
        checked.set_value ();
        future.wait ();
        EXPECT_EQ (std::future_status::ready,
                   future.wait_until (std::chrono::steady_clock::time_point::min ()));
        auto moved = std::move (future);
        EXPECT_EQ (7, moved.get ().value);
        done.set_value ();
    });
    observed.wait ();
    completion.set_value (std::make_unique<move_constructible_snapshot_t> (7));
    completed.wait ();
    caller.drain ();
    target.drain ();
}

TEST (ZLinkStateLane, DebugWaitAcceptsReadyOwnerCompletion)
{
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    EXPECT_EQ (7, lane.run_checked ([] { return 7; }).get ());
}

TEST (ZLinkStateLane, DebugWaitPreservesReadyVoidReferenceAndMoveResults)
{
    offload_executor_t executor (1);
    state_lane_t lane (executor);
    lane.run_checked ([] {}).get ();
    int value = 7;
    EXPECT_EQ (&value, &lane.run_checked ([&] () -> int & { return value; }).get ());
    auto future = lane.run_checked ([] { return std::make_unique<int> (7); });
    auto moved = std::move (future);
    EXPECT_FALSE (future.valid ());
    EXPECT_EQ (7, *moved.get ());
}

TEST (ZLinkStateLane, DebugWaitRejectsSameExecutorPendingBoxedGet)
{
    ASSERT_DEATH (
      {
          using namespace zlink::framework::runtime::state_lane_internal;
          offload_executor_t executor (1);
          std::promise<std::unique_ptr<move_constructible_snapshot_t>> completion;
          checked_future_t<boxed_future_t<move_constructible_snapshot_t>> future (
            boxed_future_t<move_constructible_snapshot_t> (completion.get_future ()), &executor);
          std::promise<void> done;
          auto completed = done.get_future ();
          executor.submit ([&] {
              future.get ();
              done.set_value ();
          });
          completed.wait ();
      },
      "infrastructure wait guard: state-lane/get");
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
    auto released = release.get_future ().share ();
    executor.submit ([&entered, released] {
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
    auto released = release.get_future ().share ();
    executor.submit ([&entered, released] {
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
    int count = 0;
    offload_executor_t executor (2);
    state_lane_t lane (executor);

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
    int completed = 0;
    offload_executor_t executor (2);
    state_lane_t lane (executor);

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
