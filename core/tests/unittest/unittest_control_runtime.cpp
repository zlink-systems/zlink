/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#include "core/ctx.hpp"
#include "core/control_runtime.hpp"

#include <atomic>
#include <new>

#include <unity.h>

namespace
{
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
          runtime.add_periodic_task (&throwing_task, NULL, 5, true);
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

int main (void)
{
    UNITY_BEGIN ();

    setup_test_environment ();

    RUN_TEST (test_worker_survives_task_bad_alloc_and_remove_returns);

    return UNITY_END ();
}
