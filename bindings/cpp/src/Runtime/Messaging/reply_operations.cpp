/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <Runtime/Messaging/received_access.hpp>
#include "operation_detail.hpp"
#include "operation_submit.hpp"
#include "operation_state.hpp"

#include <cerrno>

namespace zlink
{
namespace
{

void submit_raw_reply (detail::operation_state_t &state_)
{
    if (!state_.raw.socket || !state_.raw.target.first_rid
        || !state_.reply.token.has_value ())
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    const auto runtime = detail::share_runtime_state (state_.raw);
    if (!runtime)
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);
    if (!detail::received_access_t::token_owner_matches (
          *state_.reply.token, runtime->reply_owner))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    const zlink_routing_id_t first_rid =
      zlink::detail::routing_id_native_value (*state_.raw.target.first_rid);
    const zlink_reply_token_t token = detail::received_access_t::token_value (
      *state_.reply.token);
    const int rc = detail::submit_message_parts (
      state_.message.parts, [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_, bool) {
          return zlink_reply_part (state_.raw.socket, &first_rid, token, part_out_, part_flag_);
      });
    if (rc == -1) {
        detail::restore_send_parts_to_sources (state_, state_.message.parts);
        throw last_error ();
    }

    const submit_result_t result = static_cast<submit_result_t> (rc);
    if (result != submit_result_t::ok) {
        detail::restore_send_parts_to_sources (state_, state_.message.parts);
        throw submit_error_t (result, zlink_errno ());
    }
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

reply_operation_t::~reply_operation_t () = default;
reply_operation_t::reply_operation_t (reply_operation_t &&) noexcept = default;
reply_operation_t &reply_operation_t::operator= (reply_operation_t &&) noexcept = default;

reply_submit_operation_t reply_operation_t::message (message_t &part_) &&
{
    detail::append_send_part_from (state (), part_, &part_);
    return reply_submit_operation_t (release_state_ptr ());
}

void reply_submit_operation_t::submit () &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    if (state.kind == detail::operation_kind_t::raw_reply) {
        submit_raw_reply (state);
        return;
    }

    throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
}

} // namespace zlink
