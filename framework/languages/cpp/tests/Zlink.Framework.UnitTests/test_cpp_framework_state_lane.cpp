/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/state_lane.hpp"

#include <gtest/gtest.h>

#include <atomic>
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

static_assert (std::is_same_v<
               decltype (std::declval<state_lane_t &> ().run (int_work_t{})),
               std::future<int>>);
static_assert (std::is_same_v<
               decltype (std::declval<state_lane_t &> ().run (void_work_t{})),
               std::future<void>>);
static_assert (std::is_same_v<
               decltype (std::declval<state_lane_t &> ().run (reference_work_t{})),
               std::future<int &>>);

TEST (ZLinkStateLane, RunReturnsTheResultOfTheWork)
{
    offload_executor_t executor (2);
    state_lane_t lane (executor);

    EXPECT_EQ (42, lane.run ([] { return 42; }).get ());
}

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
        lane.run ([&] {
            entered.set_value ();
            released.wait ();
            order.push_back (0);
        }).get ();
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

    auto result = lane.run ([] () -> int {
        throw std::invalid_argument ("boom");
    });
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
            lane.run ([&] {
                if (++in_flight != 1) {
                    observed_overlap = true;
                }
                std::this_thread::yield ();
                --in_flight;
            }).get ();
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

    const auto message = lane.run ([&lane] {
        try {
            (void) lane.run ([] { return 1; });
        }
        catch (const std::logic_error &error) {
            return std::string (error.what ());
        }
        return std::string ();
    }).get ();

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
        lane.run ([&] {
            entered.set_value ();
            released.wait ();
            order.push_back (0);
        }).get ();
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
