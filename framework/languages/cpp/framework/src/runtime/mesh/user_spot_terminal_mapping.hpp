/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/foundation/operation_registry.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <zlink/framework/contracts/errors/error.hpp>

namespace zlink::framework::runtime::user_spot_terminal
{

inline framework_error_kind_t map_user_spot_wire_failure (const protocol::reply_header_t &header,
                                                          bool creation)
{
    // No eligible node can host the Spot; waiting on a queue cannot make a
    // placement target appear.
    if (creation
        && header.terminal_result
             == static_cast<std::uint32_t> (protocol::request_terminal_result::backpressured)
        && header.failure_code == static_cast<std::uint32_t> (protocol::framework_error_code::none))
        return framework_error_kind_t::unavailable;
    switch (static_cast<protocol::framework_error_code> (header.failure_code)) {
        case protocol::framework_error_code::spotCreateFailed:
            return framework_error_kind_t::internal_failure;
        case protocol::framework_error_code::spotRouteNotFound:
            return framework_error_kind_t::not_found;
        case protocol::framework_error_code::spotTypeMismatch:
            return framework_error_kind_t::type_mismatch;
        case protocol::framework_error_code::workerQueueFull:
            // Legacy peers may still report workerQueueFull for an unavailable
            // remote target.
            return framework_error_kind_t::unavailable;
        case protocol::framework_error_code::workerTimedOut:
            return framework_error_kind_t::deadline_exceeded;
        case protocol::framework_error_code::workerFailed:
            return framework_error_kind_t::internal_failure;
        case protocol::framework_error_code::spotGenerationStale:
            return framework_error_kind_t::invalid_operation;
        case protocol::framework_error_code::spotMoving:
            return framework_error_kind_t::unavailable;
        case protocol::framework_error_code::relocationDataLost:
            return framework_error_kind_t::data_lost;
        default:
            break;
    }

    return messaging::request_failure_mapper_t{}
      .reply_header_exception (header.terminal_result, header.failure_code, "User Spot request")
      .kind ();
}

inline framework_error_kind_t map_user_spot_operation_failure (
  foundation::operation_terminal_t terminal, const protocol::reply_header_t &header, bool creation)
{
    switch (terminal) {
        case foundation::operation_terminal_t::completed:
            return map_user_spot_wire_failure (header, creation);
        case foundation::operation_terminal_t::timed_out:
            return framework_error_kind_t::deadline_exceeded;
        case foundation::operation_terminal_t::protocol_error:
            return framework_error_kind_t::protocol_error;
        case foundation::operation_terminal_t::transport_failed:
        case foundation::operation_terminal_t::route_unavailable:
            return framework_error_kind_t::unavailable;
        case foundation::operation_terminal_t::cancelled:
            return framework_error_kind_t::invalid_operation;
        case foundation::operation_terminal_t::shutdown:
            return framework_error_kind_t::shutting_down;
    }
    return framework_error_kind_t::internal_failure;
}

} // namespace zlink::framework::runtime::user_spot_terminal
