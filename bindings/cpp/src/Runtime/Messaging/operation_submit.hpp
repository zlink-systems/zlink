/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_MESSAGING_OPERATION_SUBMIT_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_MESSAGING_OPERATION_SUBMIT_HPP_INCLUDED

#include "operation_state.hpp"
#include "../Core/duration_conversion.hpp"
#include "../Native/native_message_parts.hpp"

#include <zlink/Contracts/Errors/errors.hpp>

#include <cerrno>
#include <utility>
#include <vector>

namespace zlink
{
namespace detail
{

inline std::vector<message_t> take_send_parts (operation_state_t &state_)
{
    std::vector<message_t> parts;
    if (state_.message.single_part.has_value ()) {
        parts.push_back (std::move (*state_.message.single_part));
        state_.message.single_part_source = nullptr;
    } else if (state_.message.single_part_source) {
        parts.push_back (std::move (*state_.message.single_part_source));
    } else {
        parts = std::move (state_.message.parts);
    }
    return parts;
}

template <typename RestoreSources>
bool submit_wait_or_throw (submit_result_t result_, int errno_,
                           zlink_completion_id_t id_, operation_state_t &state_,
                           void *user_context_,
                           const RestoreSources &restore_sources_)
{
    if (result_ == submit_result_t::backpressured && id_ != 0) {
        if (errno_ != EAGAIN) {
            if (auto runtime = state_.raw.socket->runtime.lock ())
                runtime->completion->reject_wait_token (user_context_, id_);
            result_ = submit_result_t::internal_error;
            errno_ = EPROTO;
        } else if (state_.flags == send_flags_t::dontwait) {
            return false;
        }
    }
    restore_sources_ ();
    throw submit_error_t (result_, errno_);
}

inline bool submit_raw_send_state (operation_state_t &state_,
                                   void *user_context_ = nullptr,
                                   zlink_completion_id_t *completion_id_out_ = nullptr,
                                   bool restore_sources_on_failure_ = true)
{
    const auto throw_invalid_argument = [&] () {
        if (restore_sources_on_failure_)
            restore_single_send_part_to_source (state_);
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    };
    std::optional<socket_call_t> call;
    try {
        call.emplace (submit_handle (state_.raw.socket));
    }
    catch (...) {
        if (restore_sources_on_failure_)
            restore_async_send_sources (state_);
        throw;
    }
    const zlink_routing_id_t *first_rid = target_first_rid_native (state_.raw.target);

    if (state_.kind == operation_kind_t::raw_routed_send && !first_rid)
        throw_invalid_argument ();
    if (state_.kind == operation_kind_t::raw_publish && state_.raw.topic.empty ())
        throw_invalid_argument ();

    const auto submit_record = [&] (zlink_msg_t *parts_, size_t part_count_) {
        const auto flags =
          static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags));
        switch (state_.kind) {
            case operation_kind_t::raw_send:
                return zlink_send (call->handle (), parts_, part_count_, flags,
                                   user_context_, completion_id_out_);
            case operation_kind_t::raw_routed_send:
                return zlink_send_rid (call->handle (), first_rid, parts_, part_count_, flags,
                                       user_context_, completion_id_out_);
            case operation_kind_t::raw_publish:
                return zlink_publish (call->handle (), state_.raw.topic.c_str (),
                                      parts_, part_count_, flags);
            default:
                return ZLINK_SUBMIT_INVALID_ARGUMENT;
        }
    };

    // Keep multipart storage in the pooled operation state across submissions.
    const int raw_result =
      state_.message.single_part.has_value () || state_.message.single_part_source
        ? submit_borrowed_message_part (send_single_part (state_), submit_record)
        : submit_message_parts (state_.message.parts, submit_record);
    const int submit_errno = zlink_errno ();
    const auto result = static_cast<submit_result_t> (raw_result);

    if (result == submit_result_t::ok) {
        if (completion_id_out_ && *completion_id_out_ != 0)
            throw submit_error_t (submit_result_t::internal_error, EPROTO);
        return true;
    }

    if (restore_sources_on_failure_)
        restore_async_send_sources (state_);
    // DONTWAIT publish reports backpressure through its boolean terminal.
    if (!completion_id_out_ && state_.flags == send_flags_t::dontwait
        && result == submit_result_t::backpressured)
        return false;

    return submit_wait_or_throw (result, submit_errno,
      completion_id_out_ ? *completion_id_out_ : 0, state_, user_context_, [] {});
}

inline bool submit_raw_request_state (
  operation_state_t &state_, void *user_context_,
  zlink_completion_id_t *completion_id_out_,
  bool restore_sources_on_failure_ = true)
{
    const auto restore_sources = [&] () noexcept {
        if (restore_sources_on_failure_)
            restore_async_send_sources (state_);
    };
    const auto throw_invalid_argument = [&] () {
        restore_sources ();
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    };

    if ((state_.kind != operation_kind_t::raw_request
            && state_.kind != operation_kind_t::raw_routed_request)
        || !completion_id_out_)
        throw_invalid_argument ();
    std::optional<socket_call_t> call;
    try {
        call.emplace (submit_handle (state_.raw.socket));
    }
    catch (...) {
        restore_sources ();
        throw;
    }

    const zlink_routing_id_t *const target =
      state_.kind == operation_kind_t::raw_routed_request
        ? target_first_rid_native (state_.raw.target)
        : nullptr;
    if (state_.kind == operation_kind_t::raw_routed_request && !target)
        throw_invalid_argument ();

    const uint32_t timeout =
      state_.timeout > std::chrono::milliseconds::zero ()
        ? native_timeout_ms (state_.timeout)
        : 0u;
    *completion_id_out_ = 0;
    auto submit_record = [&] (zlink_msg_t *parts_, size_t part_count_) {
        return zlink_request (call->handle (), target, parts_, part_count_,
          static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
          timeout, user_context_, completion_id_out_);
    };

    int raw_result = -1;
    if (state_.message.single_part.has_value ()
        || state_.message.single_part_source) {
        message_t &part = send_single_part (state_);
        raw_result = submit_borrowed_message_part (part, submit_record);
    } else {
        raw_result = submit_message_parts (state_.message.parts, submit_record);
    }

    const int submit_errno = zlink_errno ();
    const submit_result_t result = static_cast<submit_result_t> (raw_result);
    if (result == submit_result_t::ok) {
        if (*completion_id_out_ == 0) {
            restore_sources ();
            throw submit_error_t (submit_result_t::internal_error, EPROTO);
        }
        return true;
    }

    return submit_wait_or_throw (result, submit_errno, *completion_id_out_,
                                 state_, user_context_, restore_sources);
}

} // namespace detail
} // namespace zlink

#endif
