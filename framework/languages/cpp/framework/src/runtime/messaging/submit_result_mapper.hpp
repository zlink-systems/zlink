/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/errors/error.hpp>

#include <zlink/Contracts/Messaging/request_result.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <string>
#include <utility>

namespace zlink::framework::runtime::messaging
{

/* Converts the Core submit result into the Framework public error kind. The
 * submit result remains an internal transport boundary; callers only observe
 * the Framework kind and, where applicable, its boundary error code. */
inline framework_error_kind_t
map_submit_result_error_kind (zlink::submit_result_t result) noexcept
{
    switch (result) {
        case zlink::submit_result_t::ok:
            return framework_error_kind_t::internal_failure;
        case zlink::submit_result_t::backpressured:
            return framework_error_kind_t::capacity_exceeded;
        case zlink::submit_result_t::not_connected:
            return framework_error_kind_t::unavailable;
        case zlink::submit_result_t::not_found:
            return framework_error_kind_t::not_found;
        case zlink::submit_result_t::not_admitted:
            return framework_error_kind_t::rejected;
        case zlink::submit_result_t::terminated:
            return framework_error_kind_t::shutting_down;
        case zlink::submit_result_t::invalid_handle:
        case zlink::submit_result_t::invalid_argument:
        case zlink::submit_result_t::invalid_state:
        case zlink::submit_result_t::thread_violation:
            return framework_error_kind_t::invalid_operation;
        case zlink::submit_result_t::not_supported:
        case zlink::submit_result_t::out_of_memory:
        case zlink::submit_result_t::seq_exhausted:
        case zlink::submit_result_t::internal_error:
            return framework_error_kind_t::internal_failure;
    }
    return framework_error_kind_t::internal_failure;
}

inline framework_exception_t
map_submit_result_exception (zlink::submit_result_t result, std::string message)
{
    switch (result) {
        case zlink::submit_result_t::not_connected:
            return detail::make_boundary_exception (
              detail::boundary_error_t::disconnected, std::move (message));
        case zlink::submit_result_t::terminated:
            return detail::make_boundary_exception (
              detail::boundary_error_t::shutdown, std::move (message));
        case zlink::submit_result_t::ok:
            break;
        default:
            return framework_exception_t (
              map_submit_result_error_kind (result), std::move (message));
    }
    return framework_exception_t (
      framework_error_kind_t::internal_failure, std::move (message));
}

inline framework_exception_t
map_request_result_exception (zlink::request_result_t result, std::string message)
{
    switch (result) {
        case zlink::request_result_t::timed_out:
            return detail::make_boundary_exception (
              detail::boundary_error_t::timed_out, std::move (message));
        case zlink::request_result_t::not_found:
            return framework_exception_t (
              framework_error_kind_t::not_found, std::move (message));
        case zlink::request_result_t::terminated:
            return detail::make_boundary_exception (
              detail::boundary_error_t::shutdown, std::move (message));
        case zlink::request_result_t::protocol_error:
            return framework_exception_t (
              framework_error_kind_t::protocol_error, std::move (message));
        case zlink::request_result_t::rejected:
            return framework_exception_t (
              framework_error_kind_t::rejected, std::move (message));
        case zlink::request_result_t::conflict:
        case zlink::request_result_t::busy:
            return framework_exception_t (
              framework_error_kind_t::capacity_exceeded, std::move (message));
        case zlink::request_result_t::not_connected:
            return detail::make_boundary_exception (
              detail::boundary_error_t::disconnected, std::move (message));
        case zlink::request_result_t::invalid_argument:
        case zlink::request_result_t::invalid_state:
            return framework_exception_t (
              framework_error_kind_t::invalid_operation, std::move (message));
        case zlink::request_result_t::not_supported:
        case zlink::request_result_t::internal_error:
            return framework_exception_t (
              framework_error_kind_t::internal_failure, std::move (message));
        case zlink::request_result_t::ok:
            break;
    }
    return framework_exception_t (
      framework_error_kind_t::internal_failure, std::move (message));
}

} // namespace zlink::framework::runtime::messaging
