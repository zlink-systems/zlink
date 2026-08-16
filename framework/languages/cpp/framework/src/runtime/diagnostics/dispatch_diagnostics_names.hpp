/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/execution.hpp>

#include "runtime/diagnostics/dispatch_events.hpp"

#include <string_view>

// Human-readable names for the dispatch diagnostics enums. Shared by the error
// reporter and the message-flow tracer so the two emit identical surface/kind
// tokens and a single log stream can be filtered with one set of vocabulary.
namespace zlink::framework::detail
{

inline std::string_view enum_name (flow_origin_t value) noexcept
{
    switch (value) {
        case flow_origin_t::inbound:
            return "inbound";
        case flow_origin_t::timer:
            return "timer";
        case flow_origin_t::application:
            return "application";
        case flow_origin_t::lifecycle:
            return "lifecycle";
    }
    return "unknown";
}

inline std::string_view enum_name (dispatch_error_surface_t value) noexcept
{
    switch (value) {
        case dispatch_error_surface_t::channel:
            return "channel";
        case dispatch_error_surface_t::route_mesh_channel:
            return "channel";
        case dispatch_error_surface_t::spot_route:
            return "spot";
        case dispatch_error_surface_t::spot_subscription:
            return "spot";
        case dispatch_error_surface_t::spot_actor:
            return "actor";
        case dispatch_error_surface_t::stream_session:
            return "stream";
        case dispatch_error_surface_t::node:
            return "node";
        case dispatch_error_surface_t::instance_spot:
            return "instance_spot";
        case dispatch_error_surface_t::actor_relocation:
            return "actor_relocation";
        case dispatch_error_surface_t::classic_fanout:
            return "classic_fanout";
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
            return "send";
        case dispatch_message_kind_t::response:
            return "response";
        case dispatch_message_kind_t::error:
            return "error";
        case dispatch_message_kind_t::actor_send:
            return "send";
        case dispatch_message_kind_t::actor_request:
            return "request";
        case dispatch_message_kind_t::control:
            return "control";
    }
    return "unknown";
}

inline std::string_view enum_name (dispatch_error_reason_t value) noexcept
{
    switch (value) {
        case dispatch_error_reason_t::handler_missing:
            return "no_handler";
        case dispatch_error_reason_t::payload_decode_failed:
            return "decode_error";
        case dispatch_error_reason_t::handler_exception:
            return "handler_exception";
        case dispatch_error_reason_t::invalid_frame:
            return "invalid_frame";
        case dispatch_error_reason_t::reply_path_missing:
            return "reply_path_missing";
        case dispatch_error_reason_t::unexpected_reply:
            return "unexpected_reply";
        case dispatch_error_reason_t::backpressure:
            return "backpressure";
        case dispatch_error_reason_t::stale_target:
            return "stale_target";
        case dispatch_error_reason_t::shutdown:
            return "shutdown";
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
        case message_flow_outcome_t::admitted:
            return "admitted";
        case message_flow_outcome_t::completed:
            return "completed";
        case message_flow_outcome_t::backpressured:
            return "backpressured";
    }
    return "unknown";
}

inline std::string_view enum_name (message_flow_result_t value) noexcept
{
    switch (value) {
        case message_flow_result_t::succeeded:
            return "succeeded";
        case message_flow_result_t::failed:
            return "failed";
        case message_flow_result_t::backpressured:
            return "backpressured";
        case message_flow_result_t::dropped:
            return "dropped";
        case message_flow_result_t::cancelled:
            return "cancelled";
        case message_flow_result_t::shutdown:
            return "shutdown";
    }
    return "unknown";
}

inline std::string_view enum_name (message_flow_reason_t value) noexcept
{
    switch (value) {
        case message_flow_reason_t::backpressure:
            return "backpressure";
        case message_flow_reason_t::stale_target:
            return "stale_target";
        case message_flow_reason_t::target_closed:
            return "target_closed";
        case message_flow_reason_t::shutdown:
            return "shutdown";
        case message_flow_reason_t::location_unavailable:
            return "location_unavailable";
        case message_flow_reason_t::activation_rejected:
            return "activation_rejected";
        case message_flow_reason_t::activation_timeout:
            return "activation_timeout";
    }
    return "unknown";
}

} // namespace zlink::framework::detail
