/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Eventing/timers.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>

namespace zlink::framework::runtime::eventing
{

/*
 * A runtime-owned poller must be interruptible when another thread publishes
 * work or requests shutdown. The generic zlink timer is backed by the core
 * signaler, so this source does not expose an operating-system descriptor or
 * require platform-specific branches in the framework.
 */
class runtime_wake_timer_t final
{
  public:
    static constexpr std::uintptr_t slot =
      std::numeric_limits<std::uintptr_t>::max ();

    runtime_wake_timer_t () = default;
    ~runtime_wake_timer_t () noexcept { detach (); }

    runtime_wake_timer_t (const runtime_wake_timer_t &) = delete;
    runtime_wake_timer_t &operator= (const runtime_wake_timer_t &) = delete;

    void attach (zlink::poller_t &poller)
    {
        std::lock_guard lock (_mutex);
        if (_poller)
            return;
        poller.add (_timer, slot);
        _poller = &poller;
    }

    void signal () noexcept
    {
        std::lock_guard lock (_mutex);
        if (!_poller)
            return;
        try {
            /* A one-shot timer gives the poller a bounded, cross-platform
             * wake event while coalescing concurrent notifications. */
            _timer.start (std::chrono::milliseconds (1), 1);
        }
        catch (...) {
        }
    }

    bool is_event (const zlink::poll_event_t &event) const noexcept
    {
        return event.source_kind == zlink::poll_source_kind_t::timer
               && event.slot == slot;
    }

    void consume () noexcept
    {
        std::lock_guard lock (_mutex);
        if (!_poller)
            return;
        try {
            (void) _timer.recv ();
        }
        catch (...) {
        }
    }

    void detach () noexcept
    {
        std::lock_guard lock (_mutex);
        if (!_poller)
            return;
        try {
            _timer.stop ();
        }
        catch (...) {
        }
        try {
            (void) _poller->remove (_timer);
        }
        catch (...) {
        }
        _poller = nullptr;
    }

  private:
    zlink::timer_t _timer;
    zlink::poller_t *_poller = nullptr;
    std::mutex _mutex;
};

} // namespace zlink::framework::runtime::eventing
