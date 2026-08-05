/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <Runtime/Core/duration_conversion.hpp>
#include "operation_detail.hpp"
#include "operation_submit.hpp"
#include "request_submitter.hpp"
#include "operation_state.hpp"

namespace zlink
{

namespace
{

bool is_raw_request_kind (detail::operation_kind_t kind_) noexcept
{
    return kind_ == detail::operation_kind_t::raw_request
           || kind_ == detail::operation_kind_t::raw_routed_request;
}

void ensure_raw_request_state (const detail::operation_state_t &state_)
{
    if (!state_.raw.socket)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    if (state_.kind != detail::operation_kind_t::raw_request
        && !detail::target_first_rid_native (state_.raw.target))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
}

zlink_submit_result_t submit_raw_request_part (detail::operation_state_t &state_,
                                               zlink_msg_t *part_out_,
                                               zlink_part_flag_t part_flag_,
                                               bool is_final_,
                                               zlink_send_flags_t flags_,
                                               detail::request_state_t *request_state_)
{
    const uint32_t timeout = is_final_ ? zlink::detail::native_timeout_ms (state_.timeout) : 0u;
    auto callback = is_final_ ? &detail::request_callback_trampoline : nullptr;
    void *userdata = is_final_ ? request_state_ : nullptr;
    const zlink_routing_id_t *first_rid =
      detail::target_first_rid_native (state_.raw.target);

    switch (state_.kind) {
        case detail::operation_kind_t::raw_request:
            return zlink_dealer_request_part (state_.raw.socket, part_out_, flags_, part_flag_,
                                              timeout, callback, userdata);
        case detail::operation_kind_t::raw_routed_request:
            return zlink_router_request_part (state_.raw.socket, first_rid, part_out_, flags_,
                                              part_flag_, timeout, callback, userdata);
        default:
            return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }
}

async_result_t<std::vector<message_t>>
submit_raw_request_awaitable (detail::operation_state_t &state_)
{
    ensure_raw_request_state (state_);

    // HOT PATH: a single-part public request stays in the operation state's
    // inline slot and reaches the native part API without materializing a
    // vector. Multipart requests retain the shared vector fallback below.
    if (detail::send_part_count (state_) == 1u) {
        return detail::submit_request_part_awaitable (
          detail::send_single_part (state_),
          zlink::detail::make_socket_request_progress (state_.raw.socket),
          [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_,
               zlink_reply_handler_fn callback_, void *request_state_) {
              return submit_raw_request_part (
                state_, part_out_, part_flag_, callback_ != nullptr, ZLINK_SEND_FLAGS_NONE,
                static_cast<detail::request_state_t *> (request_state_));
          });
    }

    return detail::submit_request_parts_awaitable (
      state_.message.parts, zlink::detail::make_socket_request_progress (state_.raw.socket),
      [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_, zlink_reply_handler_fn callback_,
           void *request_state_) {
          const bool is_final_ = callback_ != nullptr;
          return submit_raw_request_part (state_, part_out_, part_flag_, is_final_,
                                          ZLINK_SEND_FLAGS_NONE,
                                          static_cast<detail::request_state_t *> (request_state_));
      });
}

bool submit_raw_request_callback (detail::operation_state_t &state_,
                                  request_callback_t callback_)
{
    ensure_raw_request_state (state_);

    // HOT PATH: keep the common single-part callback request on the direct
    // native part path; only multipart requests need vector iteration.
    if (detail::send_part_count (state_) == 1u) {
        return detail::submit_request_part_callback (
          detail::send_single_part (state_), std::move (callback_), state_.flags,
          [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_,
               zlink_reply_handler_fn callback, void *request_state) {
              return submit_raw_request_part (
                state_, part_out_, part_flag_, callback != nullptr,
                static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
                static_cast<detail::request_state_t *> (request_state));
          });
    }

    return detail::submit_request_parts_callback (
      state_.message.parts, std::move (callback_), state_.flags,
      [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_, zlink_reply_handler_fn callback,
           void *request_state) {
          const bool is_final_ = callback != nullptr;
          return submit_raw_request_part (
            state_, part_out_, part_flag_, is_final_,
            static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
            static_cast<detail::request_state_t *> (request_state));
      });
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

request_submit_operation_t &&
request_submit_operation_t::timeout (std::chrono::milliseconds timeout_) &&
{
    state ().timeout = timeout_;
    return std::move (*this);
}

request_operation_t::~request_operation_t () = default;
request_operation_t::request_operation_t (request_operation_t &&) noexcept = default;
request_operation_t &request_operation_t::operator= (request_operation_t &&) noexcept = default;

request_submit_operation_t request_operation_t::message (message_t &part_) &&
{
    if (is_raw_request_kind (state ().kind))
        state ().message.single_part_source = &part_;
    else
        state ().message.parts.push_back (std::move (part_));
    return request_submit_operation_t (release_state_ptr ());
}

request_callback_submit_operation_t::~request_callback_submit_operation_t () = default;
request_callback_submit_operation_t::request_callback_submit_operation_t (
  request_callback_submit_operation_t &&) noexcept = default;
request_callback_submit_operation_t &request_callback_submit_operation_t::operator= (
  request_callback_submit_operation_t &&) noexcept = default;

request_callback_submit_operation_t &&
request_callback_submit_operation_t::message (message_t &part_) &&
{
    detail::append_send_part (state (), part_);
    return std::move (*this);
}

request_callback_submit_operation_t &&
request_callback_submit_operation_t::timeout (std::chrono::milliseconds timeout_) &&
{
    state ().timeout = timeout_;
    return std::move (*this);
}

request_callback_submit_operation_t &&request_callback_submit_operation_t::flags (int flags_) &&
{
    state ().flags = send_flags_t (flags_);
    return std::move (*this);
}

request_callback_submit_operation_t request_submit_operation_t::flags (int flags_) &&
{
    state ().flags = send_flags_t (flags_);
    return request_callback_submit_operation_t (release_state_ptr ());
}

async_result_t<std::vector<message_t>> request_submit_operation_t::async () &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    if (is_raw_request_kind (state.kind))
        return submit_raw_request_awaitable (state);

    throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
}

bool request_submit_operation_t::submit (request_callback_t callback_) &&
{
    request_callback_submit_operation_t ready (release_state_ptr ());
    return std::move (ready).submit (std::move (callback_));
}

bool request_callback_submit_operation_t::submit (request_callback_t callback_) &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    if (is_raw_request_kind (state.kind))
        return submit_raw_request_callback (state, std::move (callback_));

    throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
}

} // namespace zlink
