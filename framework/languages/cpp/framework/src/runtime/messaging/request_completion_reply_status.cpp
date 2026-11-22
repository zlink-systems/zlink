/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

namespace zlink::framework::runtime::messaging
{
result_t<request_wire_failure_t>
request_failure_mapper_t::operation_reply_status (foundation::operation_terminal_t terminal,
                                                  const zlink::message_t &payload) const
{
    using operation_terminal_t = foundation::operation_terminal_t;
    using result_type = result_t<request_wire_failure_t>;
    auto status = zlink::request_result_t::internal_error;
    switch (terminal) {
        case operation_terminal_t::completed:
            return result_type::success ({0, 0});
        case operation_terminal_t::shutdown:
            return detail::boundary_failure<request_wire_failure_t> (
              detail::boundary_error_t::shutdown, "MeshNode operation owner is closed");
        case operation_terminal_t::timed_out:
            status = zlink::request_result_t::timed_out;
            break;
        case operation_terminal_t::route_unavailable:
            status = zlink::request_result_t::not_connected;
            break;
        default:
            break;
    }

    if (!payload.is_empty ()) {
        try {
            const auto reply = protocol::decode_reply_header (protocol::message_bytes (payload));
            return result_type::success ({reply.terminal_result, reply.failure_code});
        }
        catch (const protocol::service_wire_error_t &) {
            status = zlink::request_result_t::protocol_error;
        }
    }
    return result_type::success ({static_cast<std::uint32_t> (status), 0});
}
} // namespace zlink::framework::runtime::messaging
