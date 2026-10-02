/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/messaging/request_failure_mapper.hpp"
#include <service_wire_constants.hpp>

namespace zlink::framework::runtime::messaging
{

std::uint32_t
request_failure_mapper_t::reply_failure_code (std::uint32_t terminal_result) const noexcept
{
    using protocol::framework_error_code;
    framework_error_code failure;
    switch (static_cast<protocol::request_terminal_result> (terminal_result)) {
        case protocol::request_terminal_result::notFound:
            failure = framework_error_code::requestTargetNotFound;
            break;
        case protocol::request_terminal_result::protocolError:
            failure = framework_error_code::requestProtocolError;
            break;
        case protocol::request_terminal_result::internalError:
            failure = framework_error_code::requestFailed;
            break;
        case protocol::request_terminal_result::rejected:
            failure = framework_error_code::requestRejected;
            break;
        default:
            failure = framework_error_code::none;
            break;
    }
    return static_cast<std::uint32_t> (failure);
}

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
            return framework_exception_t (framework_error_kind_t::unavailable,
                                          operation_name + " target is unavailable.");
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
        return detail::make_boundary_exception (
          detail::boundary_error_t::timed_out,
          error_message.empty () ? operation_name + " timed out." : error_message);
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
        return framework_exception_t (framework_error_kind_t::rejected,
                                      error_message.empty () ? operation_name + " was rejected."
                                                             : error_message);
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
        return framework_exception_t (framework_error_kind_t::protocol_error,
                                      error_message.empty ()
                                        ? operation_name + " failed with a protocol error."
                                        : error_message);
    }
    if (error_code == "invalid_operation") {
        return framework_exception_t (framework_error_kind_t::invalid_operation,
                                      error_message.empty ()
                                        ? operation_name + " is invalid in the current state."
                                        : error_message);
    }
    if (error_code == "data_lost") {
        return framework_exception_t (framework_error_kind_t::data_lost,
                                      error_message.empty ()
                                        ? operation_name + " failed because required data was lost."
                                        : error_message);
    }
    if (error_code == "internal_failure") {
        return framework_exception_t (framework_error_kind_t::internal_failure,
                                      error_message.empty () ? operation_name + " failed."
                                                             : error_message);
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
        return framework_exception_t (framework_error_kind_t::protocol_error,
                                      error_message.empty ()
                                        ? operation_name
                                            + " failed because the payload could not be decoded."
                                        : error_message);
    }
    return framework_exception_t (framework_error_kind_t::internal_failure,
                                  error_message.empty () ? operation_name + " failed."
                                                         : error_message);
}

framework_exception_t
request_failure_mapper_t::reply_header_exception (std::uint32_t terminal_result,
                                                  std::uint32_t failure_code,
                                                  const std::string &operation_name) const
{
    switch (static_cast<protocol::framework_error_code> (failure_code)) {
        case protocol::framework_error_code::actorAlreadyExists:
            return framework_exception_t (framework_error_kind_t::already_exists,
                                          operation_name
                                            + " failed because the actor already exists.");
        case protocol::framework_error_code::actorTypeMismatch:
        case protocol::framework_error_code::spotTypeMismatch:
            return framework_exception_t (framework_error_kind_t::type_mismatch,
                                          operation_name
                                            + " failed because the object type did not match.");
        case protocol::framework_error_code::actorSessionNotBound:
            return framework_exception_t (framework_error_kind_t::invalid_operation,
                                          operation_name
                                            + " failed because the actor session is not bound.");
        case protocol::framework_error_code::handlerNotFound:
            return framework_exception_t (framework_error_kind_t::not_found,
                                          operation_name
                                            + " failed because the handler was not found.");
        case protocol::framework_error_code::payloadDecodeFailed:
            return framework_exception_t (framework_error_kind_t::protocol_error,
                                          operation_name
                                            + " failed because the payload could not be decoded.");
        case protocol::framework_error_code::routeNotConnected:
            return framework_exception_t (framework_error_kind_t::unavailable,
                                          operation_name
                                            + " failed because the target route is not connected.");
        case protocol::framework_error_code::requestTargetNotFound:
            return framework_exception_t (framework_error_kind_t::not_found,
                                          operation_name
                                            + " failed because the target was not found.");
        case protocol::framework_error_code::requestRejected:
            return framework_exception_t (framework_error_kind_t::rejected,
                                          operation_name + " was rejected.");
        case protocol::framework_error_code::requestProtocolError:
            return framework_exception_t (framework_error_kind_t::protocol_error,
                                          operation_name + " failed with a protocol error.");
        case protocol::framework_error_code::requestFailed:
            return framework_exception_t (framework_error_kind_t::internal_failure,
                                          operation_name + " failed.");
        case protocol::framework_error_code::workerQueueFull:
            // Legacy peers may still send workerQueueFull for an unavailable
            // remote target.
            return framework_exception_t (framework_error_kind_t::unavailable,
                                          operation_name
                                            + " failed because the remote worker queue is full.");
        case protocol::framework_error_code::workerTimedOut:
            return framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                          operation_name + " timed out inside the worker.");
        case protocol::framework_error_code::workerFailed:
            return framework_exception_t (framework_error_kind_t::internal_failure,
                                          operation_name + " failed inside the worker.");
        case protocol::framework_error_code::actorLocationStale:
            return framework_exception_t (framework_error_kind_t::unavailable,
                                          operation_name
                                            + " failed because the actor location was stale.");
        case protocol::framework_error_code::spotGenerationStale:
            return framework_exception_t (framework_error_kind_t::invalid_operation,
                                          operation_name
                                            + " failed because the spot generation was stale.");
        case protocol::framework_error_code::spotMoving:
            return framework_exception_t (framework_error_kind_t::unavailable,
                                          operation_name + " failed because the spot is moving.");
        case protocol::framework_error_code::relocationDataLost:
            return framework_exception_t (framework_error_kind_t::data_lost,
                                          operation_name
                                            + " failed because relocation data was lost.");
        default:
            break;
    }

    switch (static_cast<protocol::request_terminal_result> (terminal_result)) {
        case protocol::request_terminal_result::timedOut:
            return completion_exception (request_result_t::timed_out, operation_name);
        case protocol::request_terminal_result::notFound:
            return completion_exception (request_result_t::not_found, operation_name);
        case protocol::request_terminal_result::terminated:
            return completion_exception (request_result_t::terminated, operation_name);
        case protocol::request_terminal_result::protocolError:
            return completion_exception (request_result_t::protocol_error, operation_name);
        case protocol::request_terminal_result::rejected:
            return completion_exception (request_result_t::rejected, operation_name);
        case protocol::request_terminal_result::conflict:
        case protocol::request_terminal_result::busy:
            // A terminal-only conflict/busy reply identifies an unavailable
            // remote target. Fine failures are handled above.
            return framework_exception_t (framework_error_kind_t::unavailable,
                                          operation_name
                                            + " failed because the remote target was busy.");
        case protocol::request_terminal_result::backpressured:
            return framework_exception_t (framework_error_kind_t::unavailable,
                                          operation_name + " target is unavailable.");
        case protocol::request_terminal_result::notConnected:
            return completion_exception (request_result_t::not_connected, operation_name);
        case protocol::request_terminal_result::invalidArgument:
            return completion_exception (request_result_t::invalid_argument, operation_name);
        case protocol::request_terminal_result::invalidState:
            return completion_exception (request_result_t::invalid_state, operation_name);
        case protocol::request_terminal_result::notSupported:
            return completion_exception (request_result_t::not_supported, operation_name);
        default:
            return completion_exception (request_result_t::internal_error, operation_name);
    }
}

} // namespace zlink::framework::runtime::messaging
