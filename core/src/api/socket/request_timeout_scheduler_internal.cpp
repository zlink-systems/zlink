/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#ifdef __linux__
#include <pthread.h>
#endif
#include <thread>

#include "api/socket/request_timeout_scheduler_internal.hpp"

namespace zlink
{
namespace request_timeout
{
namespace
{
typedef std::multimap<uint64_t, std::shared_ptr<struct task_t>> schedule_map_t;
const uint64_t idle_exit_wait_ns = static_cast<uint64_t> (100) * static_cast<uint64_t> (1000000);
}

struct task_t
{
    task_t () :
        registered (false),
        canceled (false),
        firing (false),
        completed (false),
        deadline_ns (0),
        handler (NULL),
        cleanup (NULL),
        userdata (NULL),
        schedule_it (schedule_map_t::iterator ())
    {
    }

    ~task_t ()
    {
        if (cleanup && userdata)
            cleanup (userdata);
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool registered;
    bool canceled;
    bool firing;
    bool completed;
    //  Lets cancel() detect a self-cancel from inside the firing handler
    //  (an operation completing itself on timeout) and skip the wait that
    //  would otherwise deadlock on its own completion.
    std::thread::id firing_thread;
    uint64_t deadline_ns;
    handler_fn handler;
    cleanup_fn cleanup;
    void *userdata;
    schedule_map_t::iterator schedule_it;
};

namespace
{
struct scheduler_state_t
{
    std::mutex mutex;
    std::condition_variable cv;
    schedule_map_t schedule;
    std::thread thread;
    uint64_t next_wake_ns;
    bool started;

    scheduler_state_t () : next_wake_ns (0), started (false) {}
};

scheduler_state_t &scheduler_state ()
{
    //  Intentionally immortal: the detached timeout thread can outlive static
    //  destruction at process exit, so this state must never be destroyed.
    static scheduler_state_t *state = new scheduler_state_t ();
    return *state;
}

void run_timeout_loop ()
{
#ifdef __linux__
    pthread_setname_np (pthread_self (), "zlink-req-time");
#endif
    scheduler_state_t &state = scheduler_state ();
    for (;;) {
        std::shared_ptr<task_t> task;
        {
            std::unique_lock<std::mutex> lock (state.mutex);
            while (state.schedule.empty ()) {
                state.next_wake_ns = monotonic_now_ns () + idle_exit_wait_ns;
                if (state.cv.wait_for (lock, std::chrono::nanoseconds (idle_exit_wait_ns))
                      == std::cv_status::timeout
                    && state.schedule.empty ()) {
                    state.next_wake_ns = 0;
                    state.started = false;
                    return;
                }
            }

            for (;;) {
                if (state.schedule.empty ())
                    break;

                const uint64_t now_ns = monotonic_now_ns ();
                const schedule_map_t::iterator next = state.schedule.begin ();
                if (next->first > now_ns) {
                    state.next_wake_ns = next->first;
                    state.cv.wait_for (lock, std::chrono::nanoseconds (next->first - now_ns));
                    continue;
                }

                task = next->second;
                state.schedule.erase (next);
                task->registered = false;
                task->schedule_it = schedule_map_t::iterator ();
                break;
            }
        }

        if (!task)
            continue;

        handler_fn handler = NULL;
        void *userdata = NULL;
        {
            std::lock_guard<std::mutex> lock (task->mutex);
            if (!task->canceled && !task->completed) {
                task->firing = true;
                task->firing_thread = std::this_thread::get_id ();
                handler = task->handler;
                userdata = task->userdata;
                task->userdata = NULL;
                task->cleanup = NULL;
            }
        }

        if (handler)
            handler (userdata);

        {
            std::lock_guard<std::mutex> lock (task->mutex);
            task->firing = false;
            task->completed = true;
            task->cv.notify_all ();
        }
    }
}

}

std::shared_ptr<task_t>
schedule (uint32_t timeout_ms_, handler_fn handler_, void *userdata_, cleanup_fn cleanup_)
{
    if (timeout_ms_ == 0 || !handler_)
        return std::shared_ptr<task_t> ();

    std::shared_ptr<task_t> task (new task_t ());
    task->handler = handler_;
    task->cleanup = cleanup_;
    task->userdata = userdata_;
    task->deadline_ns = deadline_after_ms (timeout_ms_);
    task->registered = true;

    {
        scheduler_state_t &state = scheduler_state ();
        std::lock_guard<std::mutex> lock (state.mutex);
        // Hot path: only wake the scheduler when this request becomes the next
        // deadline. Notifying on every request reintroduces cross-thread wake
        // churn in high-rate request/reply workloads.
        const bool should_notify = state.next_wake_ns == 0 || task->deadline_ns < state.next_wake_ns;
        //  The liveness check must share this critical section with the
        //  insert: the scheduler thread commits its idle exit under the same
        //  lock, so checking `started` in a separate lock hold can strand the
        //  new task with no consumer. Starting the thread before the insert
        //  keeps the map clean if thread creation throws; the fresh thread
        //  observes the task once this lock is released.
        const bool starting = !state.started;
        if (starting) {
            state.thread = std::thread (run_timeout_loop);
            state.thread.detach ();
            state.started = true;
        }
        task->schedule_it = state.schedule.insert (std::make_pair (task->deadline_ns, task));
        if (!starting && should_notify)
            state.cv.notify_all ();
    }
    return task;
}

void cancel (const std::shared_ptr<task_t> &task_)
{
    if (!task_)
        return;

    bool notify_scheduler = false;
    {
        scheduler_state_t &state = scheduler_state ();
        std::lock_guard<std::mutex> schedule_lock (state.mutex);
        if (task_->registered) {
            if (task_->schedule_it != state.schedule.end ())
                state.schedule.erase (task_->schedule_it);
            task_->registered = false;
            task_->schedule_it = schedule_map_t::iterator ();
            notify_scheduler = true;
        }
    }
    if (notify_scheduler)
        scheduler_state ().cv.notify_all ();

    std::unique_lock<std::mutex> lock (task_->mutex);
    task_->canceled = true;
    while (task_->firing && task_->firing_thread != std::this_thread::get_id ())
        task_->cv.wait (lock);
    task_->completed = true;
}

uint64_t monotonic_now_ns ()
{
    return static_cast<uint64_t> (std::chrono::duration_cast<std::chrono::nanoseconds> (
                                    std::chrono::steady_clock::now ().time_since_epoch ())
                                    .count ());
}

uint64_t deadline_after_ms (uint32_t timeout_ms_)
{
    return monotonic_now_ns ()
           + static_cast<uint64_t> (timeout_ms_) * static_cast<uint64_t> (1000000);
}
}
}
