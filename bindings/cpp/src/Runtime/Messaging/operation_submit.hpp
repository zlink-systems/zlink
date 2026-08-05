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

inline bool submit_raw_send_state (operation_state_t &state_)
{
    const auto throw_invalid_argument = [&] () {
        restore_single_send_part_to_source (state_);
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    };
    if (!state_.raw.socket)
        throw_invalid_argument ();
    const zlink_routing_id_t *first_rid = target_first_rid_native (state_.raw.target);

    if (state_.kind == operation_kind_t::raw_routed_send && !first_rid)
        throw_invalid_argument ();
    if (state_.kind == operation_kind_t::raw_publish && state_.raw.topic.empty ())
        throw_invalid_argument ();

    if (state_.message.single_part.has_value () || state_.message.single_part_source) {
        message_t &part = send_single_part (state_);
        if (!part.valid ())
            throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

        zlink_submit_result_t direct_rc = ZLINK_SUBMIT_INVALID_ARGUMENT;
        switch (state_.kind) {
            case operation_kind_t::raw_send:
                direct_rc = zlink_send_part (
                  state_.raw.socket, zlink::detail::native_handle (part),
                  static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
                  ZLINK_PART_FINAL);
                break;
            case operation_kind_t::raw_routed_send:
                direct_rc = zlink_send_part_rid (
                  state_.raw.socket, first_rid, zlink::detail::native_handle (part),
                  static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
                  ZLINK_PART_FINAL);
                break;
            case operation_kind_t::raw_publish:
                direct_rc = zlink_publish_part (
                  state_.raw.socket, state_.raw.topic.c_str (), zlink::detail::native_handle (part),
                  static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
                  ZLINK_PART_FINAL);
                break;
            default:
                break;
        }

        const submit_result_t rc = static_cast<submit_result_t> (direct_rc);
        if (rc == submit_result_t::ok) {
            zlink::detail::mark_sent (part);
            return true;
        }
        restore_single_send_part_to_source (state_);
        if (state_.flags == send_flags_t::dontwait && rc == submit_result_t::backpressured) {
            return false;
        }
        throw submit_error_t (rc, zlink_errno ());
    }

    std::vector<message_t> parts = take_send_parts (state_);
    const int raw_rc = zlink::detail::submit_message_parts (
      parts, [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_, bool) {
          switch (state_.kind) {
              case operation_kind_t::raw_send:
                  return zlink_send_part (
                    state_.raw.socket, part_out_,
                    static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)), part_flag_);
              case operation_kind_t::raw_routed_send:
                  return zlink_send_part_rid (
                    state_.raw.socket, first_rid, part_out_,
                    static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)), part_flag_);
              case operation_kind_t::raw_publish:
                  return zlink_publish_part (
                    state_.raw.socket, state_.raw.topic.c_str (), part_out_,
                    static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)), part_flag_);
              default:
                  return ZLINK_SUBMIT_INVALID_ARGUMENT;
          }
      });
    if (raw_rc == -1)
        throw last_error ();
    const submit_result_t rc = static_cast<submit_result_t> (raw_rc);
    if (rc != submit_result_t::ok) {
        restore_send_parts_to_state (state_, parts);
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
