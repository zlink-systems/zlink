/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <memory>

#include "api/core/close_result_internal.hpp"
#include "api/core/config_result_internal.hpp"
#include "api/message/handler_result_internal.hpp"
#include "api/message/recv_result_internal.hpp"
#include "api/monitoring/timer_api_internal.hpp"

timer_handle_t *as_timer_handle (void *timer_)
{
    timer_handle_t *timer = static_cast<timer_handle_t *> (timer_);
    if (!timer || !timer->check_tag ()) {
        errno = EFAULT;
        return NULL;
    }
    return timer;
}

int timer_handle_signaler_fd (timer_handle_t *timer_, zlink_fd_t *fd_out_)
{
    if (!timer_ || !fd_out_) {
        errno = EFAULT;
        return -1;
    }

    const zlink::fd_t fd = timer_->signaler.get_fd ();
    if (fd == zlink::retired_fd) {
        errno = EFAULT;
        return -1;
    }

    *fd_out_ = static_cast<zlink_fd_t> (fd);
    return 0;
}

int timer_handle_acquire_poller_ref (timer_handle_t *timer_)
{
    if (!timer_) {
        errno = EFAULT;
        return -1;
    }

    std::lock_guard<std::mutex> lock (timer_->mutex);
    if (timer_->handler || timer_->receive_callback_active || timer_->poller_refs > 0) {
        errno = EBUSY;
        return -1;
    }
    ++timer_->poller_refs;
    return 0;
}

void timer_handle_release_poller_ref (timer_handle_t *timer_)
{
    if (!timer_)
        return;

    std::lock_guard<std::mutex> lock (timer_->mutex);
    if (timer_->poller_refs > 0)
        --timer_->poller_refs;
}

void *zlink_timer_new (void)
{
    // Generic timers stay on the timer scheduler backend and do not run on
    // socket I/O threads.
    std::unique_ptr<timer_handle_t> timer (new (std::nothrow) timer_handle_t ());
    if (!timer) {
        errno = ENOMEM;
        return NULL;
    }
    if (!timer->signaler.valid ()) {
        errno = EFAULT;
        return NULL;
    }

    std::shared_ptr<scheduler_state_t> scheduler = global_timer_scheduler ();
    timer->scheduler = scheduler.get ();
    {
        std::lock_guard<std::mutex> lock (scheduler->mutex);
        ++scheduler->active_timers;
    }
    ensure_scheduler_started (scheduler);
    return timer.release ();
}

zlink_close_result_t zlink_timer_destroy (void **timer_p_)
{
    if (!timer_p_ || !*timer_p_) {
        errno = EFAULT;
        return ZLINK_CLOSE_INVALID_HANDLE;
    }

    timer_handle_t *timer = as_timer_handle (*timer_p_);
    if (!timer)
        return ZLINK_CLOSE_INVALID_HANDLE;

    scheduler_state_t *scheduler = timer->scheduler;
    std::unique_lock<std::mutex> scheduler_lock (scheduler->mutex);
    std::unique_lock<std::mutex> timer_lock (timer->mutex);
    if (timer->poller_refs > 0) {
        errno = EBUSY;
        return ZLINK_CLOSE_BUSY;
    }
    timer->destroyed = true;
    timer->running = false;
    timer->stop_requested = true;
    remove_timer_registration_locked (timer);
    //  Release the scheduler lock before waiting for an in-flight fire: the
    //  fire's completion needs that lock to decrement scheduler_busy_refs.
    scheduler_lock.unlock ();
    timer_lock.unlock ();
    timer_lock.lock ();
    while (timer->scheduler_busy_refs > 0)
        timer->cv.wait (timer_lock);
    timer_lock.unlock ();
    scheduler->cv.notify_all ();
    timer->recv_cv.notify_all ();

    {
        std::lock_guard<std::mutex> lock (scheduler->mutex);
        if (scheduler->active_timers > 0)
            --scheduler->active_timers;
        if (scheduler->active_timers == 0 && scheduler->schedule.empty ())
            scheduler->shutdown_requested = true;
    }
    scheduler->cv.notify_all ();

    delete timer;
    *timer_p_ = NULL;
    return ZLINK_CLOSE_OK;
}

zlink_config_result_t
zlink_timer_start (void *timer_, uint64_t interval_ns_, uint64_t repeat_count_)
{
    timer_handle_t *timer = as_timer_handle (timer_);
    if (!timer) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }
    if (interval_ns_ == 0) {
        errno = EINVAL;
        return ZLINK_CONFIG_INVALID_ARGUMENT;
    }

    scheduler_state_t *scheduler = timer->scheduler;
    std::unique_lock<std::mutex> scheduler_lock (scheduler->mutex);
    std::lock_guard<std::mutex> lock (timer->mutex);
    timer->destroyed = false;
    timer->stop_requested = false;
    timer->running = true;
    timer->interval_ns = interval_ns_;
    timer->repeat_count = repeat_count_;
    timer->next_fire_count = 1;
    timer->fired_counts.clear ();
    drain_timer_signal_locked (timer);
    schedule_timer_locked (timer, monotonic_now_ns () + interval_ns_);
    scheduler->cv.notify_all ();
    return ZLINK_CONFIG_OK;
}

zlink_config_result_t zlink_timer_stop (void *timer_)
{
    timer_handle_t *timer = as_timer_handle (timer_);
    if (!timer)
        return ZLINK_CONFIG_INVALID_HANDLE;

    stop_timer_scheduler (timer);
    return ZLINK_CONFIG_OK;
}

zlink_recv_result_t zlink_timer_recv (void *timer_, uint64_t *fire_count_out_)
{
    timer_handle_t *timer = as_timer_handle (timer_);
    if (!timer || !fire_count_out_) {
        errno = EFAULT;
        return ZLINK_RECV_INVALID_HANDLE;
    }

    std::unique_lock<std::mutex> lock (timer->mutex);
    if (timer->handler || timer->receive_callback_active) {
        errno = EBUSY;
        return ZLINK_RECV_BUSY;
    }

    timer->recv_in_progress = true;
    while (timer->fired_counts.empty ()) {
        if (!timer->running) {
            timer->recv_in_progress = false;
            errno = EAGAIN;
            return ZLINK_RECV_NO_DATA;
        }
        timer->recv_cv.wait (lock);
    }

    *fire_count_out_ = timer->fired_counts.front ();
    timer->fired_counts.pop_front ();
    drain_timer_signal_locked (timer);
    ensure_timer_signal_locked (timer);
    timer->recv_in_progress = false;
    return ZLINK_RECV_OK;
}

zlink_handler_result_t
zlink_timer_handler (void *timer_, zlink_timer_handler_fn handler_, void *userdata_)
{
    timer_handle_t *timer = as_timer_handle (timer_);
    if (!timer) {
        errno = EFAULT;
        return ZLINK_HANDLER_INVALID_ARGUMENT;
    }
    if (!handler_) {
        errno = EINVAL;
        return ZLINK_HANDLER_INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> lock (timer->mutex);
    if (timer->recv_in_progress || timer->poller_refs > 0 || timer->receive_callback_active) {
        errno = EBUSY;
        return ZLINK_HANDLER_BUSY;
    }

    timer->handler = handler_;
    timer->handler_userdata = userdata_;
    return ZLINK_HANDLER_OK;
}
