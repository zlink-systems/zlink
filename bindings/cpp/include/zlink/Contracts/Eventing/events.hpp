/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/routing_id.hpp"

namespace zlink
{

/// @brief Connection lifecycle events that a monitor can be subscribed to; combine as flags.
enum class monitor_event : uint32_t
{
    connected = 1,
    connect_delayed = 2,
    connect_retried = 4,
    listening = 8,
    bind_failed = 16,
    accepted = 32,
    accept_failed = 64,
    closed = 128,
    close_failed = 256,
    disconnected = 512,
    monitor_stopped = 1024,
    handshake_failed_no_detail = 2048,
    connection_ready = 4096,
    peer_weight_changed = 32768,
    handshake_failed_protocol = 8192,
    handshake_failed_auth = 16384,
    all = 65535
};

inline monitor_event operator| (monitor_event a, monitor_event b)
{
    return static_cast<monitor_event> (static_cast<uint32_t> (a) | static_cast<uint32_t> (b));
}

/// @brief A single socket connection-lifecycle event reported by a monitor.
struct monitor_event_t
{
    monitor_event_t () :
        event (monitor_event::closed),
        value (0),
        routing_id (std::nullopt),
        local_addr (),
        remote_addr ()
    {
    }

    monitor_event event;
    uint32_t value;
    std::optional<routing_id_t> routing_id;
    std::string local_addr;
    std::string remote_addr;
};

/// @brief Identifies what kind of source a monitor is attached to.
enum class monitor_target_kind_t : int
{
    socket = 0,
    discovery = 1,
    spot = 2
};

using monitor_event_type_t = monitor_event;

} // namespace zlink
