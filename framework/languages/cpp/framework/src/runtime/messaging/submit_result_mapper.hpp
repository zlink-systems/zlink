/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/errors/error.hpp>
#include "runtime/messaging/request_failure_mapper.hpp"

#include <zlink/Contracts/Messaging/request_result.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <string>
#include <utility>

namespace zlink::framework::runtime::messaging
{

// Projects the typed binding result at its initial or completion boundary.
inline zlink::submit_result_t map_submit_completion_result (zlink::submit_result_t result) noexcept
{
    return result == zlink::submit_result_t::not_found ? zlink::submit_result_t::not_connected
                                                       : result;
}

inline zlink::request_result_t map_submit_request_result (zlink::submit_result_t result,
                                                          bool completion_failure) noexcept
{
    if (completion_failure)
        result = map_submit_completion_result (result);
    switch (result) {
        case zlink::submit_result_t::ok:
            return zlink::request_result_t::ok;
        case zlink::submit_result_t::backpressured:
            return zlink::request_result_t::backpressured;
        case zlink::submit_result_t::not_connected:
            return zlink::request_result_t::not_connected;
        case zlink::submit_result_t::not_found:
            return zlink::request_result_t::not_found;
        case zlink::submit_result_t::not_admitted:
            return zlink::request_result_t::rejected;
        case zlink::submit_result_t::terminated:
            return zlink::request_result_t::terminated;
        case zlink::submit_result_t::invalid_state:
            return zlink::request_result_t::invalid_state;
        case zlink::submit_result_t::invalid_handle:
        case zlink::submit_result_t::invalid_argument:
        case zlink::submit_result_t::thread_violation:
            return zlink::request_result_t::invalid_argument;
        case zlink::submit_result_t::not_supported:
            return zlink::request_result_t::not_supported;
        default:
            return zlink::request_result_t::internal_error;
    }
}

inline framework_exception_t map_submit_result_exception (zlink::submit_result_t result,
                                                          std::string message)
{
    const auto terminal = static_cast<std::uint32_t> (map_submit_request_result (result, false));
    const request_failure_mapper_t mapper;
    return mapper.reply_header_exception (terminal, mapper.reply_failure_code (terminal), message);
}

inline framework_error_kind_t map_submit_result_error_kind (zlink::submit_result_t result)
{
    return map_submit_result_exception (result, {}).kind ();
}

inline framework_exception_t map_request_result_exception (zlink::request_result_t result,
                                                           std::string message)
{
    const auto terminal = static_cast<std::uint32_t> (result);
    const request_failure_mapper_t mapper;
    return mapper.reply_header_exception (terminal, mapper.reply_failure_code (terminal), message);
}

/* Select-one channel variant. A channel reports not_found when applying
 * eligibility and drain left no member to pick: the send path and its
 * connection are still there, so the spec ends that as unavailable rather than
 * not_found, and a request and a one-way send agree
 * (06-framework-api "no eligible select-one member"). Node-direct callers keep
 * map_submit_result_exception, where not_found still means a named target is
 * absent. */
inline framework_exception_t map_channel_submit_result_exception (zlink::submit_result_t result,
                                                                  std::string message)
{
    if (result == zlink::submit_result_t::not_found) {
        return detail::make_boundary_exception (detail::boundary_error_t::disconnected,
                                                std::move (message));
    }
    return map_submit_result_exception (result, std::move (message));
}

} // namespace zlink::framework::runtime::messaging
