/* SPDX-License-Identifier: FSL-1.1-ALv2 */
//  Spec 33-core-hwm-application-job-flow §8 contract tests for the shared
//  Application Job Queue: limit-1 head-of-line, a permit outliving its parent
//  call, the 1:N materialization cap, and stop/exactly-once release. These are
//  the named, filterable counterparts of the anonymous queue block in
//  test_cpp_framework_execution.cpp.

#include "runtime/dispatch/application_job_queue.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace
{

using queue_t = zlink::framework::runtime::application_job_queue_t;

zlink::framework::runtime::application_job_queue_configuration_t
limit_one_configuration ()
{
    return {zlink::framework::application_job_queue_profile_t::balanced,
            std::uint32_t{1}, 1, 1};
}

} // namespace

//  §8: with limit 1, while the first job waits before callback start, the
//  next ordinary record is not received first.
TEST (ZLinkFrameworkApplicationJobQueue,
      LimitOneHoldsNextOrdinaryRecordUntilHandlerEntry)
{
    queue_t queue (limit_one_configuration ());

    auto first = queue.try_reserve_supply ();
    ASSERT_TRUE (first);
    first->mark_queued ();

    //  The next ordinary reservation must neither succeed immediately ...
    EXPECT_FALSE (queue.try_reserve_supply ());

    //  ... nor complete through a waiter while the first job is still
    //  queued before its handler's first instruction.
    bool granted = false;
    std::optional<queue_t::permit_t> second;
    auto waiter = queue.wait_for_supply (
      [&] (std::optional<queue_t::permit_t> permit) {
          granted = true;
          if (permit)
              second.emplace (std::move (*permit));
      });
    EXPECT_FALSE (granted);
    const auto parked = queue.snapshot ();
    EXPECT_EQ (1u, parked.capacity_waiters);
    EXPECT_EQ (1u, parked.queued_application_jobs);
    EXPECT_EQ (1u, parked.permits_in_use);

    //  Releasing at the handler's first instruction admits the parked
    //  record in FIFO order.
    first->release_for_handler_entry ();
    EXPECT_TRUE (granted);
    ASSERT_TRUE (second);
    second->release_without_handler ();
    second.reset ();

    const auto drained = queue.snapshot ();
    EXPECT_EQ (0u, drained.permits_in_use);
    EXPECT_EQ (0u, drained.capacity_waiters);
}

//  §8: an asynchronous activation or materialization that outlives its
//  parent call retains the permit (the fanout/mesh shared_ptr capture
//  pattern), and the release stays exactly-once.
TEST (ZLinkFrameworkApplicationJobQueue,
      PermitMovedIntoDeferredContinuationOutlivesParentCall)
{
    queue_t queue (limit_one_configuration ());

    std::function<void ()> deferred;
    {
        //  Parent call: reserve, mark queued, and move the permit into a
        //  continuation that runs only after this scope has exited.
        auto permit = queue.try_reserve_supply ();
        ASSERT_TRUE (permit);
        permit->mark_queued ();
        auto shared =
          std::make_shared<queue_t::permit_t> (std::move (*permit));
        deferred = [shared] { shared->release_for_handler_entry (); };
    }

    //  The parent call has returned and the continuation has not run: the
    //  permit is retained, so no further ordinary record is admitted.
    const auto held = queue.snapshot ();
    EXPECT_EQ (1u, held.permits_in_use);
    EXPECT_EQ (1u, held.queued_application_jobs);
    EXPECT_FALSE (queue.try_reserve_supply ());

    //  The deferred handler entry releases the permit ...
    deferred ();
    EXPECT_EQ (0u, queue.snapshot ().permits_in_use);

    //  ... exactly once: destroying the continuation (and with it the
    //  shared permit wrapper) must not return capacity a second time.
    deferred = {};
    const auto settled = queue.snapshot ();
    EXPECT_EQ (0u, settled.permits_in_use);
    EXPECT_EQ (0u, settled.reserved_supply_permits);
    EXPECT_EQ (0u, settled.queued_application_jobs);
    auto reacquired = queue.try_reserve_supply ();
    EXPECT_TRUE (reacquired);
    EXPECT_FALSE (queue.try_reserve_supply ());
}

//  §8: 1:N dispatch does not materialize more children than secured
//  permits — with limit 1 the children materialize strictly one at a time,
//  in FIFO order, and the concurrent peak never exceeds the limit.
TEST (ZLinkFrameworkApplicationJobQueue,
      OneToManyChildrenNeverExceedSecuredPermits)
{
    queue_t queue (limit_one_configuration ());
    queue.reset_metrics ();

    auto parent = queue.try_reserve_supply ();
    ASSERT_TRUE (parent);
    parent->mark_queued ();

    std::vector<int> materialized;
    std::optional<queue_t::permit_t> child_two;
    std::optional<queue_t::permit_t> child_three;
    auto second_waiter = queue.wait_for_supply (
      [&] (std::optional<queue_t::permit_t> permit) {
          if (permit) {
              materialized.push_back (2);
              child_two.emplace (std::move (*permit));
          }
      });
    auto third_waiter = queue.wait_for_supply (
      [&] (std::optional<queue_t::permit_t> permit) {
          if (permit) {
              materialized.push_back (3);
              child_three.emplace (std::move (*permit));
          }
      });

    //  Both children stay unmaterialized while the parent holds the only
    //  permit.
    EXPECT_TRUE (materialized.empty ());
    EXPECT_EQ (2u, queue.snapshot ().capacity_waiters);

    parent->release_for_handler_entry ();
    EXPECT_EQ ((std::vector<int>{2}), materialized);
    ASSERT_TRUE (child_two);
    EXPECT_FALSE (child_three);
    child_two->mark_queued ();
    EXPECT_EQ (1u, queue.snapshot ().permits_in_use);

    child_two->release_for_handler_entry ();
    EXPECT_EQ ((std::vector<int>{2, 3}), materialized);
    ASSERT_TRUE (child_three);
    child_three->release_without_handler ();
    child_three.reset ();

    const auto finished = queue.snapshot ();
    EXPECT_EQ (0u, finished.permits_in_use);
    EXPECT_EQ (1u, finished.peak_permits_in_use);
}

//  §8: shutdown releases waiters and permits exactly once, and a second
//  stop (or a cancel after stop) does not release anything twice.
TEST (ZLinkFrameworkApplicationJobQueue,
      StopReleasesParkedWaitersExactlyOnceAndStaysIdempotent)
{
    queue_t queue (limit_one_configuration ());

    auto held = queue.try_reserve_supply ();
    ASSERT_TRUE (held);
    held->mark_queued ();

    int waiter_signals = 0;
    bool waiter_granted = false;
    auto waiter = queue.wait_for_supply (
      [&] (std::optional<queue_t::permit_t> permit) {
          ++waiter_signals;
          waiter_granted = static_cast<bool> (permit);
      });
    EXPECT_EQ (0, waiter_signals);
    EXPECT_EQ (1u, queue.snapshot ().capacity_waiters);

    queue.stop ();
    EXPECT_EQ (1, waiter_signals);
    EXPECT_FALSE (waiter_granted);
    EXPECT_EQ (0u, queue.snapshot ().capacity_waiters);

    //  Idempotent: a second stop and a late cancel signal nothing more.
    queue.stop ();
    EXPECT_EQ (1, waiter_signals);
    EXPECT_FALSE (waiter.cancel ());
    EXPECT_EQ (1, waiter_signals);

    //  Post-stop admission is refused, immediately and terminally.
    EXPECT_FALSE (queue.try_reserve_supply ());
    int late_signals = 0;
    bool late_granted = false;
    auto late = queue.wait_for_supply (
      [&] (std::optional<queue_t::permit_t> permit) {
          ++late_signals;
          late_granted = static_cast<bool> (permit);
      });
    EXPECT_EQ (1, late_signals);
    EXPECT_FALSE (late_granted);

    //  The still-held permit returns exactly once; the second release is
    //  a no-op and nothing is handed to the cancelled waiter.
    held->release_for_handler_entry ();
    held->release_without_handler ();
    const auto settled = queue.snapshot ();
    EXPECT_EQ (0u, settled.permits_in_use);
    EXPECT_EQ (0u, settled.reserved_supply_permits);
    EXPECT_EQ (0u, settled.queued_application_jobs);
    EXPECT_EQ (1, waiter_signals);
}
