/* SPDX-License-Identifier: MPL-2.0 */

#include "operation_state.hpp"
#include "received_access.hpp"
#include "../Native/native_message_parts.hpp"

#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include <cerrno>
#include <memory>
#include <optional>
#include <utility>

namespace zlink
{
namespace
{

void submit_raw_reply (detail::operation_state_t &state_)
{
    if (!state_.raw.target.first_rid
        || !state_.reply.token.has_value ())
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    std::shared_ptr<detail::socket_runtime_state_t> runtime;
    std::optional<detail::socket_call_t> call;
    try {
        runtime = detail::share_runtime_state (state_.raw);
        call.emplace (detail::submit_handle (state_.raw.socket));
    }
    catch (...) {
        detail::restore_async_send_sources (state_);
        throw;
    }
    if (!detail::received_access_t::token_owner_matches (
          *state_.reply.token, runtime->reply_owner))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    const zlink_routing_id_t first_rid =
      zlink::detail::routing_id_native_value (*state_.raw.target.first_rid);
    const zlink_reply_token_t token = detail::received_access_t::token_value (
      *state_.reply.token);
    const int rc = detail::submit_message_parts (
      state_.message.parts, [&] (zlink_msg_t *parts_, size_t part_count_) {
          return zlink_reply (call->handle (), &first_rid, token, parts_, part_count_);
      });
    if (rc == ZLINK_SUBMIT_OK)
        return;

    const int submit_errno = zlink_errno ();
    detail::restore_send_parts_to_sources (state_, state_.message.parts);
    if (rc == -1)
        throw error_t (submit_errno);
    throw submit_error_t (static_cast<submit_result_t> (rc), submit_errno);
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
