/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_MESSAGING_OPERATION_SUBMIT_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_MESSAGING_OPERATION_SUBMIT_HPP_INCLUDED

#include "operation_state.hpp"
#include "operation_detail.hpp"
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
          part, [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_) {
              switch (state_.kind) {
                  case operation_kind_t::raw_send:
                      return zlink_send_part (
                        state_.raw.socket, part_out_,
                        static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
                        part_flag_, user_context_, completion_id_out_);
                  case operation_kind_t::raw_routed_send:
                      return zlink_send_part_rid (
                        state_.raw.socket, first_rid, part_out_,
                        static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
                        part_flag_, user_context_, completion_id_out_);
                  case operation_kind_t::raw_publish:
                      return zlink_publish_part (
                        state_.raw.socket, state_.raw.topic.c_str (), part_out_,
                        static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
                        part_flag_);
                  default:
                      return ZLINK_SUBMIT_INVALID_ARGUMENT;
              }
          });

        const submit_result_t rc = static_cast<submit_result_t> (direct_rc);
        if (rc == submit_result_t::ok)
            return true;
        if (restore_sources_on_failure_)
            restore_single_send_part_to_source (state_);
        if (state_.flags == send_flags_t::dontwait && rc == submit_result_t::backpressured) {
            return false;
        }
        throw submit_error_t (rc, zlink_errno ());
    }

    // Multipart record staging, ownership recovery, and pooled capacity belong
    // to one operation state. Moving this vector into a call-local temporary
    // makes the terminal destroy its capacity after every record, even though
    // the state pool is intended to retain it for the next builder chain.
    std::vector<message_t> &parts = state_.message.parts;
    const int raw_rc = zlink::detail::submit_message_parts (
      parts, [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_, bool is_final_) {
          void *const context = is_final_ ? user_context_ : nullptr;
          zlink_completion_id_t *const completion_id =
            is_final_ ? completion_id_out_ : nullptr;
          switch (state_.kind) {
              case operation_kind_t::raw_send:
                  return zlink_send_part (
                    state_.raw.socket, part_out_,
                    static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)), part_flag_,
                    context, completion_id);
              case operation_kind_t::raw_routed_send:
                  return zlink_send_part_rid (
                    state_.raw.socket, first_rid, part_out_,
                    static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)), part_flag_,
                    context, completion_id);
              case operation_kind_t::raw_publish:
                  return zlink_publish_part (
                    state_.raw.socket, state_.raw.topic.c_str (), part_out_,
                    static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)), part_flag_);
              default:
                  return ZLINK_SUBMIT_INVALID_ARGUMENT;
          }
      });
    if (raw_rc == -1) {
        if (restore_sources_on_failure_)
            restore_send_parts_to_sources (state_, parts);
        throw last_error ();
    }
    const submit_result_t rc = static_cast<submit_result_t> (raw_rc);
    if (rc != submit_result_t::ok) {
        if (restore_sources_on_failure_)
            restore_send_parts_to_sources (state_, parts);
        if (state_.flags == send_flags_t::dontwait && rc == submit_result_t::backpressured) {
            return false;
        }
        throw submit_error_t (rc, zlink_errno ());
    }
    return true;
}

} // namespace detail
} // namespace zlink

#endif
