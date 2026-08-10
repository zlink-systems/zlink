/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <Runtime/Messaging/received_access.hpp>
#include "operation_detail.hpp"
#include "operation_submit.hpp"
#include "operation_state.hpp"

namespace zlink
{

send_submit_operation_t::~send_submit_operation_t () = default;
send_submit_operation_t::send_submit_operation_t (send_submit_operation_t &&) noexcept = default;
send_submit_operation_t &
send_submit_operation_t::operator= (send_submit_operation_t &&) noexcept = default;

send_submit_operation_t &&send_submit_operation_t::message (message_t &part_) &&
{
    detail::append_send_part (state (), part_);
    return std::move (*this);
}

send_submit_operation_t &&send_submit_operation_t::message (message_t &&part_) &&
{
    state ().message.single_part.emplace (std::move (part_));
    state ().message.single_part_source = nullptr;
    state ().message.discard_single_part_on_backpressure = true;
    return std::move (*this);
}

send_submit_operation_t &&send_submit_operation_t::flags (int flags_) &&
{
    state ().flags = send_flags_t (flags_);
    return std::move (*this);
}

send_operation_t::~send_operation_t () = default;
send_operation_t::send_operation_t (send_operation_t &&) noexcept = default;
send_operation_t &send_operation_t::operator= (send_operation_t &&) noexcept = default;

send_submit_operation_t send_operation_t::message (message_t &part_) &&
{
    state ().message.single_part_source = &part_;
    if (!detail::can_borrow_single_send_part (state ().kind))
        state ().message.single_part.emplace (std::move (part_));
    return send_submit_operation_t (release_state_ptr ());
}

send_submit_operation_t send_operation_t::message (message_t &&part_) &&
{
    state ().message.single_part.emplace (std::move (part_));
    state ().message.single_part_source = nullptr;
    state ().message.discard_single_part_on_backpressure = true;
    return send_submit_operation_t (release_state_ptr ());
}

bool send_submit_operation_t::submit () &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    switch (state.kind) {
        case detail::operation_kind_t::raw_send:
        case detail::operation_kind_t::raw_routed_send:
        case detail::operation_kind_t::raw_publish:
            return detail::submit_raw_send_state (state);
        case detail::operation_kind_t::received_send: {
            if (!state.received.received
                || !zlink::detail::received_access_t::has_send_context (
                  *state.received.received))
                throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
            if (detail::send_part_count (state) == 1u) {
                message_t &part = detail::send_single_part (state);
                if (!part.valid ()) {
                    detail::restore_single_send_part_to_source (state);
                    throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
                }

                submit_result_t result = submit_result_t::invalid_argument;
                int result_errno = EINVAL;
                if (zlink::detail::received_access_t::submit_direct_send (
                      *state.received.received, part, state.flags, result, result_errno)) {
                    if (result == submit_result_t::ok)
                        return true;
                    detail::restore_single_send_part_to_source (state);
                    if (state.flags == send_flags_t::dontwait
                        && result == submit_result_t::backpressured)
                        return false;
                    throw submit_error_t (result, result_errno);
                }
            }
            std::vector<message_t> parts = detail::take_send_parts (state);
            const bool sent =
              zlink::detail::received_access_t::submit_send (
                *state.received.received, parts, state.flags);
            if (!sent)
                detail::restore_single_send_part_to_source (state, parts);
            return sent;
        }
        default:
            break;
    }

    throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
}

} // namespace zlink
