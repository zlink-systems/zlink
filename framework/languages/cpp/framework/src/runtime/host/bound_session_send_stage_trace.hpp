/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/backend/raw_route_port.hpp"

#include <string>
#include <string_view>

namespace zlink::framework::detail
{

struct bound_session_send_stage_trace_context_t
{
    actor_gateway_runtime_t *gateway;
    std::string_view actor_id;
    const zlink::routing_id_t *session_rid;
};

inline backend::raw_send_stage_trace_t
make_bound_session_send_stage_trace (const bound_session_send_stage_trace_context_t &context)
{
    if (!context.gateway->trace_bound_session_send_stage_enabled ())
        return {};

    return [gateway = *context.gateway, actor_id = std::string (context.actor_id),
            session_rid = *context.session_rid] (std::string_view stage, std::string_view result) {
        gateway.trace_bound_session_send_stage (
          actor_id, stage, [&] { return std::string (result); }, &session_rid);
    };
}

} // namespace zlink::framework::detail
