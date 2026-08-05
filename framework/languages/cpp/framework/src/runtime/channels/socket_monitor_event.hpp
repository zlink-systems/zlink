/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/diagnostics/monitoring_runtime.hpp"

#include <zlink/Contracts/Eventing/events.hpp>

#include <optional>

namespace zlink::framework::detail
{

inline std::optional<socket_event_kind_t> map_socket_monitor_event (zlink::monitor_event event)
{
    switch (event) {
        case zlink::monitor_event::connected:
        case zlink::monitor_event::accepted:
            return socket_event_kind_t::connected;
        case zlink::monitor_event::connection_ready:
            return socket_event_kind_t::connection_ready;
        case zlink::monitor_event::disconnected:
            return socket_event_kind_t::disconnected;
        case zlink::monitor_event::closed:
            return socket_event_kind_t::closed;
        case zlink::monitor_event::handshake_failed_no_detail:
        case zlink::monitor_event::handshake_failed_protocol:
        case zlink::monitor_event::handshake_failed_auth:
            return socket_event_kind_t::handshake_failed;
        default:
            return std::nullopt;
    }
}

} // namespace zlink::framework::detail
