/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <chrono>
#ifdef __linux__
#include <pthread.h>
#endif

#include "api/monitoring/timer_api_internal.hpp"
timer_handle_t::timer_handle_t () :
    tag (0x74696d72),
    scheduler (NULL),
    destroyed (false),
    running (false),
    receive_callback_active (false),
    recv_in_progress (false),
    stop_requested (false),
    signal_pending (false),
    poller_refs (0),
    scheduler_busy_refs (0),
    scheduler_registered (false),
    scheduled_deadline_ns (0),
    interval_ns (0),
    repeat_count (0),
    next_fire_count (1),
    handler (NULL),
    handler_userdata (NULL)
{
}

scheduler_state_t::scheduler_state_t () :
    started (false), shutdown_requested (false), active_timers (0)
{
}

namespace
{
std::shared_ptr<scheduler_state_t> g_global_scheduler (new scheduler_state_t ());
}

uint64_t monotonic_now_ns ()
{
    return static_cast<uint64_t> (std::chrono::duration_cast<std::chrono::nanoseconds> (
                                    std::chrono::steady_clock::now ().time_since_epoch ())
                                    .count ());
}

std::shared_ptr<scheduler_state_t> global_timer_scheduler ()
{
    return g_global_scheduler;
}

void drain_timer_signal_locked (timer_handle_t *timer_)
{
    if (!timer_ || !timer_->signal_pending)
        return;

    while (timer_->signaler.recv_failable () == 0) {
    }
    timer_->signal_pending = false;
}

void ensure_timer_signal_locked (timer_handle_t *timer_)
{
    if (!timer_ || timer_->signal_pending || timer_->fired_counts.empty ())
        return;

    timer_->signaler.send ();
    timer_->signal_pending = true;
}

void remove_timer_registration_locked (timer_handle_t *timer_)
{
    if (!timer_ || !timer_->scheduler || !timer_->scheduler_registered)
        return;

    scheduler_state_t *scheduler = timer_->scheduler;
    scheduler->schedule.erase (timer_->schedule_it);

    timer_->scheduler_registered = false;
    timer_->scheduled_deadline_ns = 0;
}

void schedule_timer_locked (timer_handle_t *timer_, uint64_t deadline_ns_)
{
    remove_timer_registration_locked (timer_);
    timer_->scheduler_registered = true;
    timer_->scheduled_deadline_ns = deadline_ns_;
    timer_->schedule_it =
      timer_->scheduler->schedule.insert (std::make_pair (deadline_ns_, timer_));
}

void scheduler_fire_timer (timer_handle_t *timer_)
{
    zlink_timer_handler_fn handler = NULL;
    void *handler_userdata = NULL;
    uint64_t fire_count = 0;
    bool reschedule = false;
    uint64_t next_deadline_ns = 0;
    scheduler_state_t *scheduler = NULL;

    {
        std::lock_guard<std::mutex> lock (timer_->mutex);
        scheduler = timer_->scheduler;
        if (!timer_->destroyed && timer_->running && !timer_->stop_requested) {
            fire_count = timer_->next_fire_count++;
            handler = timer_->handler;
            handler_userdata = timer_->handler_userdata;

            if (handler) {
                timer_->receive_callback_active = true;
            } else {
                timer_->fired_counts.push_back (fire_count);
                ensure_timer_signal_locked (timer_);
                timer_->recv_cv.notify_one ();
            }

            if (timer_->repeat_count > 0) {
                --timer_->repeat_count;
                if (timer_->repeat_count == 0)
                    timer_->running = false;
            }

            if (timer_->running && !timer_->destroyed && !timer_->stop_requested) {
                next_deadline_ns = monotonic_now_ns () + timer_->interval_ns;
                reschedule = true;
            }
        }
    }

    if (handler)
        handler (timer_, fire_count, handler_userdata);

    std::unique_lock<std::mutex> scheduler_lock (scheduler->mutex);
    std::unique_lock<std::mutex> timer_lock (timer_->mutex);
    if (handler) {
        timer_->receive_callback_active = false;
    }
    if (reschedule && !timer_->destroyed && timer_->running && !timer_->stop_requested) {
        schedule_timer_locked (timer_, next_deadline_ns);
        scheduler->cv.notify_all ();
    }
    if (timer_->scheduler_busy_refs > 0)
        --timer_->scheduler_busy_refs;
    timer_->cv.notify_all ();
}

void run_scheduler_loop (std::shared_ptr<scheduler_state_t> scheduler_)
{
#ifdef __linux__
    pthread_setname_np (pthread_self (), "zlink-timer");
#endif
    for (;;) {
        timer_handle_t *timer = NULL;
        {
            std::unique_lock<std::mutex> scheduler_lock (scheduler_->mutex);
            while (scheduler_->schedule.empty () && !scheduler_->shutdown_requested)
                scheduler_->cv.wait (scheduler_lock);

            if (scheduler_->shutdown_requested && scheduler_->schedule.empty ()) {
                scheduler_->started = false;
                scheduler_->shutdown_requested = false;
                break;
            }

            for (;;) {
                if (scheduler_->schedule.empty ()) {
                    timer = NULL;
                    break;
                }

                const uint64_t now_ns = monotonic_now_ns ();
                const std::multimap<uint64_t, timer_handle_t *>::iterator next =
                  scheduler_->schedule.begin ();
                if (next->first > now_ns) {
                    scheduler_->cv.wait_for (scheduler_lock,
                                             std::chrono::nanoseconds (next->first - now_ns));
                    continue;
                }

                timer = next->second;
                scheduler_->schedule.erase (next);
                {
                    std::lock_guard<std::mutex> timer_lock (timer->mutex);
                    timer->scheduler_registered = false;
                    timer->scheduled_deadline_ns = 0;
                    ++timer->scheduler_busy_refs;
                }
                break;
            }
        }

        if (!timer)
            continue;

        scheduler_fire_timer (timer);
    }
}

void ensure_scheduler_started (const std::shared_ptr<scheduler_state_t> &scheduler_)
{
    std::lock_guard<std::mutex> lock (scheduler_->mutex);
    scheduler_->shutdown_requested = false;
    if (scheduler_->started) {
        scheduler_->cv.notify_all ();
        return;
    }
    scheduler_->worker = std::thread (run_scheduler_loop, scheduler_);
    scheduler_->worker.detach ();
    scheduler_->started = true;
}

std::shared_ptr<scheduler_state_t> scheduler_for_timer (timer_handle_t *timer_)
{
    if (!timer_)
        return std::shared_ptr<scheduler_state_t> ();
    return g_global_scheduler;
}

void stop_timer_scheduler (timer_handle_t *timer_)
{
    if (!timer_ || !timer_->scheduler)
        return;

    scheduler_state_t *scheduler = timer_->scheduler;
    std::unique_lock<std::mutex> scheduler_lock (scheduler->mutex);
    std::unique_lock<std::mutex> timer_lock (timer_->mutex);
    timer_->stop_requested = true;
    timer_->running = false;
    remove_timer_registration_locked (timer_);
    timer_lock.unlock ();
    scheduler_lock.unlock ();
    scheduler->cv.notify_all ();
    timer_->recv_cv.notify_all ();
}
