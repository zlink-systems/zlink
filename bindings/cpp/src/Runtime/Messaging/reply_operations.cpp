/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <Runtime/Messaging/received_access.hpp>
#include "operation_detail.hpp"
#include "operation_submit.hpp"
#include "operation_state.hpp"

namespace zlink
{
namespace
{

void submit_raw_reply (detail::operation_state_t &state_)
{
    if (!state_.raw.socket || !state_.raw.target.first_rid)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    zlink::detail::throw_if_reply_flags_unsupported (state_.flags);
    const zlink_routing_id_t first_rid =
      zlink::detail::routing_id_native_value (*state_.raw.target.first_rid);
    const int rc = detail::submit_message_parts (
      state_.message.parts, [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_, bool) {
          return zlink_router_reply_part (state_.raw.socket, &first_rid,
                                          state_.reply.request_seq, part_out_, part_flag_);
      });
    if (rc == -1)
        throw last_error ();

    const submit_result_t result = static_cast<submit_result_t> (rc);
    if (result != submit_result_t::ok)
        throw submit_error_t (result, zlink_errno ());
}

} // namespace

reply_submit_operation_t::~reply_submit_operation_t () = default;
reply_submit_operation_t::reply_submit_operation_t (reply_submit_operation_t &&) noexcept = default;
reply_submit_operation_t &
reply_submit_operation_t::operator= (reply_submit_operation_t &&) noexcept = default;

reply_submit_operation_t &&reply_submit_operation_t::message (message_t &part_) &&
{
    detail::append_send_part (state (), part_);
    return std::move (*this);
}

reply_submit_operation_t &&reply_submit_operation_t::flags (int flags_) &&
{
    state ().flags = send_flags_t (flags_);
    return std::move (*this);
}

reply_operation_t::~reply_operation_t () = default;
reply_operation_t::reply_operation_t (reply_operation_t &&) noexcept = default;
reply_operation_t &reply_operation_t::operator= (reply_operation_t &&) noexcept = default;

reply_submit_operation_t reply_operation_t::message (message_t &part_) &&
{
    if (state ().kind == detail::operation_kind_t::received_reply)
        state ().message.single_part_source = &part_;
    else
        state ().message.parts.push_back (std::move (part_));
    return reply_submit_operation_t (release_state_ptr ());
}

void reply_submit_operation_t::submit () &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    if (state.kind == detail::operation_kind_t::received_reply) {
        if (!state.received.received
            || !zlink::detail::received_access_t::has_reply_context (*state.received.received))
            throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
        zlink::detail::throw_if_reply_flags_unsupported (state.flags);
        if (detail::send_part_count (state) == 1u) {
            message_t &part = detail::send_single_part (state);
            if (!part.valid ())
                throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
            submit_result_t result = submit_result_t::invalid_argument;
            int result_errno = EINVAL;
            if (zlink::detail::received_access_t::submit_direct_reply (
                  *state.received.received, part, result, result_errno)) {
                if (result == submit_result_t::ok)
                    return;
                throw submit_error_t (result, result_errno);
            }
        }
        std::vector<message_t> parts = detail::take_send_parts (state);
        zlink::detail::received_access_t::submit_reply (*state.received.received,
                                                        parts, state.flags);
        return;
    }

    if (state.kind == detail::operation_kind_t::raw_reply) {
        submit_raw_reply (state);
        return;
    }

    throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
}

} // namespace zlink
