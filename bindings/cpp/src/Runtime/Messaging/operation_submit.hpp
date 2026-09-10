/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_MESSAGING_OPERATION_SUBMIT_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_MESSAGING_OPERATION_SUBMIT_HPP_INCLUDED

#include "operation_state.hpp"
#include "operation_detail.hpp"
#include "../Core/duration_conversion.hpp"
#include "../Native/native_message_parts.hpp"

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
    if (!state_.raw.socket)
        throw_invalid_argument ();
    if (!share_runtime_state (state_.raw)) {
        if (restore_sources_on_failure_)
            restore_async_send_sources (state_);
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);
    }
    const zlink_routing_id_t *first_rid = target_first_rid_native (state_.raw.target);

    if (state_.kind == operation_kind_t::raw_routed_send && !first_rid)
        throw_invalid_argument ();
    if (state_.kind == operation_kind_t::raw_publish && state_.raw.topic.empty ())
        throw_invalid_argument ();

    if (state_.message.single_part.has_value () || state_.message.single_part_source) {
        message_t &part = send_single_part (state_);
        if (!part.valid ())
            throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

        const int direct_rc = zlink::detail::submit_borrowed_message_part (
          part, [&] (zlink_msg_t *parts_, size_t part_count_) {
              switch (state_.kind) {
                  case operation_kind_t::raw_send:
                      return zlink_send (
                        state_.raw.socket, parts_, part_count_,
                        static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
                        user_context_, completion_id_out_);
                  case operation_kind_t::raw_routed_send:
                      return zlink_send_rid (
                        state_.raw.socket, first_rid, parts_, part_count_,
                        static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
                        user_context_, completion_id_out_);
                  case operation_kind_t::raw_publish:
                      return zlink_publish (
                        state_.raw.socket, state_.raw.topic.c_str (), parts_, part_count_,
                        static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)));
                  default:
                      return ZLINK_SUBMIT_INVALID_ARGUMENT;
              }
          });

        const int submit_errno = zlink_errno ();
        if (direct_rc == -1) {
            if (restore_sources_on_failure_)
                restore_single_send_part_to_source (state_);
            throw submit_error_t (submit_result_from_errno (submit_errno), submit_errno);
        }
        const submit_result_t rc = static_cast<submit_result_t> (direct_rc);
        if (rc == submit_result_t::ok)
            return true;
        if (restore_sources_on_failure_)
            restore_single_send_part_to_source (state_);
        if (state_.flags == send_flags_t::dontwait && rc == submit_result_t::backpressured) {
            return false;
        }
        throw submit_error_t (rc, submit_errno);
    }

    // Multipart record staging, ownership recovery, and pooled capacity belong
    // to one operation state. Moving this vector into a call-local temporary
    // makes the terminal destroy its capacity after every record, even though
    // the state pool is intended to retain it for the next builder chain.
    std::vector<message_t> &parts = state_.message.parts;
    const int raw_rc = zlink::detail::submit_message_parts (
      parts, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
          switch (state_.kind) {
              case operation_kind_t::raw_send:
                  return zlink_send (
                    state_.raw.socket, native_parts_, part_count_,
                    static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
                    user_context_, completion_id_out_);
              case operation_kind_t::raw_routed_send:
                  return zlink_send_rid (
                    state_.raw.socket, first_rid, native_parts_, part_count_,
                    static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
                    user_context_, completion_id_out_);
              case operation_kind_t::raw_publish:
                  return zlink_publish (
                    state_.raw.socket, state_.raw.topic.c_str (), native_parts_, part_count_,
                    static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)));
              default:
                  return ZLINK_SUBMIT_INVALID_ARGUMENT;
          }
      });
    const int submit_errno = zlink_errno ();
    if (raw_rc == -1) {
        if (restore_sources_on_failure_)
            restore_send_parts_to_sources (state_, parts);
        throw submit_error_t (submit_result_from_errno (submit_errno), submit_errno);
    }
    const submit_result_t rc = static_cast<submit_result_t> (raw_rc);
    if (rc != submit_result_t::ok) {
        if (restore_sources_on_failure_)
            restore_send_parts_to_sources (state_, parts);
        if (state_.flags == send_flags_t::dontwait && rc == submit_result_t::backpressured) {
            return false;
        }
        throw submit_error_t (rc, submit_errno);
    }
    return true;
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

    if (!state_.raw.socket
        || (state_.kind != operation_kind_t::raw_request
            && state_.kind != operation_kind_t::raw_routed_request)
        || !completion_id_out_)
        throw_invalid_argument ();
    if (!share_runtime_state (state_.raw)) {
        restore_sources ();
        throw submit_error_t (submit_result_t::invalid_state, ESHUTDOWN);
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
        return zlink_request (
          state_.raw.socket, target, parts_, part_count_,
          static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
          timeout, user_context_, completion_id_out_);
    };

    int raw_result = -1;
    if (state_.message.single_part.has_value ()
        || state_.message.single_part_source) {
        message_t &part = send_single_part (state_);
        if (!part.valid ())
            throw_invalid_argument ();
        raw_result = submit_borrowed_message_part (part, submit_record);
    } else {
        raw_result = submit_message_parts (state_.message.parts, submit_record);
    }

    const int submit_errno = zlink_errno ();
    if (raw_result == -1) {
        restore_sources ();
        throw submit_error_t (submit_result_from_errno (submit_errno),
                              submit_errno);
    }

    const submit_result_t result = static_cast<submit_result_t> (raw_result);
    if (result == submit_result_t::ok) {
        if (*completion_id_out_ == 0) {
            restore_sources ();
            throw submit_error_t (submit_result_t::internal_error, EPROTO);
        }
        return true;
    }

    if (state_.flags == send_flags_t::dontwait
        && result == submit_result_t::backpressured
        && submit_errno == EAGAIN && *completion_id_out_ != 0)
        return false;

    restore_sources ();
    throw submit_error_t (result, submit_errno);
}

} // namespace detail
} // namespace zlink

#endif
