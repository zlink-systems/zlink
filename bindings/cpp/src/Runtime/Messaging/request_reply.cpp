/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include "completion_owner.hpp"
#include "operation_state.hpp"
#include <Runtime/Core/duration_conversion.hpp>
#include <Runtime/Native/native_message_parts.hpp>

#include <cerrno>

namespace zlink
{
namespace
{

struct request_submission_t
{
    std::shared_ptr<detail::socket_runtime_state_t> runtime;
    zlink_completion_id_t completion_id = 0;
};

request_submission_t submit_request_native (
  detail::operation_state_t &state_, zlink_send_flags_t flags_, void *user_context_)
{
    if (!state_.raw.socket
        || (state_.kind != detail::operation_kind_t::raw_request
            && state_.kind != detail::operation_kind_t::raw_routed_request))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    const auto runtime = detail::share_runtime_state (state_.raw);
    if (!runtime)
        throw submit_error_t (submit_result_t::invalid_state, ESHUTDOWN);

    const zlink_routing_id_t *target =
      state_.kind == detail::operation_kind_t::raw_routed_request
        ? detail::target_first_rid_native (state_.raw.target) : nullptr;
    if (state_.kind == detail::operation_kind_t::raw_routed_request && !target)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    const uint32_t timeout = state_.timeout > std::chrono::milliseconds::zero ()
                               ? detail::native_timeout_ms (state_.timeout) : 0u;

    zlink_completion_id_t completion_id = 0;
    auto submit_part = [&] (zlink_msg_t *part_, zlink_part_flag_t part_flag_,
                            bool is_final_) {
        return zlink_request_part (
          state_.raw.socket, target, part_, flags_, part_flag_, is_final_ ? timeout : 0u,
          is_final_ ? user_context_ : nullptr,
          is_final_ ? &completion_id : nullptr);
    };

    int raw_result = -1;
    if (state_.message.single_part.has_value () || state_.message.single_part_source) {
        message_t &part = detail::send_single_part (state_);
        raw_result = detail::submit_borrowed_message_part (
          part, [&] (zlink_msg_t *native_, zlink_part_flag_t flag_) {
              return submit_part (native_, flag_, true);
          });
        if (raw_result != ZLINK_SUBMIT_OK)
            detail::restore_single_send_part_to_source (state_);
    } else {
        raw_result = detail::submit_message_parts (
          state_.message.parts,
          [&] (zlink_msg_t *native_, zlink_part_flag_t flag_, bool final_) {
              return submit_part (native_, flag_, final_);
          });
        if (raw_result != ZLINK_SUBMIT_OK)
            detail::restore_send_parts_to_sources (state_, state_.message.parts);
    }

    if (raw_result != ZLINK_SUBMIT_OK) {
        const int error = zlink_errno ();
        const submit_result_t result = raw_result == -1
                                         ? detail::submit_result_from_errno (error)
                                         : static_cast<submit_result_t> (raw_result);
        throw submit_error_t (result, error);
    }
    if (completion_id == 0)
        throw submit_error_t (submit_result_t::internal_error, EPROTO);
    return {runtime, completion_id};
}

} // namespace

request_submit_operation_t::~request_submit_operation_t () = default;
request_submit_operation_t::request_submit_operation_t (request_submit_operation_t &&) noexcept =
  default;
request_submit_operation_t &
request_submit_operation_t::operator= (request_submit_operation_t &&) noexcept = default;

request_submit_operation_t &&request_submit_operation_t::message (message_t &part_) &&
{
    detail::append_send_part (state (), part_);
    return std::move (*this);
}

request_submit_operation_t &&request_submit_operation_t::message (message_t &&part_) &&
{
    detail::append_send_part (state (), std::move (part_));
    return std::move (*this);
}

request_submit_operation_t &&
request_submit_operation_t::timeout (std::chrono::milliseconds timeout_) &&
{
    if (timeout_ < std::chrono::milliseconds::zero ())
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    state ().timeout = timeout_;
    return std::move (*this);
}

request_operation_t::~request_operation_t () = default;
request_operation_t::request_operation_t (request_operation_t &&) noexcept = default;
request_operation_t &request_operation_t::operator= (request_operation_t &&) noexcept = default;

request_submit_operation_t request_operation_t::message (message_t &part_) &&
{
    state ().message.single_part_source = &part_;
    return request_submit_operation_t (release_state_ptr ());
}

request_submit_operation_t request_operation_t::message (message_t &&part_) &&
{
    state ().message.single_part.emplace (std::move (part_));
    state ().message.single_part_source = nullptr;
    return request_submit_operation_t (release_state_ptr ());
}

std::vector<message_t> request_submit_operation_t::submit () &&
{
    auto &operation = state ();
    if (!detail::has_send_parts (operation))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    const auto runtime = detail::share_runtime_state (operation.raw);
    if (!runtime)
        throw submit_error_t (submit_result_t::invalid_state, ESHUTDOWN);

    std::shared_ptr<detail::async_operation_state_t<std::vector<message_t>>> no_async;
    auto entry = std::make_shared<detail::completion_entry_t> (no_async);
    runtime->completion->register_entry (entry);
    try {
        const request_submission_t submitted = submit_request_native (
          operation, static_cast<zlink_send_flags_t> (0), entry.get ());
        entry->publish (submitted.completion_id);
    }
    catch (...) {
        entry->fail_submit ();
        runtime->completion->unregister_entry (entry.get ());
        throw;
    }
    return entry->wait_request ();
}

async_result_t<std::vector<message_t>> request_submit_operation_t::async () &&
{
    auto &operation = state ();
    if (!detail::has_send_parts (operation))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    const auto runtime = detail::share_runtime_state (operation.raw);
    if (!runtime)
        throw submit_error_t (submit_result_t::invalid_state, ESHUTDOWN);

    auto result = std::make_shared<
      detail::async_operation_state_t<std::vector<message_t>>> ();
    auto entry = std::make_shared<detail::completion_entry_t> (result);
    runtime->completion->register_entry (entry);
    try {
        const request_submission_t submitted = submit_request_native (
          operation, static_cast<zlink_send_flags_t> (ZLINK_DONTWAIT), entry.get ());
        entry->publish (submitted.completion_id);
    }
    catch (...) {
        entry->fail_submit ();
        runtime->completion->unregister_entry (entry.get ());
        throw;
    }
    return detail::async_result_access_t::make<std::vector<message_t>> (
      std::move (result));
}

} // namespace zlink
