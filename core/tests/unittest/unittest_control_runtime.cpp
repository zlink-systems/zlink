/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"
#include "../testutil_unity.hpp"

#include "core/ctx.hpp"
#include "core/control_runtime.hpp"

#include <atomic>
#include <limits.h>
#include <new>
#include <thread>

#include <unity.h>

namespace
{
const int test_wait_timeout_ms = 3000;
std::atomic<int> g_ticks (0);
std::atomic<bool> g_throw_once (false);

void throwing_task (void *)
{
    g_ticks.fetch_add (1, std::memory_order_relaxed);
    if (g_throw_once.exchange (false, std::memory_order_acq_rel))
        throw std::bad_alloc ();
}

void wait_for_ticks (int target_)
{
    const unsigned long timeout_us =
      static_cast<unsigned long> (SETTLE_TIME) * 20 * 1000;
    void *watch = zlink_stopwatch_start ();
    while (g_ticks.load (std::memory_order_relaxed) < target_) {
        msleep (1);
        TEST_ASSERT_LESS_OR_EQUAL_MESSAGE (
          timeout_us, zlink_stopwatch_intermediate (watch),
          "Timeout waiting for periodic task ticks");
    }
    zlink_stopwatch_stop (watch);
}

void barrier_task (void *arg_)
{
    static_cast<std::atomic<bool> *> (arg_)->store (true, std::memory_order_release);
}

void fence_callbacks (zlink::control_runtime_t &runtime_)
{
    std::atomic<bool> completed (false);
    const uint64_t task =
      runtime_.add_task (&barrier_task, &completed, UINT_MAX, false);
    TEST_ASSERT_TRUE (task != 0);
    TEST_ASSERT_EQUAL_INT (0, runtime_.schedule_task_after (task, 0));
    const bool ran = zlink_test_wait_until (test_wait_timeout_ms, [&] {
        return completed.load (std::memory_order_acquire);
    });
    TEST_ASSERT_EQUAL_INT (0, runtime_.remove_task (task));
    TEST_ASSERT_TRUE (ran);
}

struct rearming_task_t
{
    rearming_task_t () : runtime (NULL), task_id (0), calls (0), result (-1) {}
    zlink::control_runtime_t *runtime;
    uint64_t task_id;
    std::atomic<int> calls;
    std::atomic<int> result;
};

void rearming_task (void *arg_)
{
    rearming_task_t &state = *static_cast<rearming_task_t *> (arg_);
    if (state.calls.fetch_add (1, std::memory_order_relaxed) == 0)
        state.result.store (
          state.runtime->schedule_task_after (state.task_id, 0),
          std::memory_order_release);
}

struct held_task_t
{
    held_task_t () : calls (0), release (false) {}
    std::atomic<int> calls;
    std::atomic<bool> release;
};

void held_task (void *arg_)
{
    held_task_t &state = *static_cast<held_task_t *> (arg_);
    if (state.calls.fetch_add (1, std::memory_order_relaxed) == 0)
        while (!state.release.load (std::memory_order_acquire))
            std::this_thread::yield ();
}
}

void setUp ()
{
}

void tearDown ()
{
}

void test_worker_survives_task_bad_alloc_and_remove_returns ()
{
    //  Regression for S5-15-01: a task callback that throws bad_alloc must
    //  not take the worker down or leave a stale active-task ID. The task
    //  keeps its schedule (the failed tick is dropped), later ticks run, and
    //  remove_task()/teardown return instead of waiting forever.
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    zlink::ctx_t *ctx = static_cast<zlink::ctx_t *> (ctx_handle);

    {
        zlink::control_runtime_t runtime (ctx, "core-ut");
        TEST_ASSERT_TRUE (runtime.start ());

        g_ticks.store (0, std::memory_order_relaxed);
        g_throw_once.store (true, std::memory_order_release);
        const uint64_t task_id =
          runtime.add_task (&throwing_task, NULL, 5, true);
        TEST_ASSERT_TRUE (task_id != 0);

        //  The first tick throws; observing three ticks proves the worker
        //  survived it and the task stayed scheduled.
        wait_for_ticks (3);

        TEST_ASSERT_EQUAL_INT (0, runtime.remove_task (task_id));
        runtime.stop ();
    }

    TEST_ASSERT_EQUAL_INT (static_cast<int> (ZLINK_CLOSE_OK),
                           static_cast<int> (zlink_ctx_term (ctx_handle)));
}

void test_task_can_be_scheduled_at_owned_deadline ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    zlink::ctx_t *ctx = static_cast<zlink::ctx_t *> (ctx_handle);

    {
        zlink::control_runtime_t runtime (ctx, "core-ut");
        TEST_ASSERT_TRUE (runtime.start ());

        g_ticks.store (0, std::memory_order_relaxed);
        g_throw_once.store (false, std::memory_order_release);
        const uint64_t task_id =
          runtime.add_task (&throwing_task, NULL, UINT_MAX, false);
        TEST_ASSERT_TRUE (task_id != 0);
        TEST_ASSERT_EQUAL_INT (0, runtime.schedule_task_after (task_id, 1));
        wait_for_ticks (1);

        msleep (10);
        TEST_ASSERT_EQUAL_INT (1, g_ticks.load (std::memory_order_relaxed));
        TEST_ASSERT_EQUAL_INT (0, runtime.remove_task (task_id));
        runtime.stop ();
    }

    TEST_ASSERT_EQUAL_INT (static_cast<int> (ZLINK_CLOSE_OK),
                           static_cast<int> (zlink_ctx_term (ctx_handle)));
}

void test_event_task_stays_dormant_and_runs_once_per_wake ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    {
        zlink::control_runtime_t runtime (
          static_cast<zlink::ctx_t *> (ctx_handle), "core-ut");
        TEST_ASSERT_TRUE (runtime.start ());
        g_ticks.store (0, std::memory_order_relaxed);
        g_throw_once.store (false, std::memory_order_release);
        const uint64_t task = runtime.add_task (&throwing_task, NULL, 0, false);
        TEST_ASSERT_TRUE (task != 0);
        fence_callbacks (runtime);
        TEST_ASSERT_EQUAL_INT (0, g_ticks.load (std::memory_order_relaxed));
        for (int wake = 1; wake <= 2; ++wake) {
            TEST_ASSERT_EQUAL_INT (0, runtime.schedule_task_after (task, 0));
            wait_for_ticks (wake);
            fence_callbacks (runtime);
            TEST_ASSERT_EQUAL_INT (wake, g_ticks.load (std::memory_order_relaxed));
        }
        TEST_ASSERT_EQUAL_INT (0, runtime.remove_task (task));
        runtime.stop ();
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (ctx_handle));
}

void test_event_task_preserves_rearm_from_its_callback ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    {
        zlink::control_runtime_t runtime (
          static_cast<zlink::ctx_t *> (ctx_handle), "core-ut");
        TEST_ASSERT_TRUE (runtime.start ());
        rearming_task_t state;
        state.runtime = &runtime;
        state.task_id = runtime.add_task (&rearming_task, &state, 0, false);
        TEST_ASSERT_TRUE (state.task_id != 0);
        TEST_ASSERT_EQUAL_INT (0, runtime.schedule_task_after (state.task_id, 0));
        TEST_ASSERT_TRUE (zlink_test_wait_until (test_wait_timeout_ms, [&] {
            return state.calls.load (std::memory_order_relaxed) >= 2;
        }));
        fence_callbacks (runtime);
        TEST_ASSERT_EQUAL_INT (0, state.result.load (std::memory_order_acquire));
        TEST_ASSERT_EQUAL_INT (2, state.calls.load (std::memory_order_relaxed));
        TEST_ASSERT_EQUAL_INT (0, runtime.remove_task (state.task_id));
        runtime.stop ();
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (ctx_handle));
}

void test_event_task_preserves_wake_published_while_active ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    {
        zlink::control_runtime_t runtime (
          static_cast<zlink::ctx_t *> (ctx_handle), "core-ut");
        TEST_ASSERT_TRUE (runtime.start ());
        held_task_t state;
        const uint64_t task = runtime.add_task (&held_task, &state, 0, false);
        TEST_ASSERT_TRUE (task != 0);
        TEST_ASSERT_EQUAL_INT (0, runtime.schedule_task_after (task, 0));
        const bool started = zlink_test_wait_until (test_wait_timeout_ms, [&] {
            return state.calls.load (std::memory_order_relaxed) == 1;
        });
        const int schedule_result = runtime.schedule_task_after (task, 0);
        // Release the owned callback before reporting a scheduling failure.
        state.release.store (true, std::memory_order_release);
        TEST_ASSERT_TRUE (started);
        TEST_ASSERT_EQUAL_INT (0, schedule_result);
        TEST_ASSERT_TRUE (zlink_test_wait_until (test_wait_timeout_ms, [&] {
            return state.calls.load (std::memory_order_relaxed) >= 2;
        }));
        fence_callbacks (runtime);
        TEST_ASSERT_EQUAL_INT (2, state.calls.load (std::memory_order_relaxed));
        TEST_ASSERT_EQUAL_INT (0, runtime.remove_task (task));
        runtime.stop ();
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (ctx_handle));
}

int main (void)
{
    UNITY_BEGIN ();

    setup_test_environment ();

    RUN_TEST (test_worker_survives_task_bad_alloc_and_remove_returns);
    RUN_TEST (test_task_can_be_scheduled_at_owned_deadline);
    RUN_TEST (test_event_task_stays_dormant_and_runs_once_per_wake);
    RUN_TEST (test_event_task_preserves_rearm_from_its_callback);
    RUN_TEST (test_event_task_preserves_wake_published_while_active);

    return UNITY_END ();
}
