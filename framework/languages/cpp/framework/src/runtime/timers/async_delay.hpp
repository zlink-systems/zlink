/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>
#include "runtime/timers/core_timer_drain_loop.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace zlink::framework::detail
{

// The host owns every delay worker and uses its teardown mutex to serialize
// registration, cancellation, and access to each worker's Core timer.
class delay_owner_t
{
  public:
    explicit delay_owner_t (std::mutex &mutex) : _mutex (mutex) {}
    ~delay_owner_t () { stop_and_join (); }

    delay_owner_t (const delay_owner_t &) = delete;
    delay_owner_t &operator= (const delay_owner_t &) = delete;

    task_t<void> start (std::chrono::milliseconds duration, bool accepted_relocation = false)
    {
        auto source = std::make_shared<task_completion_source_t<void>> ();
        auto pending = source->task ();
        bool rejected = false;
        {
            std::lock_guard lock (_mutex);
            if (stopping_locked (accepted_relocation)) {
                rejected = true;
            } else {
                reap_completed_locked ();
                auto worker = std::make_shared<worker_t> ();
                _workers.push_back (worker);
                try {
                    worker->thread =
                      std::thread ([this, worker, source, duration, accepted_relocation] {
                          run_worker (*worker, source, duration, accepted_relocation);
                      });
                }
                catch (const std::exception &) {
                    _workers.pop_back ();
                    throw;
                }
            }
        }
        if (rejected)
            source->complete (shutdown_result ());
        return pending;
    }

    void request_stop (
      std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now ()) noexcept
    {
        std::lock_guard lock (_mutex);
        if (_stop_at)
            return;
        _stop_at = deadline;
        wake_workers_locked ();
    }

    std::optional<std::chrono::steady_clock::time_point> stop_deadline () const noexcept
    {
        std::lock_guard lock (_mutex);
        return _stop_at;
    }

    void reap_completed ()
    {
        std::lock_guard lock (_mutex);
        reap_completed_locked ();
    }

    std::size_t worker_count () const
    {
        std::lock_guard lock (_mutex);
        return _workers.size ();
    }

    void stop_and_join () noexcept
    {
        std::vector<std::shared_ptr<worker_t>> workers;
        {
            std::lock_guard lock (_mutex);
            // Final teardown closes registration before detaching the workers.
            // An accepted relocation cannot register another worker during join.
            const auto now = std::chrono::steady_clock::now ();
            _stop_at = _stop_at ? std::min (*_stop_at, now) : now;
            wake_workers_locked ();
            workers.swap (_workers);
        }
        for (auto &worker : workers)
            worker->thread.join ();
    }

  private:
    static constexpr auto wake_interval = std::chrono::nanoseconds (1);

    struct worker_t
    {
        std::thread thread;
        std::unique_ptr<zlink::timer_t> timer;
        std::unique_ptr<zlink::poller_t> poller;
        std::atomic_bool completed{false};
    };

    static result_t<void> shutdown_result ()
    {
        return result_t<void>::failure (framework_error_kind_t::shutting_down,
                                        "Framework host is stopping");
    }

    bool stopping_locked (bool accepted_relocation) const noexcept
    {
        return _stop_at && (!accepted_relocation || std::chrono::steady_clock::now () >= *_stop_at);
    }

    void wake_workers_locked () noexcept
    {
        for (const auto &worker : _workers) {
            if (!worker->timer)
                continue;
            try {
                // stop() does not signal the Core poller. A one-shot firing does.
                worker->timer->start (wake_interval, 1);
            }
            catch (const std::exception &) {
                // The bounded poller wait still observes the owner deadline.
            }
        }
    }

    void reap_completed_locked ()
    {
        auto it = _workers.begin ();
        while (it != _workers.end ()) {
            if ((*it)->completed.load (std::memory_order_acquire)) {
                (*it)->thread.join ();
                it = _workers.erase (it);
            } else {
                ++it;
            }
        }
    }

    void run_worker (worker_t &worker,
                     const std::shared_ptr<task_completion_source_t<void>> &source,
                     std::chrono::milliseconds duration,
                     bool accepted_relocation) noexcept
    {
        runtime::runtime_failure_collector_t failures;
        failures.capture ([&] {
            {
                std::lock_guard lock (_mutex);
                if (stopping_locked (accepted_relocation))
                    return;
            }
            auto timer = std::make_unique<zlink::timer_t> ();
            auto poller = std::make_unique<zlink::poller_t> ();
            poller->add (*timer, 1);
            {
                std::lock_guard lock (_mutex);
                worker.timer = std::move (timer);
                worker.poller = std::move (poller);
                if (stopping_locked (accepted_relocation))
                    return;
                auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                  duration > std::chrono::milliseconds::zero () ? duration
                                                                : std::chrono::milliseconds (1));
                if (accepted_relocation && _stop_at)
                    interval = std::min (interval, *_stop_at - std::chrono::steady_clock::now ());
                if (interval <= std::chrono::steady_clock::duration::zero ())
                    return;
                worker.timer->start (interval, 1);
            }
            std::array<zlink::poll_event_t, 1> events{};
            for (;;) {
                {
                    std::lock_guard lock (_mutex);
                    if (stopping_locked (accepted_relocation))
                        break;
                }
                if (worker.poller->wait (events.data (), events.size (),
                                         core_timer_drain_loop_t::poll_interval)
                    == 0)
                    continue;
                std::lock_guard lock (_mutex);
                if (stopping_locked (accepted_relocation))
                    break;
                if (const auto count = worker.timer->recv (); count && *count)
                    break;
            }
        });
        bool stopping;
        {
            std::lock_guard lock (_mutex);
            if (worker.poller)
                failures.capture ([&] { worker.poller->close (); });
            if (worker.timer)
                failures.capture ([&] { worker.timer->close (); });
            worker.poller.reset ();
            worker.timer.reset ();
            // This read decides the result against request_stop under one mutex.
            // Publishing it below can resume a caller, so it stays outside the lock.
            stopping = stopping_locked (accepted_relocation);
        }
        auto result = stopping ? shutdown_result () : result_t<void>::success ();
        if (!stopping) {
            try {
                failures.rethrow_if_failed ();
            }
            catch (const std::exception &) {
                result = current_exception_result<void> ();
            }
        }
        source->complete (std::move (result));
        worker.completed.store (true, std::memory_order_release);
    }

    std::mutex &_mutex;
    std::optional<std::chrono::steady_clock::time_point> _stop_at;
    std::vector<std::shared_ptr<worker_t>> _workers;
};

inline task_t<void>
delay (delay_owner_t &owner, std::chrono::milliseconds duration, bool accepted_relocation = false)
{
    return owner.start (duration, accepted_relocation);
}

inline task_t<void>
delay (delay_owner_t *owner, std::chrono::milliseconds duration, bool accepted_relocation = false)
{
    if (!owner)
        return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_configured,
                                                      "Framework delay has no host owner"));
    return owner->start (duration, accepted_relocation);
}

} // namespace zlink::framework::detail
