/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/foundation/operation_registry.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <zlink/framework/contracts/errors/error.hpp>

namespace zlink::framework::runtime::user_spot_terminal
{

inline framework_error_kind_t map_user_spot_wire_failure (
  const protocol::reply_header_t &header,
  bool creation)
{
    if (header.terminal_result == 101)
        return framework_error_kind_t::deadline_exceeded;
    if (header.terminal_result == 109)
        return framework_error_kind_t::unavailable;
    if (creation
        && (header.terminal_result == 108
            || header.terminal_result == 113)
        && header.failure_code == 0)
        return framework_error_kind_t::capacity_exceeded;

    switch (
      static_cast<protocol::framework_error_code> (
        header.failure_code)) {
        case protocol::framework_error_code::spotCreateFailed:
            return framework_error_kind_t::internal_failure;
        case protocol::framework_error_code::spotRouteNotFound:
            return framework_error_kind_t::not_found;
        case protocol::framework_error_code::spotTypeMismatch:
            return framework_error_kind_t::type_mismatch;
        case protocol::framework_error_code::requestRejected:
            return framework_error_kind_t::rejected;
        case protocol::framework_error_code::requestProtocolError:
            return framework_error_kind_t::protocol_error;
        case protocol::framework_error_code::requestFailed:
            return framework_error_kind_t::internal_failure;
        case protocol::framework_error_code::workerQueueFull:
            return creation
                     ? framework_error_kind_t::capacity_exceeded
                     : framework_error_kind_t::capacity_exceeded;
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

    if (header.terminal_result == 103
        || header.terminal_result == 106)
        return framework_error_kind_t::rejected;
    return framework_error_kind_t::internal_failure;
}

inline framework_error_kind_t map_user_spot_operation_failure (
  foundation::operation_terminal_t terminal,
  const protocol::reply_header_t &header,
  bool creation)
{
    switch (terminal) {
        case foundation::operation_terminal_t::completed:
            return map_user_spot_wire_failure (
              header, creation);
        case foundation::operation_terminal_t::timed_out:
            return framework_error_kind_t::deadline_exceeded;
        case foundation::operation_terminal_t::transport_failed:
            return framework_error_kind_t::unavailable;
        case foundation::operation_terminal_t::cancelled:
            return framework_error_kind_t::invalid_operation;
        case foundation::operation_terminal_t::shutdown:
            return framework_error_kind_t::shutting_down;
    }
    return framework_error_kind_t::internal_failure;
}

} // namespace zlink::framework::runtime::user_spot_terminal
