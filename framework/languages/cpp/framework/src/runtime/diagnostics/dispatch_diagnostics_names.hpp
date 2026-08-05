/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/execution.hpp>

#include <string_view>

// Human-readable names for the dispatch diagnostics enums. Shared by the error
// reporter and the message-flow tracer so the two emit identical surface/kind
// tokens and a single log stream can be filtered with one set of vocabulary.
namespace zlink::framework::detail
{

inline std::string_view enum_name (dispatch_error_surface_t value) noexcept
{
    switch (value) {
        case dispatch_error_surface_t::channel:
            return "channel";
        case dispatch_error_surface_t::route_mesh_channel:
            return "route_mesh_channel";
        case dispatch_error_surface_t::spot_route:
            return "spot_route";
        case dispatch_error_surface_t::spot_subscription:
            return "spot_subscription";
        case dispatch_error_surface_t::spot_actor:
            return "spot_actor";
        case dispatch_error_surface_t::stream_session:
            return "stream_session";
    }
    return "unknown";
}

inline std::string_view enum_name (dispatch_message_kind_t value) noexcept
{
    switch (value) {
        case dispatch_message_kind_t::send:
            return "send";
        case dispatch_message_kind_t::request:
            return "request";
        case dispatch_message_kind_t::publish:
            return "publish";
        case dispatch_message_kind_t::response:
            return "response";
        case dispatch_message_kind_t::error:
            return "error";
        case dispatch_message_kind_t::actor_send:
            return "actor_send";
        case dispatch_message_kind_t::actor_request:
            return "actor_request";
    }
    return "unknown";
}

inline std::string_view enum_name (dispatch_error_reason_t value) noexcept
{
    switch (value) {
        case dispatch_error_reason_t::handler_missing:
            return "handler_missing";
        case dispatch_error_reason_t::payload_decode_failed:
            return "payload_decode_failed";
        case dispatch_error_reason_t::handler_exception:
            return "handler_exception";
        case dispatch_error_reason_t::invalid_frame:
            return "invalid_frame";
        case dispatch_error_reason_t::reply_path_missing:
            return "reply_path_missing";
        case dispatch_error_reason_t::unexpected_reply:
            return "unexpected_reply";
    }
    return "unknown";
}

inline std::string_view enum_name (dispatch_error_action_t value) noexcept
{
    switch (value) {
        case dispatch_error_action_t::drop:
            return "drop";
        case dispatch_error_action_t::reply_error:
            return "reply_error";
        case dispatch_error_action_t::fail_caller:
            return "fail_caller";
    }
    return "unknown";
}

inline std::string_view enum_name (message_flow_outcome_t value) noexcept
{
    switch (value) {
        case message_flow_outcome_t::received:
            return "received";
        case message_flow_outcome_t::dispatched:
            return "dispatched";
        case message_flow_outcome_t::replied:
            return "replied";
        case message_flow_outcome_t::dropped:
            return "dropped";
        case message_flow_outcome_t::sent:
            return "sent";
        case message_flow_outcome_t::reply_received:
            return "reply_received";
        case message_flow_outcome_t::error:
            return "error";
    }
    return "unknown";
}

} // namespace zlink::framework::detail
