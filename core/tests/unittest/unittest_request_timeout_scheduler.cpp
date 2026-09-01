/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#include "api/socket/request_timeout_scheduler_internal.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include <unity.h>

namespace
{
struct callback_state_t
{
    callback_state_t () : entered (0), finished (0), cleanup_count (0) {}

    std::atomic<int> entered;
    std::atomic<int> finished;
    std::atomic<int> cleanup_count;
};

void timeout_handler (void *userdata_)
{
    callback_state_t *state = static_cast<callback_state_t *> (userdata_);
    state->entered.fetch_add (1, std::memory_order_relaxed);
    std::this_thread::sleep_for (std::chrono::milliseconds (30));
    state->finished.fetch_add (1, std::memory_order_relaxed);
}

void fast_timeout_handler (void *userdata_)
{
    callback_state_t *state = static_cast<callback_state_t *> (userdata_);
    state->entered.fetch_add (1, std::memory_order_relaxed);
    state->finished.fetch_add (1, std::memory_order_relaxed);
}

void cleanup_handler (void *userdata_)
{
    callback_state_t *state = static_cast<callback_state_t *> (userdata_);
    state->cleanup_count.fetch_add (1, std::memory_order_relaxed);
}

void wait_until_entered (callback_state_t *state_)
{
    //  zlink_stopwatch_intermediate() reports microseconds, so the bound must
    //  be expressed in the same unit or the intended multi-second allowance
    //  silently shrinks to a few milliseconds.
    const unsigned long timeout_us = static_cast<unsigned long> (SETTLE_TIME) * 20 * 1000;
    void *watch = zlink_stopwatch_start ();
    while (state_->entered.load (std::memory_order_relaxed) == 0) {
        msleep (1);
        TEST_ASSERT_LESS_OR_EQUAL_MESSAGE (timeout_us, zlink_stopwatch_intermediate (watch),
                                           "Timeout waiting for request timeout handler");
    }
    zlink_stopwatch_stop (watch);
}
}

void setUp ()
{
}

void tearDown ()
{
}

void test_cancel_before_deadline_prevents_handler_and_runs_cleanup ()
{
    callback_state_t state;

    std::shared_ptr<zlink::request_timeout::task_t> task =
      zlink::request_timeout::schedule (100, timeout_handler, &state, cleanup_handler);
    TEST_ASSERT_TRUE (task.get () != NULL);

    zlink::request_timeout::cancel (task);
    task.reset ();

    msleep (150);

    TEST_ASSERT_EQUAL_INT (0, state.entered.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, state.finished.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (1, state.cleanup_count.load (std::memory_order_relaxed));
}

void test_cancel_while_handler_is_firing_waits_for_handler_completion ()
{
    callback_state_t state;

    std::shared_ptr<zlink::request_timeout::task_t> task =
      zlink::request_timeout::schedule (1, timeout_handler, &state, cleanup_handler);
    TEST_ASSERT_TRUE (task.get () != NULL);

    wait_until_entered (&state);

    zlink::request_timeout::cancel (task);
    task.reset ();

    TEST_ASSERT_EQUAL_INT (1, state.entered.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (1, state.finished.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, state.cleanup_count.load (std::memory_order_relaxed));
}

void test_cancel_notifies_scheduler_only_when_last_deadline_is_removed ()
{
    callback_state_t earliest_state;
    callback_state_t middle_state;
    callback_state_t latest_state;

    std::shared_ptr<zlink::request_timeout::task_t> earliest =
      zlink::request_timeout::schedule (10000, timeout_handler,
                                        &earliest_state, cleanup_handler);
    TEST_ASSERT_TRUE (earliest.get () != NULL);
    std::shared_ptr<zlink::request_timeout::task_t> middle =
      zlink::request_timeout::schedule (20000, timeout_handler, &middle_state,
                                        cleanup_handler);
    TEST_ASSERT_TRUE (middle.get () != NULL);
    std::shared_ptr<zlink::request_timeout::task_t> latest =
      zlink::request_timeout::schedule (30000, timeout_handler, &latest_state,
                                        cleanup_handler);
    TEST_ASSERT_TRUE (latest.get () != NULL);

    zlink::request_timeout::test_reset_cancel_notification_count ();
    zlink::request_timeout::cancel (earliest);
    TEST_ASSERT_EQUAL_UINT64 (
      0, zlink::request_timeout::test_cancel_notification_count ());

    zlink::request_timeout::cancel (middle);
    TEST_ASSERT_EQUAL_UINT64 (
      0, zlink::request_timeout::test_cancel_notification_count ());

    zlink::request_timeout::cancel (latest);
    TEST_ASSERT_EQUAL_UINT64 (
      1, zlink::request_timeout::test_cancel_notification_count ());

    latest.reset ();
    middle.reset ();
    earliest.reset ();
    TEST_ASSERT_EQUAL_INT (
      0, earliest_state.entered.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (
      0, middle_state.entered.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (
      0, latest_state.entered.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (
      1, earliest_state.cleanup_count.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (
      1, middle_state.cleanup_count.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (
      1, latest_state.cleanup_count.load (std::memory_order_relaxed));
}

void test_schedule_across_idle_exit_boundary_fires_every_task ()
{
    //  Regression for the S5-10-01 lost-wakeup race: schedule() used to check
    //  scheduler-thread liveness in a separate critical section from the task
    //  insert, so a task issued exactly while the idle thread committed its
    //  exit could be stranded with no consumer. The scheduler idles out after
    //  100ms, so sweeping the inter-schedule sleep across that boundary lands
    //  successive schedules before, at and after the exit commit. Every task
    //  must fire exactly once within the bound.
    for (int i = 0; i != 25; i++) {
        callback_state_t state;
        std::shared_ptr<zlink::request_timeout::task_t> task =
          zlink::request_timeout::schedule (1, timeout_handler, &state, cleanup_handler);
        TEST_ASSERT_TRUE (task.get () != NULL);
        wait_until_entered (&state);
        zlink::request_timeout::cancel (task);
        task.reset ();
        TEST_ASSERT_EQUAL_INT (1, state.entered.load (std::memory_order_relaxed));
        msleep (90 + (i % 21));
    }
}

void test_schedule_after_canceling_last_task_across_idle_exit_fires ()
{
    // Canceling the only registered deadline wakes the scheduler so it can
    // enter its bounded idle wait. Sweep the next schedule across that wait's
    // exit boundary: it must be observed either by the existing thread or by
    // a freshly started one.
    for (int i = 0; i != 25; ++i) {
        callback_state_t canceled_state;
        std::shared_ptr<zlink::request_timeout::task_t> canceled =
          zlink::request_timeout::schedule (
            1000, fast_timeout_handler, &canceled_state, cleanup_handler);
        TEST_ASSERT_TRUE (canceled.get () != NULL);
        zlink::request_timeout::cancel (canceled);
        canceled.reset ();
        TEST_ASSERT_EQUAL_INT (
          1, canceled_state.cleanup_count.load (std::memory_order_relaxed));

        msleep (90 + (i % 21));

        callback_state_t firing_state;
        std::shared_ptr<zlink::request_timeout::task_t> firing =
          zlink::request_timeout::schedule (
            1, fast_timeout_handler, &firing_state, cleanup_handler);
        TEST_ASSERT_TRUE (firing.get () != NULL);
        wait_until_entered (&firing_state);
        zlink::request_timeout::cancel (firing);
        firing.reset ();
        TEST_ASSERT_EQUAL_INT (
          1, firing_state.entered.load (std::memory_order_relaxed));
        TEST_ASSERT_EQUAL_INT (
          1, firing_state.finished.load (std::memory_order_relaxed));
    }
}

int main (void)
{
    UNITY_BEGIN ();

    setup_test_environment ();

    RUN_TEST (test_cancel_before_deadline_prevents_handler_and_runs_cleanup);
    RUN_TEST (test_cancel_while_handler_is_firing_waits_for_handler_completion);
    RUN_TEST (test_cancel_notifies_scheduler_only_when_last_deadline_is_removed);
    RUN_TEST (test_schedule_across_idle_exit_boundary_fires_every_task);
    RUN_TEST (test_schedule_after_canceling_last_task_across_idle_exit_fires);

    return UNITY_END ();
}
