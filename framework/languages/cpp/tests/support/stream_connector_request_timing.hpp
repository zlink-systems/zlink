/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/connector_runtime.hpp"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>

namespace stream_connector_fixture
{
class scripted_request_timing_t final : public zlink::stream_connector::detail::request_timing_t
{
  public:
    using clock_t = zlink::stream_connector::detail::request_clock_t;
    using elapsed_t = std::chrono::milliseconds;

    clock_t::time_point now () const override { return clock_t::time_point{} + _elapsed.load (); }

    elapsed_t admission_jitter () override
    {
        if (_jitter.empty ()) {
            throw std::runtime_error ("runtime requested unexpected admission jitter");
        }
        const auto delay = _jitter.front ();
        _jitter.pop_front ();
        return delay;
    }

    std::function<void ()> schedule (clock_t::time_point when,
                                     std::function<void ()> operation) override
    {
        std::lock_guard lock (_mutex);
        const auto key = std::make_pair (when, _next_timer++);
        _timers.emplace (key, std::move (operation));
        return [this, key] {
            std::lock_guard cancel_lock (_mutex);
            _timers.erase (key);
        };
    }

    void advance (elapsed_t elapsed)
    {
        if (elapsed < _elapsed.load ()) {
            throw std::runtime_error ("test clock moved backwards");
        }
        _elapsed = elapsed;
        for (;;) {
            std::function<void ()> due;
            {
                std::lock_guard lock (_mutex);
                const auto first = _timers.begin ();
                if (first == _timers.end () || first->first.first > now ()) {
                    return;
                }
                due = std::move (first->second);
                _timers.erase (first);
            }
            due ();
        }
    }

    void expect_jitter (elapsed_t delay) { _jitter.push_back (delay); }
    bool jitter_consumed () const { return _jitter.empty (); }

  private:
    std::atomic<elapsed_t> _elapsed{elapsed_t::zero ()};
    std::deque<elapsed_t> _jitter;
    std::mutex _mutex;
    std::map<std::pair<clock_t::time_point, std::size_t>, std::function<void ()>> _timers;
    std::size_t _next_timer = 0;
};

} // namespace stream_connector_fixture
