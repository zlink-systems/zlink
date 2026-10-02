/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace zlink::framework
{
enum class stream_close_reason_t : std::uint8_t;
}
namespace zlink::framework::runtime
{
struct session_liveness_t
{
    using clock_t = std::chrono::steady_clock;
    clock_t::time_point last_application_inbound;
    clock_t::time_point last_ping;
    clock_t::time_point last_inbound;
    static constexpr auto heartbeat_interval = std::chrono::seconds (1);
    static constexpr auto heartbeat_timeout = std::chrono::seconds (5);
    static constexpr auto application_idle_timeout = std::chrono::seconds (30);

    explicit session_liveness_t (clock_t::time_point now = clock_t::now ()) :
        session_liveness_t (now, now)
    {
    }
    session_liveness_t (clock_t::time_point established, clock_t::time_point application_initial) :
        last_application_inbound (application_initial),
        last_ping (established),
        last_inbound (established)
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
        if (now - last_inbound >= heartbeat_timeout) {
            return decision_t::heartbeat_timeout;
        }
        if (now - last_application_inbound >= application_idle_timeout) {
            return decision_t::idle_timeout;
        }
        if (now - last_ping >= heartbeat_interval) {
            last_ping = now;
            return decision_t::send_heartbeat;
        }
        return decision_t::none;
    }

    clock_t::time_point next_due () const noexcept
    {
        if (forced_reason)
            return clock_t::time_point::max ();
        return std::min ({last_ping + heartbeat_interval, last_inbound + heartbeat_timeout,
                          last_application_inbound + application_idle_timeout});
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
