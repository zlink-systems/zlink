/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/messaging/request_failure_mapper.hpp"

namespace zlink::framework::runtime::messaging
{

framework_exception_t
request_failure_mapper_t::completion_exception (request_result_t result,
                                                const std::string &operation_name) const
{
    switch (result) {
        case request_result_t::timed_out:
            return detail::make_boundary_exception (detail::boundary_error_t::timed_out,
                                          operation_name + " timed out.");
        case request_result_t::not_connected:
            return detail::make_boundary_exception (
              detail::boundary_error_t::disconnected,
              operation_name + " failed because the target route is not connected.");
        case request_result_t::not_found:
            return framework_exception_t (framework_error_kind_t::not_found,
                                          operation_name
                                            + " failed because the target was not found.");
        case request_result_t::rejected:
            return framework_exception_t (framework_error_kind_t::rejected,
                                          operation_name + " was rejected.");
        case request_result_t::conflict:
        case request_result_t::busy:
            return framework_exception_t (framework_error_kind_t::capacity_exceeded,
                                          operation_name + " exceeded capacity.");
        case request_result_t::protocol_error:
            return framework_exception_t (framework_error_kind_t::protocol_error,
                                          operation_name + " failed with a protocol error.");
        case request_result_t::invalid_argument:
        case request_result_t::invalid_state:
            return framework_exception_t (framework_error_kind_t::invalid_operation,
                                          operation_name + " is invalid in the current state.");
        case request_result_t::not_supported:
        case request_result_t::internal_error:
            return framework_exception_t (framework_error_kind_t::internal_failure,
                                          operation_name + " failed.");
        case request_result_t::terminated:
            return detail::make_boundary_exception (
              detail::boundary_error_t::shutdown,
              operation_name + " stopped because the runtime is shutting down.");
    }
    return framework_exception_t (framework_error_kind_t::internal_failure,
                                  operation_name + " failed.");
}

framework_exception_t
request_failure_mapper_t::error_header_exception (const std::string &error_code,
                                                  const std::string &error_message,
                                                  const std::string &operation_name) const
{
    if (error_code == "timeout") {
        return detail::make_boundary_exception (detail::boundary_error_t::timed_out,
                                      error_message.empty () ? operation_name + " timed out."
                                                             : error_message);
    }
    if (error_code == "not_found") {
        return framework_exception_t (
          framework_error_kind_t::not_found,
          error_message.empty () ? operation_name + " failed because the target was not found."
                                 : error_message);
    }
    if (error_code == "already_exists") {
        return framework_exception_t (
          framework_error_kind_t::already_exists,
          error_message.empty () ? operation_name + " failed because the object already exists."
                                 : error_message);
    }
    if (error_code == "type_mismatch") {
        return framework_exception_t (
          framework_error_kind_t::type_mismatch,
          error_message.empty () ? operation_name + " failed because the type does not match."
                                 : error_message);
    }
    if (error_code == "not_configured") {
        return framework_exception_t (
          framework_error_kind_t::not_configured,
          error_message.empty () ? operation_name + " is not configured." : error_message);
    }
    if (error_code == "rejected") {
        return framework_exception_t (
          framework_error_kind_t::rejected,
          error_message.empty () ? operation_name + " was rejected." : error_message);
    }
    if (error_code == "unavailable") {
        return detail::make_boundary_exception (
          detail::boundary_error_t::disconnected,
          error_message.empty () ? operation_name + " is unavailable." : error_message);
    }
    if (error_code == "not_connected") {
        return detail::make_boundary_exception (
          detail::boundary_error_t::disconnected,
          error_message.empty ()
            ? operation_name + " failed because the target route is not connected."
            : error_message);
    }
    if (error_code == "capacity_exceeded") {
        return framework_exception_t (
          framework_error_kind_t::capacity_exceeded,
          error_message.empty () ? operation_name + " exceeded capacity." : error_message);
    }
    if (error_code == "deadline_exceeded") {
        return detail::make_boundary_exception (
          detail::boundary_error_t::timed_out,
          error_message.empty () ? operation_name + " exceeded its deadline." : error_message);
    }
    if (error_code == "shutting_down") {
        return detail::make_boundary_exception (
          detail::boundary_error_t::shutdown,
          error_message.empty () ? operation_name + " stopped because the runtime is shutting down."
                                 : error_message);
    }
    if (error_code == "protocol_error") {
        return framework_exception_t (
          framework_error_kind_t::protocol_error,
          error_message.empty () ? operation_name + " failed with a protocol error."
                                 : error_message);
    }
    if (error_code == "invalid_operation") {
        return framework_exception_t (
          framework_error_kind_t::invalid_operation,
          error_message.empty () ? operation_name + " is invalid in the current state."
                                 : error_message);
    }
    if (error_code == "data_lost") {
        return framework_exception_t (
          framework_error_kind_t::data_lost,
          error_message.empty () ? operation_name + " failed because required data was lost."
                                 : error_message);
    }
    if (error_code == "internal_failure") {
        return framework_exception_t (
          framework_error_kind_t::internal_failure,
          error_message.empty () ? operation_name + " failed." : error_message);
    }
    if (error_code == "route_not_connected") {
        return detail::make_boundary_exception (
          detail::boundary_error_t::disconnected,
          error_message.empty ()
            ? operation_name + " failed because the target route is not connected."
            : error_message);
    }
    if (error_code == "request_target_not_found") {
        return framework_exception_t (
          framework_error_kind_t::not_found,
          error_message.empty () ? operation_name + " failed because the target was not found."
                                 : error_message);
    }
    if (error_code == "request_rejected") {
        return framework_exception_t (framework_error_kind_t::rejected,
                                      error_message.empty () ? operation_name + " was rejected."
                                                             : error_message);
    }
    if (error_code == "request_protocol_error") {
        return framework_exception_t (framework_error_kind_t::protocol_error,
                                      error_message.empty ()
                                        ? operation_name + " failed with a protocol error."
                                        : error_message);
    }
    if (error_code == "handler_not_found") {
        return framework_exception_t (
          framework_error_kind_t::not_found,
          error_message.empty () ? operation_name + " failed because the handler was not found."
                                 : error_message);
    }
    if (error_code == "payload_decode_failed") {
        return framework_exception_t (
          framework_error_kind_t::protocol_error,
          error_message.empty () ? operation_name + " failed because the payload could not be decoded."
                                 : error_message);
    }
    return framework_exception_t (framework_error_kind_t::internal_failure,
                                  error_message.empty () ? operation_name + " failed."
                                                         : error_message);
}

framework_exception_t
request_failure_mapper_t::reply_header_exception (
  std::uint32_t terminal_result,
  std::uint32_t failure_code,
  const std::string &operation_name) const
{
    switch (failure_code) {
        case 9:
            return framework_exception_t (
              framework_error_kind_t::not_found,
              operation_name + " failed because the handler was not found.");
        case 12:
            return framework_exception_t (
              framework_error_kind_t::protocol_error,
              operation_name
                + " failed because the payload could not be decoded.");
        case 13:
            return framework_exception_t (
              framework_error_kind_t::unavailable,
              operation_name
                + " failed because the target route is not connected.");
        case 14:
            return framework_exception_t (
              framework_error_kind_t::not_found,
              operation_name + " failed because the target was not found.");
        case 15:
            return framework_exception_t (
              framework_error_kind_t::rejected,
              operation_name + " was rejected.");
        case 16:
            return framework_exception_t (
              framework_error_kind_t::protocol_error,
              operation_name + " failed with a protocol error.");
        case 17:
            return framework_exception_t (
              framework_error_kind_t::internal_failure,
              operation_name + " failed.");
        default:
            break;
    }

    switch (terminal_result) {
        case 101:
            return completion_exception (
              request_result_t::timed_out, operation_name);
        case 102:
            return completion_exception (
              request_result_t::not_found, operation_name);
        case 103:
            return completion_exception (
              request_result_t::terminated, operation_name);
        case 104:
            return completion_exception (
              request_result_t::protocol_error, operation_name);
        case 106:
            return completion_exception (
              request_result_t::rejected, operation_name);
        case 107:
            return completion_exception (
              request_result_t::conflict, operation_name);
        case 108:
            return completion_exception (
              request_result_t::busy, operation_name);
        case 113:
            return framework_exception_t (
              framework_error_kind_t::capacity_exceeded,
              operation_name + " exceeded capacity.");
        case 109:
            return completion_exception (
              request_result_t::not_connected, operation_name);
        case 110:
            return completion_exception (
              request_result_t::invalid_argument, operation_name);
        case 111:
            return completion_exception (
              request_result_t::invalid_state, operation_name);
        case 112:
            return completion_exception (
              request_result_t::not_supported, operation_name);
        default:
            return completion_exception (
              request_result_t::internal_error, operation_name);
    }
}

} // namespace zlink::framework::runtime::messaging
