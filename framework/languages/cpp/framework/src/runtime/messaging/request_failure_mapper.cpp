/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/messaging/request_failure_mapper.hpp"
#include <service_wire_constants.hpp>


namespace zlink::framework::runtime::messaging
{

namespace
{
using terminal_t = protocol::request_terminal_result;
using failure_t = protocol::framework_error_code;
using kind_t = framework_error_kind_t;

// The first entry for each ErrorKind is its outgoing representation. Later entries
// retain incoming fine-code aliases and their existing diagnostic text.
struct wire_failure_mapping_t
{
    kind_t kind;
    failure_t failure;
    terminal_t terminal;
    const char *message;
    detail::boundary_error_t boundary = detail::boundary_error_t::none;
    bool receive = true;
};
constexpr wire_failure_mapping_t wire_failure_mapping[] = {
  {kind_t::not_found, failure_t::requestTargetNotFound, terminal_t::notFound,
   " failed because the target was not found."},
  {kind_t::not_found, failure_t::actorRouteNotFound, terminal_t::notFound,
   " failed because the actor route was not found."},
  {kind_t::not_found, failure_t::spotRouteNotFound, terminal_t::notFound,
   " failed because the spot route was not found."},
  {kind_t::not_found, failure_t::routeHandlerNotFound, terminal_t::notFound,
   " failed because the route handler was not found."},
  {kind_t::not_found, failure_t::actorDispatchHandlerNotFound, terminal_t::notFound,
   " failed because the actor handler was not found."},
  {kind_t::already_exists, failure_t::actorAlreadyExists, terminal_t::conflict,
   " failed because the actor already exists."},
  {kind_t::type_mismatch, failure_t::spotTypeMismatch, terminal_t::conflict,
   " failed because the object type did not match."},
  {kind_t::type_mismatch, failure_t::actorTypeMismatch, terminal_t::conflict,
   " failed because the object type did not match."},
  {kind_t::not_configured, failure_t::requestFailed, terminal_t::internalError, " failed.",
   detail::boundary_error_t::none, false},
  {kind_t::rejected, failure_t::requestRejected, terminal_t::rejected, " was rejected."},
  {kind_t::rejected, failure_t::actorCreateRejected, terminal_t::rejected, " was rejected."},
  {kind_t::unavailable, failure_t::routeNotConnected, terminal_t::internalError,
   " failed because the target route is not connected.", detail::boundary_error_t::disconnected},
  {kind_t::deadline_exceeded, failure_t::workerTimedOut, terminal_t::internalError,
   " timed out inside the worker."},
  {kind_t::shutting_down, failure_t::none, terminal_t::terminated, nullptr},
  {kind_t::protocol_error, failure_t::requestProtocolError, terminal_t::protocolError,
   " failed with a protocol error."},
  {kind_t::invalid_operation, failure_t::none, terminal_t::invalidState, nullptr},
  {kind_t::data_lost, failure_t::relocationDataLost, terminal_t::internalError,
   " failed because relocation data was lost."},
  {kind_t::internal_failure, failure_t::requestFailed, terminal_t::internalError, " failed."},
  {kind_t::invalid_operation, failure_t::actorSessionNotBound, terminal_t::notFound,
   " failed because the actor session is not bound."},
  {kind_t::not_found, failure_t::handlerNotFound, terminal_t::notFound,
   " failed because the handler was not found."},
  {kind_t::protocol_error, failure_t::payloadDecodeFailed, terminal_t::protocolError,
   " failed because the payload could not be decoded."},
  // Legacy peers may still report an unavailable target as workerQueueFull.
  {kind_t::unavailable, failure_t::workerQueueFull, terminal_t::rejected,
   " failed because the remote worker queue is full."},
  {kind_t::internal_failure, failure_t::actorCreateFailed, terminal_t::internalError,
   " failed because actor creation failed."},
  {kind_t::internal_failure, failure_t::spotCreateFailed, terminal_t::internalError,
   " failed because spot creation failed."},
  {kind_t::internal_failure, failure_t::workerFailed, terminal_t::internalError,
   " failed inside the worker."},
  {kind_t::unavailable, failure_t::actorLocationStale, terminal_t::conflict,
   " failed because the actor location was stale."},
  {kind_t::invalid_operation, failure_t::spotGenerationStale, terminal_t::conflict,
   " failed because the spot generation was stale."},
  {kind_t::unavailable, failure_t::spotMoving, terminal_t::conflict,
   " failed because the spot is moving."}};
}

std::optional<request_wire_failure_t>
request_failure_mapper_t::target_failure_reply (framework_error_kind_t kind,
                                                std::uint32_t cause_code) const
{
    const wire_failure_mapping_t *representative = nullptr;
    for (const auto &row : wire_failure_mapping) {
        if (row.kind != kind)
            continue;
        if (cause_code == 0)
            return request_wire_failure_t{static_cast<std::uint32_t> (row.terminal),
                                          static_cast<std::uint32_t> (row.failure)};
        if (!representative)
            representative = &row;
        if (cause_code != 0 && row.receive
            && cause_code == static_cast<std::uint32_t> (row.failure))
            return request_wire_failure_t{static_cast<std::uint32_t> (row.terminal), cause_code};
    }
    if (!representative)
        return std::nullopt;
    return request_wire_failure_t{static_cast<std::uint32_t> (representative->terminal),
                                  static_cast<std::uint32_t> (representative->failure)};
}

framework_error_kind_t
request_failure_mapper_t::failure_code_kind (std::uint32_t failure_code) const noexcept
{
    for (const auto &row : wire_failure_mapping) {
        if (row.receive && failure_code != 0
            && failure_code == static_cast<std::uint32_t> (row.failure))
            return row.kind;
    }
    return framework_error_kind_t::internal_failure;
}

std::optional<foundation::operation_terminal_t>
request_failure_mapper_t::transport_terminal (zlink::request_result_t terminal) const noexcept
{
    switch (terminal) {
        case zlink::request_result_t::timed_out:
            return foundation::operation_terminal_t::timed_out;
        case zlink::request_result_t::terminated:
            return foundation::operation_terminal_t::shutdown;
        case zlink::request_result_t::not_connected:
        case zlink::request_result_t::conflict:
        case zlink::request_result_t::busy:
        case zlink::request_result_t::backpressured:
            return foundation::operation_terminal_t::transport_failed;
        default:
            return std::nullopt;
    }
}

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
    // Fine codes retain precedence even for an invalid terminal/code combination.
    // None is decoded by the unchanged source/native terminal fallback below.
    if (failure_code != static_cast<std::uint32_t> (failure_t::none)) {
        for (const auto &[kind, failure, terminal, message, boundary, receive] :
             wire_failure_mapping) {
            if (receive && failure_code == static_cast<std::uint32_t> (failure))
                return detail::with_failure_code (
                  boundary == detail::boundary_error_t::none
                    ? framework_exception_t (kind, operation_name + message)
                    : detail::make_boundary_exception (boundary, operation_name + message),
                  failure_code);
        }
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
