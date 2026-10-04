/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Eventing/timers.hpp>

#include "runtime/host/runtime_failure_collector.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace zlink::framework::detail
{

/* Owns a Core timer together with the readiness poller that drains it.
 * Core 0.16.0 deliberately exposes timer notification as pull readiness;
 * keeping the poller beside the timer makes that ownership and shutdown
 * boundary explicit at every framework call site. */
class core_timer_drain_loop_t
{
  public:
    static constexpr std::chrono::milliseconds poll_interval{50};

    explicit core_timer_drain_loop_t (
      std::shared_ptr<runtime::runtime_failure_collector_t> failures) :
        _failures (std::move (failures))
    {
    }

    ~core_timer_drain_loop_t () noexcept
    {
        try {
            close ();
        }
        catch (...) {
            _failures->report (std::current_exception ());
            auto native = _native;
            _failures->retain ([native] {
                native->poller.close ();
                native->timer.close ();
            });
        }
    }

    core_timer_drain_loop_t (const core_timer_drain_loop_t &) = delete;
    core_timer_drain_loop_t &operator= (const core_timer_drain_loop_t &) = delete;

    template <class Rep, class Period>
    void start (std::chrono::duration<Rep, Period> interval,
                std::uint64_t repeat_count,
                std::function<void (std::uint64_t)> drain)
    {
        std::lock_guard lock (_lifecycle_mutex);
        if (!_native || _stop.load (std::memory_order_acquire) || _worker.joinable ())
            return;
        _native->poller.add (_native->timer, 1);
        _native->timer.start (interval, repeat_count);
        _stop.store (false, std::memory_order_release);
        _worker = std::thread (
          [this, native = _native, drain = std::move (drain)] { run (*native, drain); });
    }

    bool valid () const noexcept
    {
        std::lock_guard lock (_lifecycle_mutex);
        return _native && _native->timer.valid ();
    }

    void stop ()
    {
        std::lock_guard lock (_lifecycle_mutex);
        if (_native)
            _native->timer.stop ();
    }

    void close ()
    {
        std::thread worker;
        std::shared_ptr<native_resources_t> native;
        {
            std::lock_guard lock (_lifecycle_mutex);
            if (!_native)
                return;
            _stop.store (true, std::memory_order_release);
            // The drain callback may request stop, but cannot join itself.
            if (_worker.get_id () == std::this_thread::get_id ())
                return;
            worker = std::move (_worker);
            native = std::move (_native);
        }
        if (worker.joinable ())
            worker.join ();
        try {
            native->poller.close ();
            native->timer.close ();
        }
        catch (const std::exception &) {
            std::lock_guard lock (_lifecycle_mutex);
            _native = std::move (native);
            throw;
        }
    }

  private:
    struct native_resources_t
    {
        zlink::timer_t timer;
        zlink::poller_t poller;
    };

    void run (native_resources_t &native, const std::function<void (std::uint64_t)> &drain) noexcept
    {
        std::array<zlink::poll_event_t, 1> events{};
        while (!_stop.load (std::memory_order_acquire)) {
            try {
                if (native.poller.wait (events.data (), events.size (), poll_interval) == 0)
                    continue;
                const auto fire_count = native.timer.recv ();
                if (fire_count && drain)
                    drain (*fire_count);
            }
            catch (...) {
                _failures->report (std::current_exception ());
                break;
            }
        }
    }

    std::shared_ptr<native_resources_t> _native = std::make_shared<native_resources_t> ();
    std::shared_ptr<runtime::runtime_failure_collector_t> _failures;
    std::atomic_bool _stop{false};
    std::thread _worker;
    mutable std::mutex _lifecycle_mutex;
};

} // namespace zlink::framework::detail
