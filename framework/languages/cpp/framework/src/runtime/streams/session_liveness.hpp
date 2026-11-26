/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework/contracts/streams/stream.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace zlink::framework::runtime
{
struct session_liveness_t
{
    using clock_t = std::chrono::steady_clock;
    class reader_t
    {
      public:
        reader_t () noexcept : _resumed_at (clock_t::time_point::min ()) {}

        void observe_permit (bool acquired) noexcept
        {
            if (!acquired)
                _resumed_at.store (waiting, std::memory_order_relaxed);
            else if (_resumed_at.load (std::memory_order_relaxed) == waiting)
                _resumed_at.store (clock_t::now (), std::memory_order_relaxed);
        }

        clock_t::time_point inbound_due (clock_t::time_point last_activity,
                                         std::chrono::milliseconds timeout) const noexcept
        {
            if (timeout.count () <= 0)
                return clock_t::time_point::max ();
            const auto resumed_at = _resumed_at.load (std::memory_order_relaxed);
            return resumed_at == waiting ? clock_t::time_point::max ()
                                         : due_at (std::max (last_activity, resumed_at), timeout);
        }

      private:
        static constexpr auto waiting = clock_t::time_point::max ();
        // Only the reader writes. Core test-created sessions can read from the test thread.
        std::atomic<clock_t::time_point> _resumed_at;
    };

    clock_t::time_point last_application_inbound;
    clock_t::time_point last_ping_phase;
    clock_t::time_point last_inbound;
    const detail::stream_liveness_options_t options;

    explicit session_liveness_t (clock_t::time_point now = clock_t::now (),
                                 detail::stream_liveness_options_t settings = {}) :
        session_liveness_t (now, now, settings)
    {
    }
    session_liveness_t (clock_t::time_point established,
                        clock_t::time_point application_initial,
                        detail::stream_liveness_options_t settings = {}) :
        last_application_inbound (application_initial),
        last_ping_phase (established),
        last_inbound (established),
        options (settings)
    {
    }
    std::optional<stream_close_reason_t> forced_reason;

    enum class decision_t
    {
        none,
        send_heartbeat,
        idle_timeout,
        heartbeat_timeout
    };

    void record_application_inbound (clock_t::time_point now = clock_t::now ())
    {
        last_application_inbound = std::max (last_application_inbound, now);
    }

    void record_inbound (clock_t::time_point now = clock_t::now (), bool application = false)
    {
        last_inbound = std::max (last_inbound, now);
        if (application)
            last_application_inbound = std::max (last_application_inbound, now);
    }

    decision_t evaluate (clock_t::time_point now = clock_t::now (), const reader_t &reader = {})
    {
        if (forced_reason) {
            return decision_t::none;
        }
        const auto expired = [now] (clock_t::time_point due) {
            return due != clock_t::time_point::max () && now >= due;
        };
        if (expired (reader.inbound_due (last_inbound, options.heartbeat_timeout))) {
            return decision_t::heartbeat_timeout;
        }
        if (expired (reader.inbound_due (last_application_inbound, options.idle_timeout))) {
            return decision_t::idle_timeout;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds> (now - last_ping_phase)
            >= options.heartbeat_interval) {
            const auto interval =
              std::chrono::duration_cast<clock_t::duration> (options.heartbeat_interval);
            last_ping_phase = now - (now - last_ping_phase) % interval;
            return decision_t::send_heartbeat;
        }
        return decision_t::none;
    }

    clock_t::time_point next_due (const reader_t &reader = {}) const noexcept
    {
        if (forced_reason)
            return clock_t::time_point::max ();
        return std::min ({due_at (last_ping_phase, options.heartbeat_interval),
                          reader.inbound_due (last_inbound, options.heartbeat_timeout),
                          reader.inbound_due (last_application_inbound, options.idle_timeout)});
    }

    // The connection execution owner records facts and makes terminal decisions.
    bool try_terminate (stream_close_reason_t reason)
    {
        if (forced_reason) {
            return false;
        }
        forced_reason = reason;
        return true;
    }

    std::optional<stream_close_reason_t> forced () { return forced_reason; }

  private:
    static clock_t::time_point due_at (clock_t::time_point baseline,
                                       std::chrono::milliseconds timeout) noexcept
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
          clock_t::time_point::max () - baseline);
        return timeout >= remaining ? clock_t::time_point::max () : baseline + timeout;
    }
};

}
