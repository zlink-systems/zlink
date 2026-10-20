/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/state_lane.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "../support/runtime_failure_fixture.hpp"
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
