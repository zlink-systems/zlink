/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace zlink::framework
{
enum class stream_close_reason_t : std::uint8_t;
}
namespace zlink::framework::runtime
{
struct session_liveness_t
{
    std::mutex gate;
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
        const std::lock_guard<std::mutex> lock (gate);
        last_application_inbound = now;
    }

    void record_inbound (clock_t::time_point now = clock_t::now (), bool application = false)
    {
        const std::lock_guard<std::mutex> lock (gate);
        last_inbound = std::max (last_inbound, now);
        if (application)
            last_application_inbound = now;
    }

    decision_t evaluate (clock_t::time_point now = clock_t::now ())
    {
        const std::lock_guard<std::mutex> lock (gate);
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

    // The terminal close runs once even when sweeps race the reader exit.
    bool try_terminate (stream_close_reason_t reason)
    {
        const std::lock_guard<std::mutex> lock (gate);
        if (forced_reason) {
            return false;
        }
        forced_reason = reason;
        return true;
    }

    std::optional<stream_close_reason_t> forced ()
    {
        const std::lock_guard<std::mutex> lock (gate);
        return forced_reason;
    }
};

}
