/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/streams/stream.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace zlink::framework::runtime
{
struct session_liveness_t
{
    using clock_t = std::chrono::steady_clock;
    clock_t::time_point last_application_inbound;
    clock_t::time_point last_ping;
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
        last_ping (established),
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

    decision_t evaluate (clock_t::time_point now = clock_t::now ())
    {
        if (forced_reason) {
            return decision_t::none;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds> (now - last_inbound)
            >= options.heartbeat_timeout) {
            return decision_t::heartbeat_timeout;
        }
        if (options.idle_timeout.count () > 0
            && std::chrono::duration_cast<std::chrono::milliseconds> (now
                                                                      - last_application_inbound)
                 >= options.idle_timeout) {
            return decision_t::idle_timeout;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds> (now - last_ping)
            >= options.heartbeat_interval) {
            last_ping = now;
            return decision_t::send_heartbeat;
        }
        return decision_t::none;
    }

    clock_t::time_point next_due () const noexcept
    {
        if (forced_reason)
            return clock_t::time_point::max ();
        const auto due_at = [] (clock_t::time_point baseline, std::chrono::milliseconds timeout) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
              clock_t::time_point::max () - baseline);
            return timeout >= remaining ? clock_t::time_point::max () : baseline + timeout;
        };
        const auto heartbeat_due = std::min (due_at (last_ping, options.heartbeat_interval),
                                             due_at (last_inbound, options.heartbeat_timeout));
        return options.idle_timeout.count () > 0
                 ? std::min (heartbeat_due, due_at (last_application_inbound, options.idle_timeout))
                 : heartbeat_due;
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
};

}
