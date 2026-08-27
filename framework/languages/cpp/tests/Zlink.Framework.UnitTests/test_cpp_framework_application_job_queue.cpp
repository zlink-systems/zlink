/* SPDX-License-Identifier: FSL-1.1-ALv2 */
//  Spec 33-core-hwm-application-job-flow §8 contract tests for the shared
//  Application Job Queue: limit-1 head-of-line, a permit outliving its parent
//  call, the 1:N materialization cap, and stop/exactly-once release. These are
//  the named, filterable counterparts of the anonymous queue block in
//  test_cpp_framework_execution.cpp.

#include "runtime/dispatch/application_job_queue.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
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

zlink::framework::runtime::application_job_queue_configuration_t
pressure_configuration (
  std::uint32_t maximum,
  std::uint32_t pause_percent = 80,
  std::uint32_t resume_percent = 60)
{
    return {zlink::framework::application_job_queue_profile_t::balanced,
            maximum, 1, maximum, pause_percent, resume_percent};
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

TEST (ZLinkFrameworkApplicationJobQueue,
      PressureThresholdsUseCeilingFloorDefaultsAndValidation)
{
    queue_t queue (pressure_configuration (7));
    const auto status = queue.snapshot ();
    EXPECT_EQ (80u, status.configured_pause_threshold_percent);
    EXPECT_EQ (60u, status.configured_resume_threshold_percent);
    EXPECT_EQ (6u, status.pause_permit_count);
    EXPECT_EQ (4u, status.resume_permit_count);
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::running,
      status.pressure_state);

    constexpr auto maximum = static_cast<std::uint32_t> (
      std::numeric_limits<std::int32_t>::max ());
    queue_t large (pressure_configuration (maximum, 100, 99));
    const auto large_status = large.snapshot ();
    EXPECT_EQ (maximum, large_status.pause_permit_count);
    EXPECT_EQ (
      static_cast<std::uint32_t> (
        (static_cast<std::uint64_t> (maximum) * 99u) / 100u),
      large_status.resume_permit_count);

    EXPECT_THROW (
      queue_t (pressure_configuration (7, 0, 0)),
      std::invalid_argument);
    EXPECT_THROW (
      queue_t (pressure_configuration (7, 101, 60)),
      std::invalid_argument);
    EXPECT_THROW (
      queue_t (pressure_configuration (7, 80, 100)),
      std::invalid_argument);
    EXPECT_THROW (
      queue_t (pressure_configuration (7, 60, 60)),
      std::invalid_argument);
}

TEST (ZLinkFrameworkApplicationJobQueue,
      PressureHysteresisCountsReservedAndQueuedPermitOnce)
{
    queue_t queue (pressure_configuration (7));
    std::vector<zlink::framework::application_job_queue_pressure_state_t>
      applied;
    auto registration = queue.register_receive_flow_socket (
      [&] (auto state) {
          applied.push_back (state);
          return zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
      });
    ASSERT_EQ (1u, applied.size ());
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::running,
      applied.back ());

    std::vector<queue_t::permit_t> permits;
    for (int index = 0; index < 6; ++index) {
        auto permit = queue.try_reserve_supply ();
        ASSERT_TRUE (permit);
        permits.push_back (std::move (*permit));
    }
    ASSERT_EQ (2u, applied.size ());
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::paused,
      applied.back ());

    for (auto &permit : permits)
        permit.mark_queued ();
    const auto queued = queue.snapshot ();
    EXPECT_EQ (0u, queued.reserved_supply_permits);
    EXPECT_EQ (6u, queued.queued_application_jobs);
    EXPECT_EQ (6u, queued.permits_in_use);
    EXPECT_EQ (2u, applied.size ());

    permits.back ().release_for_handler_entry ();
    permits.pop_back ();
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::paused,
      queue.snapshot ().pressure_state);
    EXPECT_EQ (2u, applied.size ());

    permits.back ().release_for_handler_entry ();
    permits.pop_back ();
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::running,
      queue.snapshot ().pressure_state);
    ASSERT_EQ (3u, applied.size ());
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::running,
      applied.back ());
}

// §6: host pressure is a single absolute receive-flow state.  A permit
// acquired through one socket must therefore pause and resume every supported
// socket in the host snapshot, not just the socket that acquired the permit.
TEST (ZLinkFrameworkApplicationJobQueue,
      PressureTransitionAppliesToEveryRegisteredReceiveFlowSocket)
{
    queue_t queue (pressure_configuration (2, 100, 50));
    std::vector<zlink::framework::application_job_queue_pressure_state_t>
      first_socket_states;
    std::vector<zlink::framework::application_job_queue_pressure_state_t>
      second_socket_states;
    auto first_socket = queue.register_receive_flow_socket (
      [&] (auto state) {
          first_socket_states.push_back (state);
          return zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
      });
    auto second_socket = queue.register_receive_flow_socket (
      [&] (auto state) {
          second_socket_states.push_back (state);
          return zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
      });

    ASSERT_EQ (1u, first_socket_states.size ());
    ASSERT_EQ (1u, second_socket_states.size ());
    EXPECT_EQ (zlink::framework::application_job_queue_pressure_state_t::running,
               first_socket_states.back ());
    EXPECT_EQ (zlink::framework::application_job_queue_pressure_state_t::running,
               second_socket_states.back ());

    auto first_permit = queue.try_reserve_supply ();
    auto second_permit = queue.try_reserve_supply ();
    ASSERT_TRUE (first_permit);
    ASSERT_TRUE (second_permit);

    ASSERT_EQ (2u, first_socket_states.size ());
    ASSERT_EQ (2u, second_socket_states.size ());
    EXPECT_EQ (zlink::framework::application_job_queue_pressure_state_t::paused,
               first_socket_states.back ());
    EXPECT_EQ (zlink::framework::application_job_queue_pressure_state_t::paused,
               second_socket_states.back ());

    first_permit->release_without_handler ();

    ASSERT_EQ (3u, first_socket_states.size ());
    ASSERT_EQ (3u, second_socket_states.size ());
    EXPECT_EQ (zlink::framework::application_job_queue_pressure_state_t::running,
               first_socket_states.back ());
    EXPECT_EQ (zlink::framework::application_job_queue_pressure_state_t::running,
               second_socket_states.back ());
}

TEST (ZLinkFrameworkApplicationJobQueue,
      CapacityWaiterAndPermitHandoffDoNotIncreasePressureCount)
{
    queue_t queue (pressure_configuration (2, 100, 50));
    auto first = queue.try_reserve_supply ();
    auto second = queue.try_reserve_supply ();
    ASSERT_TRUE (first);
    ASSERT_TRUE (second);
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::paused,
      queue.snapshot ().pressure_state);

    std::optional<queue_t::permit_t> handed_off;
    auto waiter = queue.wait_for_supply (
      [&] (std::optional<queue_t::permit_t> permit) {
          handed_off = std::move (permit);
      });
    EXPECT_FALSE (handed_off);
    EXPECT_EQ (2u, queue.snapshot ().permits_in_use);
    EXPECT_EQ (1u, queue.snapshot ().capacity_waiters);

    first->release_without_handler ();
    ASSERT_TRUE (handed_off);
    EXPECT_EQ (2u, queue.snapshot ().permits_in_use);
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::paused,
      queue.snapshot ().pressure_state);

    handed_off->release_without_handler ();
    EXPECT_EQ (1u, queue.snapshot ().permits_in_use);
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::running,
      queue.snapshot ().pressure_state);
}

TEST (ZLinkFrameworkApplicationJobQueue,
      SocketRegisteredWhilePausedReceivesCurrentAbsoluteStateBeforeExposure)
{
    queue_t queue (pressure_configuration (1, 100, 0));
    auto permit = queue.try_reserve_supply ();
    ASSERT_TRUE (permit);
    ASSERT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::paused,
      queue.snapshot ().pressure_state);

    std::vector<zlink::framework::application_job_queue_pressure_state_t>
      applied;
    auto registration = queue.register_receive_flow_socket (
      [&] (auto state) {
          applied.push_back (state);
          return zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
      });
    ASSERT_TRUE (registration);
    ASSERT_EQ (1u, applied.size ());
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::paused,
      applied.front ());
}

TEST (ZLinkFrameworkApplicationJobQueue,
      RegistrationRetrySkipsDuplicateSameStateNativeApply)
{
    queue_t queue (pressure_configuration (1, 100, 0));
    std::mutex mutex;
    std::condition_variable changed;
    bool initial_running_entered = false;
    bool release_initial_running = false;
    std::size_t running_apply_count = 0;
    std::size_t paused_apply_count = 0;
    std::optional<queue_t::receive_flow_registration_t> registration;
    std::exception_ptr registration_error;

    std::thread register_thread ([&] {
        try {
            registration.emplace (
              queue.register_receive_flow_socket (
                [&] (auto state) {
                    std::unique_lock lock (mutex);
                    if (state
                        == zlink::framework::application_job_queue_pressure_state_t::running) {
                        ++running_apply_count;
                        if (!initial_running_entered) {
                            initial_running_entered = true;
                            changed.notify_all ();
                            changed.wait (
                              lock,
                              [&] { return release_initial_running; });
                        }
                    }
                    else {
                        ++paused_apply_count;
                    }
                    return zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
                }));
        }
        catch (...) {
            registration_error = std::current_exception ();
        }
    });

    {
        std::unique_lock lock (mutex);
        changed.wait (lock, [&] { return initial_running_entered; });
    }
    auto permit = queue.try_reserve_supply ();
    ASSERT_TRUE (permit);
    permit->release_without_handler ();
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::running,
      queue.snapshot ().pressure_state);

    {
        std::lock_guard lock (mutex);
        release_initial_running = true;
    }
    changed.notify_all ();
    register_thread.join ();

    if (registration_error)
        std::rethrow_exception (registration_error);
    ASSERT_TRUE (registration);
    EXPECT_EQ (1u, running_apply_count);
    EXPECT_EQ (0u, paused_apply_count);
}

TEST (ZLinkFrameworkApplicationJobQueue,
      StopFencesRegistrationThatIsApplyingItsInitialState)
{
    queue_t queue (pressure_configuration (1, 100, 0));
    std::mutex mutex;
    std::condition_variable changed;
    bool apply_entered = false;
    bool release_apply = false;
    std::optional<queue_t::receive_flow_registration_t> registration;
    std::exception_ptr registration_error;

    std::thread register_thread ([&] {
        try {
            registration.emplace (
              queue.register_receive_flow_socket (
                [&] (auto) {
                    std::unique_lock lock (mutex);
                    apply_entered = true;
                    changed.notify_all ();
                    changed.wait (lock, [&] { return release_apply; });
                    return zlink::framework::runtime::receive_flow_state_apply_result_t::invalid_state;
                }));
        }
        catch (...) {
            registration_error = std::current_exception ();
        }
    });

    {
        std::unique_lock lock (mutex);
        changed.wait (lock, [&] { return apply_entered; });
    }
    queue.stop ();
    {
        std::lock_guard lock (mutex);
        release_apply = true;
    }
    changed.notify_all ();
    register_thread.join ();

    EXPECT_FALSE (registration);
    ASSERT_TRUE (registration_error);
    EXPECT_THROW (std::rethrow_exception (registration_error),
                  std::logic_error);
    EXPECT_EQ (
      0u,
      queue.pressure_metrics_snapshot ()
        .flow_state_config_failure_count);
}

TEST (ZLinkFrameworkApplicationJobQueue,
      StopDoesNotSuppressAConfigFailureDuringInitialStateApply)
{
    std::size_t diagnostic_count = 0;
    queue_t queue (
      pressure_configuration (1, 100, 0),
      [&] { ++diagnostic_count; });
    std::mutex mutex;
    std::condition_variable changed;
    bool apply_entered = false;
    bool release_apply = false;
    std::optional<queue_t::receive_flow_registration_t> registration;
    std::exception_ptr registration_error;

    std::thread register_thread ([&] {
        try {
            registration.emplace (
              queue.register_receive_flow_socket (
                [&] (auto) {
                    std::unique_lock lock (mutex);
                    apply_entered = true;
                    changed.notify_all ();
                    changed.wait (lock, [&] { return release_apply; });
                    return zlink::framework::runtime::receive_flow_state_apply_result_t::failed;
                }));
        }
        catch (...) {
            registration_error = std::current_exception ();
        }
    });

    {
        std::unique_lock lock (mutex);
        changed.wait (lock, [&] { return apply_entered; });
    }
    queue.stop ();
    {
        std::lock_guard lock (mutex);
        release_apply = true;
    }
    changed.notify_all ();
    register_thread.join ();

    EXPECT_FALSE (registration);
    ASSERT_TRUE (registration_error);
    EXPECT_THROW (std::rethrow_exception (registration_error),
                  std::logic_error);
    EXPECT_EQ (1u, diagnostic_count);
    EXPECT_EQ (
      1u,
      queue.pressure_metrics_snapshot ()
        .flow_state_config_failure_count);
}

TEST (ZLinkFrameworkApplicationJobQueue,
      StopDetachesReceiveFlowBeforeAQueuedPermitReleases)
{
    queue_t queue (pressure_configuration (1, 100, 0));
    std::vector<zlink::framework::application_job_queue_pressure_state_t>
      applied;
    auto registration = queue.register_receive_flow_socket (
      [&] (auto state) {
          applied.push_back (state);
          return zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
      });
    auto permit = queue.try_reserve_supply ();
    ASSERT_TRUE (permit);
    ASSERT_EQ (2u, applied.size ());
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::paused,
      applied.back ());

    queue.stop ();
    permit->release_without_handler ();

    EXPECT_EQ (2u, applied.size ());
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::running,
      queue.snapshot ().pressure_state);
}

TEST (ZLinkFrameworkApplicationJobQueue,
      StopDoesNotWaitForAnInFlightReceiveFlowApply)
{
    queue_t queue (pressure_configuration (1, 100, 0));
    std::mutex mutex;
    std::condition_variable changed;
    bool pause_entered = false;
    bool release_pause = false;
    auto registration = queue.register_receive_flow_socket (
      [&] (auto state) {
          if (state
              != zlink::framework::application_job_queue_pressure_state_t::paused)
              return zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
          std::unique_lock lock (mutex);
          pause_entered = true;
          changed.notify_all ();
          changed.wait (lock, [&] { return release_pause; });
          return zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
      });

    std::optional<queue_t::permit_t> permit;
    std::thread reserve_thread ([&] {
        permit = queue.try_reserve_supply ();
    });
    {
        std::unique_lock lock (mutex);
        changed.wait (lock, [&] { return pause_entered; });
    }

    auto stopped = std::async (std::launch::async, [&] { queue.stop (); });
    const auto stop_status =
      stopped.wait_for (std::chrono::seconds (1));
    if (stop_status != std::future_status::ready) {
        {
            std::lock_guard lock (mutex);
            release_pause = true;
        }
        changed.notify_all ();
        reserve_thread.join ();
        stopped.wait ();
        FAIL () << "queue stop waited for an in-flight receive-flow apply";
        return;
    }
    stopped.get ();

    auto closed = std::async (
      std::launch::async, [&] { registration.close (); });
    EXPECT_EQ (
      std::future_status::timeout,
      closed.wait_for (std::chrono::milliseconds (100)));
    {
        std::lock_guard lock (mutex);
        release_pause = true;
    }
    changed.notify_all ();
    reserve_thread.join ();
    EXPECT_EQ (
      std::future_status::ready,
      closed.wait_for (std::chrono::seconds (1)));
    closed.get ();
}

TEST (ZLinkFrameworkApplicationJobQueue,
      StopBlocksAStaleTransitionBeforeItsNextSocketApply)
{
    queue_t queue (pressure_configuration (1, 100, 0));
    std::mutex mutex;
    std::condition_variable changed;
    bool first_pause_entered = false;
    bool release_first_pause = false;
    auto first = queue.register_receive_flow_socket (
      [&] (auto state) {
          if (state
              != zlink::framework::application_job_queue_pressure_state_t::paused)
              return zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
          std::unique_lock lock (mutex);
          first_pause_entered = true;
          changed.notify_all ();
          changed.wait (lock, [&] { return release_first_pause; });
          return zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
      });
    std::size_t second_pause_apply_count = 0;
    auto second = queue.register_receive_flow_socket (
      [&] (auto state) {
          if (state
              == zlink::framework::application_job_queue_pressure_state_t::paused)
              ++second_pause_apply_count;
          return zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
      });

    std::optional<queue_t::permit_t> permit;
    std::thread reserve_thread ([&] {
        permit = queue.try_reserve_supply ();
    });
    {
        std::unique_lock lock (mutex);
        changed.wait (lock, [&] { return first_pause_entered; });
    }
    queue.stop ();
    {
        std::lock_guard lock (mutex);
        release_first_pause = true;
    }
    changed.notify_all ();
    reserve_thread.join ();

    EXPECT_EQ (0u, second_pause_apply_count);
}

TEST (ZLinkFrameworkApplicationJobQueue,
      NonClosingInvalidStateIsAConfigFailure)
{
    queue_t queue (pressure_configuration (1, 100, 0));
    EXPECT_THROW (
      queue.register_receive_flow_socket (
        [] (auto) {
            return zlink::framework::runtime::receive_flow_state_apply_result_t::invalid_state;
        }),
      std::runtime_error);
    EXPECT_EQ (
      1u,
      queue.pressure_metrics_snapshot ()
        .flow_state_config_failure_count);
}

TEST (ZLinkFrameworkApplicationJobQueue,
      ConcurrentTransitionsRejectAStaleOutOfOrderFinalState)
{
    queue_t queue (pressure_configuration (2, 100, 50));
    std::mutex mutex;
    std::condition_variable changed;
    bool pause_entered = false;
    bool release_pause = false;
    std::vector<zlink::framework::application_job_queue_pressure_state_t>
      applied;
    auto registration = queue.register_receive_flow_socket (
      [&] (auto state) {
          std::unique_lock lock (mutex);
          applied.push_back (state);
          if (state
              == zlink::framework::application_job_queue_pressure_state_t::paused) {
              pause_entered = true;
              changed.notify_all ();
              changed.wait (lock, [&] { return release_pause; });
          }
          return zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
      });

    auto first = queue.try_reserve_supply ();
    ASSERT_TRUE (first);
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::running,
      queue.snapshot ().pressure_state);

    std::optional<queue_t::permit_t> second;
    std::thread pause_thread ([&] {
        second = queue.try_reserve_supply ();
    });
    {
        std::unique_lock lock (mutex);
        changed.wait (lock, [&] { return pause_entered; });
    }

    std::thread resume_thread ([&] {
        first->release_without_handler ();
    });
    {
        std::lock_guard lock (mutex);
        release_pause = true;
    }
    changed.notify_all ();
    pause_thread.join ();
    resume_thread.join ();

    ASSERT_TRUE (second);
    ASSERT_EQ (3u, applied.size ());
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::paused,
      applied[1]);
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::running,
      applied[2]);
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::running,
      queue.snapshot ().pressure_state);
}

TEST (ZLinkFrameworkApplicationJobQueue,
      DeregistrationWaitsForInFlightApplyBeforeSocketClose)
{
    queue_t queue (pressure_configuration (1, 100, 0));
    std::mutex mutex;
    std::condition_variable changed;
    bool pause_entered = false;
    bool release_pause = false;
    bool socket_closed = false;
    std::size_t apply_count = 0;
    auto registration = queue.register_receive_flow_socket (
      [&] (auto state) {
          std::unique_lock lock (mutex);
          EXPECT_FALSE (socket_closed);
          ++apply_count;
          if (state
              == zlink::framework::application_job_queue_pressure_state_t::paused) {
              pause_entered = true;
              changed.notify_all ();
              changed.wait (lock, [&] { return release_pause; });
          }
          return zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
      });

    std::optional<queue_t::permit_t> permit;
    std::thread pause_thread ([&] {
        permit = queue.try_reserve_supply ();
    });
    {
        std::unique_lock lock (mutex);
        changed.wait (lock, [&] { return pause_entered; });
    }
    std::thread close_thread ([&] {
        registration.close ();
        std::lock_guard lock (mutex);
        socket_closed = true;
    });
    {
        std::lock_guard lock (mutex);
        release_pause = true;
    }
    changed.notify_all ();
    pause_thread.join ();
    close_thread.join ();

    ASSERT_TRUE (permit);
    permit->release_without_handler ();
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::running,
      queue.snapshot ().pressure_state);

    auto after_close = queue.try_reserve_supply ();
    ASSERT_TRUE (after_close);
    after_close->release_without_handler ();
    std::lock_guard lock (mutex);
    EXPECT_TRUE (socket_closed);
    EXPECT_EQ (2u, apply_count);
}

TEST (ZLinkFrameworkApplicationJobQueue,
      MetricResetRetainsPausedStateAndCurrentDurationOnly)
{
    queue_t queue (pressure_configuration (1, 100, 0));
    auto registration = queue.register_receive_flow_socket (
      [] (auto state) {
          return state
                     == zlink::framework::application_job_queue_pressure_state_t::paused
                   ? zlink::framework::runtime::receive_flow_state_apply_result_t::failed
                   : zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
      });
    auto permit = queue.try_reserve_supply ();
    ASSERT_TRUE (permit);
    std::this_thread::sleep_for (std::chrono::milliseconds (2));
    const auto before = queue.snapshot ();
    const auto metrics_before = queue.pressure_metrics_snapshot ();
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::paused,
      before.pressure_state);
    EXPECT_GT (before.current_pause_duration,
               std::chrono::nanoseconds::zero ());
    EXPECT_EQ (1u, metrics_before.paused_transition_count);
    EXPECT_EQ (1u, metrics_before.flow_state_config_failure_count);

    queue.reset_metrics ();
    const auto after = queue.snapshot ();
    const auto metrics_after = queue.pressure_metrics_snapshot ();
    EXPECT_EQ (
      zlink::framework::application_job_queue_pressure_state_t::paused,
      after.pressure_state);
    EXPECT_GE (after.current_pause_duration,
               before.current_pause_duration);
    EXPECT_EQ (0u, metrics_after.running_transition_count);
    EXPECT_EQ (0u, metrics_after.paused_transition_count);
    EXPECT_EQ (0u, metrics_after.flow_state_config_failure_count);
    EXPECT_LT (metrics_after.cumulative_pause_duration,
               after.current_pause_duration);
}

TEST (ZLinkFrameworkApplicationJobQueue,
      FlowStateConfigFailureInvokesDiagnosticSinkAndMetric)
{
    std::size_t failure_count = 0;
    queue_t queue (
      pressure_configuration (1, 100, 0),
      [&] { ++failure_count; });
    auto registration = queue.register_receive_flow_socket (
      [] (auto state) {
          return state
                     == zlink::framework::application_job_queue_pressure_state_t::paused
                   ? zlink::framework::runtime::receive_flow_state_apply_result_t::failed
                   : zlink::framework::runtime::receive_flow_state_apply_result_t::applied;
      });

    auto permit = queue.try_reserve_supply ();
    ASSERT_TRUE (permit);
    EXPECT_EQ (1u, failure_count);
    EXPECT_EQ (
      1u,
      queue.pressure_metrics_snapshot ().flow_state_config_failure_count);
}
