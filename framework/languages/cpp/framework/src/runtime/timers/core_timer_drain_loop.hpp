/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Eventing/timers.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
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
    core_timer_drain_loop_t () = default;

    ~core_timer_drain_loop_t () noexcept
    {
        close ();
    }

    core_timer_drain_loop_t (const core_timer_drain_loop_t &) = delete;
    core_timer_drain_loop_t &operator= (const core_timer_drain_loop_t &) = delete;

    template <class Rep, class Period>
    void start (std::chrono::duration<Rep, Period> interval,
                std::uint64_t repeat_count,
                std::function<void (std::uint64_t)> drain)
    {
        std::lock_guard lock (_lifecycle_mutex);
        if (_worker.joinable ())
            return;
        _drain = std::move (drain);
        _poller.add (_timer, 1);
        _timer.start (interval, repeat_count);
        _stop.store (false, std::memory_order_release);
        _worker = std::thread ([this] { run (); });
    }

    bool valid () const noexcept
    {
        return _timer.valid ();
    }

    void stop ()
    {
        _timer.stop ();
    }

    void close () noexcept
    {
        std::thread worker;
        {
            std::lock_guard lock (_lifecycle_mutex);
            _stop.store (true, std::memory_order_release);
            if (_worker.joinable ())
                worker = std::move (_worker);
        }
        if (worker.joinable ())
            worker.join ();
        try {
            _poller.close ();
        }
        catch (...) {
        }
        try {
            _timer.close ();
        }
        catch (...) {
        }
        _drain = {};
    }

  private:
    void run () noexcept
    {
        std::array<zlink::poll_event_t, 1> events{};
        while (!_stop.load (std::memory_order_acquire)) {
            try {
                if (_poller.wait (events.data (), events.size (),
                                  std::chrono::milliseconds (50)) == 0)
                    continue;
                const auto fire_count = _timer.recv ();
                if (fire_count && _drain)
                    _drain (*fire_count);
            }
            catch (...) {
                break;
            }
        }
    }

    zlink::timer_t _timer;
    zlink::poller_t _poller;
    std::function<void (std::uint64_t)> _drain;
    std::atomic_bool _stop{false};
    std::thread _worker;
    std::mutex _lifecycle_mutex;
};

} // namespace zlink::framework::detail
