/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/routing_id.hpp"

#include <cstdint>

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
    /// Fires once when a remote PAUSE is first applied to an application pipe
    /// (core-byte-hwm-flow-control-plan.ko.md §6). `value` carries the flow epoch.
    send_flow_paused = 65536,
    /// Fires once when a remote RUNNING clears the remote-pause cause for a
    /// pipe. `value` carries the flow epoch; check `monitor_event_flag_t::send_flow_writable`
    /// in the event's `flags` for whether the pipe is actually writable now.
    send_flow_resumed = 131072,
    /// Fires when a stale or duplicate completion-lane flow-state frame is
    /// rejected. `flags` disambiguates the stale reason and `value` carries
    /// the corresponding rejected field.
    flow_state_stale = 262144,
    all = 0x7FFFF
};

inline monitor_event operator| (monitor_event a, monitor_event b)
{
    return static_cast<monitor_event> (static_cast<uint32_t> (a) | static_cast<uint32_t> (b));
}

/// @brief Bits that can appear in @c monitor_event_t::flags. Combine as flags.
enum class monitor_event_flag_t : uint32_t
{
    none = 0,
    /// Set on a CONNECTION_READY event that moves a connection from
    /// not-ready to ready.
    connection_ready_edge = 1u << 0,
    /// Set on `send_flow_resumed` when clearing the remote pause left the
    /// pipe actually writable; clear when another cause (byte HWM, transport
    /// wait or termination) still blocks it.
    send_flow_writable = 1u << 1,
    /// Set on `flow_state_stale` when the epoch did not advance inside the
    /// current connection. `value` then carries the received epoch.
    flow_state_stale_epoch = 1u << 3
};

inline monitor_event_flag_t operator| (monitor_event_flag_t a, monitor_event_flag_t b)
{
    return static_cast<monitor_event_flag_t> (static_cast<uint32_t> (a)
                                              | static_cast<uint32_t> (b));
}

inline bool has_flag (uint32_t flags_, monitor_event_flag_t flag_)
{
    return (flags_ & static_cast<uint32_t> (flag_)) != 0;
}

/// @brief A single socket connection-lifecycle event reported by a monitor.
struct monitor_event_t
{
    monitor_event_t () :
        event (monitor_event::closed),
        value (0),
        connection_id (0),
        transport_lane (0),
        flags (0),
        routing_id (std::nullopt),
        local_addr (),
        remote_addr ()
    {
    }

    monitor_event event;
    std::uint64_t value;
    /* Identifies the physical transport attempt that emitted the event. */
    std::uint64_t connection_id;
    std::uint32_t transport_lane;
    std::uint32_t flags;
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
