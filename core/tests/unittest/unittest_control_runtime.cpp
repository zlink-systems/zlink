/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#include "core/ctx.hpp"
#include "core/control_runtime.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits.h>
#include <mutex>
#include <new>
#include <thread>

#include <unity.h>

namespace
{
struct task_probe_t
{
    task_probe_t () : calls (0), hold_first (false), released (false), throw_first (false) {}

    bool wait_for_calls (int target_)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return changed.wait_for (lock, std::chrono::milliseconds (SETTLE_TIME * 20),
                                 [this, target_] { return calls >= target_; });
    }

    void release ()
    {
        std::lock_guard<std::mutex> lock (mutex);
        released = true;
        changed.notify_all ();
    }

    int call_count ()
    {
        std::lock_guard<std::mutex> lock (mutex);
        return calls;
    }

    std::mutex mutex;
    std::condition_variable changed;
    int calls;
    bool hold_first;
    bool released;
    bool throw_first;
};

void tracked_task (void *arg_)
{
    task_probe_t &probe = *static_cast<task_probe_t *> (arg_);
    std::unique_lock<std::mutex> lock (probe.mutex);
    const bool first = ++probe.calls == 1;
    probe.changed.notify_all ();
    if (first && probe.hold_first)
        probe.changed.wait (lock, [&probe] { return probe.released; });
    if (first && probe.throw_first) {
        lock.unlock ();
        throw std::bad_alloc ();
    }
}

bool complete_scheduler_turn (zlink::control_runtime_t &runtime_)
{
    // A separate callback cannot enter until the preceding callback and its
    // active-task epilogue have completed on the sole control worker.
    task_probe_t barrier;
    const uint64_t id = runtime_.add_periodic_task (&tracked_task, &barrier, 0, true);
    if (id == 0)
        return false;
    const bool entered = barrier.wait_for_calls (1);
    runtime_.remove_task (id);
    return entered;
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

        task_probe_t probe;
        probe.throw_first = true;
        const uint64_t task_id = runtime.add_periodic_task (&tracked_task, &probe, 5, true);
        TEST_ASSERT_TRUE (task_id != 0);

        //  The first tick throws; observing three ticks proves the worker
        //  survived it and the task stayed scheduled.
        const bool three_calls = probe.wait_for_calls (3);

        TEST_ASSERT_EQUAL_INT (0, runtime.remove_task (task_id));
        runtime.stop ();
        TEST_ASSERT_TRUE_MESSAGE (three_calls, "periodic task stopped after bad_alloc");
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

        task_probe_t probe;
        const uint64_t task_id = runtime.add_periodic_task (&tracked_task, &probe, UINT_MAX, false);
        TEST_ASSERT_TRUE (task_id != 0);
        TEST_ASSERT_EQUAL_INT (0, runtime.schedule_task_after (task_id, 1));
        const bool first_call = probe.wait_for_calls (1);
        const bool barrier_completed = complete_scheduler_turn (runtime);

        const bool periodic_deadline_retained = runtime.test_task_scheduled (task_id);
        TEST_ASSERT_EQUAL_INT (0, runtime.remove_task (task_id));
        runtime.stop ();
        TEST_ASSERT_TRUE (first_call);
        TEST_ASSERT_TRUE (barrier_completed);
        TEST_ASSERT_TRUE_MESSAGE (periodic_deadline_retained,
                                  "UINT_MAX periodic task lost its next deadline");
        TEST_ASSERT_EQUAL_INT (1, probe.call_count ());
    }

    TEST_ASSERT_EQUAL_INT (static_cast<int> (ZLINK_CLOSE_OK),
                           static_cast<int> (zlink_ctx_term (ctx_handle)));
}

void test_zero_interval_task_stays_parked_until_explicit_wake ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    zlink::ctx_t *ctx = static_cast<zlink::ctx_t *> (ctx_handle);

    {
        zlink::control_runtime_t runtime (ctx, "core-ut");
        TEST_ASSERT_TRUE (runtime.start ());
        task_probe_t probe;
        const uint64_t id = runtime.add_periodic_task (&tracked_task, &probe, 0, false);
        TEST_ASSERT_TRUE (id != 0);
        const bool initially_registered = runtime.test_task_exists (id);
        const bool initially_parked = !runtime.test_task_scheduled (id);
        const bool initial_barrier = complete_scheduler_turn (runtime);
        const int initial_calls = probe.call_count ();

        const int wake_rc = runtime.schedule_task_after (id, 0);
        const bool delivered = probe.wait_for_calls (1);
        const bool completed_barrier = complete_scheduler_turn (runtime);
        const bool parked_after_call = !runtime.test_task_scheduled (id);
        runtime.remove_task (id);
        runtime.stop ();

        TEST_ASSERT_TRUE (initially_registered);
        TEST_ASSERT_TRUE (initially_parked);
        TEST_ASSERT_TRUE (initial_barrier);
        TEST_ASSERT_EQUAL_INT (0, initial_calls);
        TEST_ASSERT_EQUAL_INT (0, wake_rc);
        TEST_ASSERT_TRUE (delivered);
        TEST_ASSERT_TRUE (completed_barrier);
        TEST_ASSERT_TRUE_MESSAGE (parked_after_call,
                                  "nonperiodic task retained an automatic deadline");
        TEST_ASSERT_EQUAL_INT (1, probe.call_count ());
    }

    TEST_ASSERT_EQUAL_INT (static_cast<int> (ZLINK_CLOSE_OK),
                           static_cast<int> (zlink_ctx_term (ctx_handle)));
}

void test_wake_during_callback_survives_callback_completion ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    zlink::ctx_t *ctx = static_cast<zlink::ctx_t *> (ctx_handle);

    {
        zlink::control_runtime_t runtime (ctx, "core-ut");
        TEST_ASSERT_TRUE (runtime.start ());
        task_probe_t probe;
        probe.hold_first = true;
        const uint64_t id = runtime.add_periodic_task (&tracked_task, &probe, 0, true);
        TEST_ASSERT_TRUE (id != 0);
        const bool entered = probe.wait_for_calls (1);
        const bool active_task_unscheduled = !runtime.test_task_scheduled (id);
        const int wake_rc = runtime.schedule_task_after (id, 0);
        const bool wake_retained = runtime.test_task_scheduled (id);
        probe.release ();
        const bool second_call = probe.wait_for_calls (2);
        const bool completed_barrier = complete_scheduler_turn (runtime);
        const bool parked_after_calls = !runtime.test_task_scheduled (id);
        runtime.remove_task (id);
        runtime.stop ();

        TEST_ASSERT_TRUE (entered);
        TEST_ASSERT_TRUE (active_task_unscheduled);
        TEST_ASSERT_EQUAL_INT (0, wake_rc);
        TEST_ASSERT_TRUE (wake_retained);
        TEST_ASSERT_TRUE_MESSAGE (second_call, "callback completion erased a concurrent wake");
        TEST_ASSERT_TRUE (completed_barrier);
        TEST_ASSERT_TRUE (parked_after_calls);
        TEST_ASSERT_EQUAL_INT (2, probe.call_count ());
    }

    TEST_ASSERT_EQUAL_INT (static_cast<int> (ZLINK_CLOSE_OK),
                           static_cast<int> (zlink_ctx_term (ctx_handle)));
}

void test_remove_retires_task_before_joining_active_callback ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    zlink::ctx_t *ctx = static_cast<zlink::ctx_t *> (ctx_handle);

    {
        zlink::control_runtime_t runtime (ctx, "core-ut");
        TEST_ASSERT_TRUE (runtime.start ());
        task_probe_t probe;
        probe.hold_first = true;
        const uint64_t id = runtime.add_periodic_task (&tracked_task, &probe, 0, true);
        TEST_ASSERT_TRUE (id != 0);
        const bool entered = probe.wait_for_calls (1);
        std::atomic<bool> remove_returned (false);
        int remove_rc = -1;
        std::thread remover ([&] {
            remove_rc = runtime.remove_task (id);
            remove_returned.store (true, std::memory_order_release);
        });

        // Removal of the registered ID is a positive observation that the
        // remover has reached its active-callback join boundary.
        const std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (SETTLE_TIME * 20);
        while (runtime.test_task_exists (id) && std::chrono::steady_clock::now () < deadline)
            std::this_thread::yield ();
        const bool retired = !runtime.test_task_exists (id);
        const bool joined_before_release = remove_returned.load (std::memory_order_acquire);
        const int wake_rc = runtime.schedule_task_after (id, 0);
        const int wake_errno = errno;
        probe.release ();
        remover.join ();
        runtime.stop ();

        TEST_ASSERT_TRUE (entered);
        TEST_ASSERT_TRUE_MESSAGE (retired, "active task was not retired before join");
        TEST_ASSERT_FALSE_MESSAGE (joined_before_release,
                                   "remove returned while callback still held its argument");
        TEST_ASSERT_EQUAL_INT (-1, wake_rc);
        TEST_ASSERT_EQUAL_INT (EINVAL, wake_errno);
        TEST_ASSERT_EQUAL_INT (0, remove_rc);
        TEST_ASSERT_TRUE (remove_returned.load (std::memory_order_acquire));
        TEST_ASSERT_EQUAL_INT (1, probe.call_count ());
    }

    TEST_ASSERT_EQUAL_INT (static_cast<int> (ZLINK_CLOSE_OK),
                           static_cast<int> (zlink_ctx_term (ctx_handle)));
}

void test_stop_wakes_worker_with_only_parked_tasks ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    zlink::ctx_t *ctx = static_cast<zlink::ctx_t *> (ctx_handle);

    {
        zlink::control_runtime_t runtime (ctx, "core-ut");
        TEST_ASSERT_TRUE (runtime.start ());
        task_probe_t probe;
        const uint64_t id = runtime.add_periodic_task (&tracked_task, &probe, 0, false);
        TEST_ASSERT_TRUE (id != 0);
        const bool barrier_completed = complete_scheduler_turn (runtime);
        runtime.stop ();

        TEST_ASSERT_TRUE (barrier_completed);
        TEST_ASSERT_FALSE (runtime.test_task_exists (id));
        TEST_ASSERT_EQUAL_INT (0, probe.call_count ());
    }

    TEST_ASSERT_EQUAL_INT (static_cast<int> (ZLINK_CLOSE_OK),
                           static_cast<int> (zlink_ctx_term (ctx_handle)));
}

int main (void)
{
    UNITY_BEGIN ();

    setup_test_environment ();

    RUN_TEST (test_worker_survives_task_bad_alloc_and_remove_returns);
    RUN_TEST (test_task_can_be_scheduled_at_owned_deadline);
    RUN_TEST (test_zero_interval_task_stays_parked_until_explicit_wake);
    RUN_TEST (test_wake_during_callback_survives_callback_completion);
    RUN_TEST (test_remove_retires_task_before_joining_active_callback);
    RUN_TEST (test_stop_wakes_worker_with_only_parked_tasks);

    return UNITY_END ();
}
