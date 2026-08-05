/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_TIMER_API_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_TIMER_API_INTERNAL_HPP_INCLUDED__

#include "utils/precompiled.hpp"

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <thread>

#include "core/signaler.hpp"

struct scheduler_state_t;

struct timer_handle_t
{
    explicit timer_handle_t ();

    bool check_tag () const { return tag == 0x74696d72; }

    uint32_t tag;
    scheduler_state_t *scheduler;
    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable recv_cv;
    zlink::signaler_t signaler;
    bool destroyed;
    bool running;
    bool receive_callback_active;
    bool recv_in_progress;
    bool stop_requested;
    bool signal_pending;
    int poller_refs;
    int scheduler_busy_refs;
    bool scheduler_registered;
    uint64_t scheduled_deadline_ns;
    std::multimap<uint64_t, timer_handle_t *>::iterator schedule_it;
    uint64_t interval_ns;
    uint64_t repeat_count;
    uint64_t next_fire_count;
    std::deque<uint64_t> fired_counts;
    zlink_timer_handler_fn handler;
    void *handler_userdata;
};

struct scheduler_state_t
{
    explicit scheduler_state_t ();

    std::mutex mutex;
    std::condition_variable cv;
    std::multimap<uint64_t, timer_handle_t *> schedule;
    std::thread worker;
    bool started;
    bool shutdown_requested;
    size_t active_timers;
};

uint64_t monotonic_now_ns ();
std::shared_ptr<scheduler_state_t> global_timer_scheduler ();
void drain_timer_signal_locked (timer_handle_t *timer_);
void ensure_timer_signal_locked (timer_handle_t *timer_);
void remove_timer_registration_locked (timer_handle_t *timer_);
void schedule_timer_locked (timer_handle_t *timer_, uint64_t deadline_ns_);
void scheduler_fire_timer (timer_handle_t *timer_);
void run_scheduler_loop (std::shared_ptr<scheduler_state_t> scheduler_);
void ensure_scheduler_started (const std::shared_ptr<scheduler_state_t> &scheduler_);
std::shared_ptr<scheduler_state_t> scheduler_for_timer (timer_handle_t *timer_);
void stop_timer_scheduler (timer_handle_t *timer_);

timer_handle_t *as_timer_handle (void *timer_);
int timer_handle_signaler_fd (timer_handle_t *timer_, zlink_fd_t *fd_out_);
int timer_handle_acquire_poller_ref (timer_handle_t *timer_);
void timer_handle_release_poller_ref (timer_handle_t *timer_);

#endif
