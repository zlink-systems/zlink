/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/routing_id.hpp"

#include <cstdint>

namespace zlink
{

/// @brief Identifies what a monitored source is.
enum class monitor_source_kind : int
{
    socket = 1,   ///< A plain socket.
    spot_pub = 3, ///< The publish side of a spot.
    spot_sub = 4  ///< The subscribe side of a spot.
};

/// @brief Bitmask of a monitored socket's readiness states.
enum class monitor_state : uint32_t
{
    ready = 1,
    bound_ready = 2,
    closed = 8
};

inline monitor_state operator| (monitor_state a, monitor_state b)
{
    return static_cast<monitor_state> (static_cast<uint32_t> (a) | static_cast<uint32_t> (b));
}

enum class monitor_status_detail : uint32_t
{
    snd_pending_msgs = 2,
    rcv_pending_msgs = 4
};

inline monitor_status_detail operator| (monitor_status_detail a, monitor_status_detail b)
{
    return static_cast<monitor_status_detail> (static_cast<uint32_t> (a)
                                               | static_cast<uint32_t> (b));
}

/// @brief Reason a connection was disconnected.
enum class disconnect_reason : int
{
    unknown = 0,
    handshake_failed = 3,
    transport_error = 4,
    ctx_term = 5
};

/// @brief A snapshot of a monitored socket's state and auto-HWM telemetry.
struct monitor_status_t
{
    monitor_status_t () :
        abi_version (0),
        struct_size (0),
        source_kind (monitor_source_kind::socket),
        state_flags (0),
        detail_flags (0),
        snd_pending_msgs (0),
        rcv_pending_msgs (0),
        auto_hwm_enabled (false),
        auto_hwm_profile (0),
        auto_hwm_role (0),
        auto_hwm_policy_class (0),
        auto_hwm_unit_budget_bytes (0),
        auto_hwm_size_cap (0),
        auto_hwm_socket_message_slots (0),
        auto_hwm_connection_bucket_enabled (false),
        auto_hwm_connection_bucket_count (0),
        auto_hwm_connection_bucket_index (0xffffffffu),
        auto_hwm_connection_bucket_hwm_4k (0),
        auto_hwm_connection_bucket_hysteresis_retained (false),
        auto_hwm_effective_message_bytes (0),
        auto_hwm_planned_sndhwm_bytes (0),
        auto_hwm_planned_rcvhwm_bytes (0),
        auto_hwm_applied_sndhwm_bytes (0),
        auto_hwm_applied_rcvhwm_bytes (0),
        auto_hwm_effective_sndbuf (0),
        auto_hwm_effective_rcvbuf (0),
        auto_hwm_last_recalc_ms (0),
        auto_hwm_last_recalc_reason (0),
        auto_hwm_send_blocked_ratio_ppm (0),
        auto_hwm_deferred_sndhwm_bytes (0),
        auto_hwm_deferred_rcvhwm_bytes (0),
        auto_hwm_deferred_sndhwm_valid (false),
        auto_hwm_deferred_rcvhwm_valid (false),
        snd_bytes_in_flight (0),
        rcv_bytes_in_flight (0),
        minimum_core_message_charge_bytes (0),
        oversize_message_admission_count (0),
        oversize_message_admission_max_bytes (0)
    {
    }

    bool is_ready () const noexcept { return (state_flags & 1) != 0u; }

    uint32_t abi_version;
    uint32_t struct_size;
    monitor_source_kind source_kind;
    uint32_t state_flags;
    uint32_t detail_flags;
    uint64_t snd_pending_msgs;
    uint64_t rcv_pending_msgs;
    bool auto_hwm_enabled;
    uint32_t auto_hwm_profile;
    uint32_t auto_hwm_role;
    uint32_t auto_hwm_policy_class;
    uint64_t auto_hwm_unit_budget_bytes;
    uint32_t auto_hwm_size_cap;
    uint64_t auto_hwm_socket_message_slots;
    bool auto_hwm_connection_bucket_enabled;
    uint32_t auto_hwm_connection_bucket_count;
    uint32_t auto_hwm_connection_bucket_index;
    uint32_t auto_hwm_connection_bucket_hwm_4k;
    bool auto_hwm_connection_bucket_hysteresis_retained;
    uint64_t auto_hwm_effective_message_bytes;
    uint64_t auto_hwm_planned_sndhwm_bytes;
    uint64_t auto_hwm_planned_rcvhwm_bytes;
    uint64_t auto_hwm_applied_sndhwm_bytes;
    uint64_t auto_hwm_applied_rcvhwm_bytes;
    int32_t auto_hwm_effective_sndbuf;
    int32_t auto_hwm_effective_rcvbuf;
    uint64_t auto_hwm_last_recalc_ms;
    uint32_t auto_hwm_last_recalc_reason;
    uint32_t auto_hwm_send_blocked_ratio_ppm;
    uint64_t auto_hwm_deferred_sndhwm_bytes;
    uint64_t auto_hwm_deferred_rcvhwm_bytes;
    bool auto_hwm_deferred_sndhwm_valid;
    bool auto_hwm_deferred_rcvhwm_valid;
    uint64_t snd_bytes_in_flight;
    uint64_t rcv_bytes_in_flight;
    uint64_t minimum_core_message_charge_bytes;
    uint64_t oversize_message_admission_count;
    uint64_t oversize_message_admission_max_bytes;
};

using monitor_source_kind_t = monitor_source_kind;

} // namespace zlink
