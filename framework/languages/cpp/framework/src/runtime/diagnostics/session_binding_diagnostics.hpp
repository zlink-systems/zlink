/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/diagnostics/message_flow_tracer.hpp"
#include <zlink/Contracts/Core/routing_id.hpp>

namespace zlink::framework::detail
{

inline void trace_session_binding_control_drop (const dispatch_options_t &options,
                                                const std::vector<std::uint8_t> &source,
                                                const std::string &actor_id,
                                                const char *packet_name,
                                                message_flow_reason_t reason)
{
    message_flow_tracer_t (options).trace (message_flow_outcome_t::dropped, [&] {
        message_flow_event_t event{message_flow_outcome_t::dropped,
                                   dispatch_error_surface_t::stream_session,
                                   dispatch_message_kind_t::control, packet_name};
        event.source_rid = zlink::routing_id_t::from (source).to_hex ();
        event.actor_id = actor_id;
        event.reason = reason;
        return event;
    });
}

} // namespace zlink::framework::detail
