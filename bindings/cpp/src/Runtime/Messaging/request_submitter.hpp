/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_MESSAGING_REQUEST_SUBMITTER_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_MESSAGING_REQUEST_SUBMITTER_HPP_INCLUDED

#include "operation_detail.hpp"

namespace zlink
{
namespace detail
{

template <typename SubmitPart>
async_result_t<std::vector<message_t>> submit_request_part_awaitable (
  message_t &part_, std::function<void ()> progress_, SubmitPart submit_part_)
{
    if (!part_.valid ())
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    std::unique_ptr<request_state_t> state (make_future_request_state ());
    std::future<std::vector<message_t>> future = state->promise->get_future ();

    zlink_msg_t native;
    move_to_native_or_reject (part_, &native);

    const submit_result_t rc = static_cast<submit_result_t> (
      submit_part_ (&native, ZLINK_PART_FINAL, &request_callback_trampoline, state.get ()));
    if (rc != submit_result_t::ok) {
        const int err = zlink_errno ();
        zlink::detail::restore_part_from_native (part_, native);
        throw submit_error_t (rc, err);
    }

    state.release ();
    return async_result_t<std::vector<message_t>> (std::move (future), std::move (progress_));
}

template <typename SubmitPart>
bool submit_request_part_callback (message_t &part_,
                                   request_callback_t callback_,
                                   send_flags_t flags_,
                                   SubmitPart submit_part_)
{
    if (!part_.valid ())
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    std::unique_ptr<request_state_t> state (make_callback_request_state (std::move (callback_)));

    zlink_msg_t native;
    move_to_native_or_reject (part_, &native);

    const submit_result_t rc = static_cast<submit_result_t> (
      submit_part_ (&native, ZLINK_PART_FINAL, &request_callback_trampoline, state.get ()));
    if (rc != submit_result_t::ok) {
        const int err = zlink_errno ();
        zlink::detail::restore_part_from_native (part_, native);
        if (flags_ == send_flags_t::dontwait && rc == submit_result_t::backpressured)
            return false;
        throw submit_error_t (rc, err);
    }

    state.release ();
    return true;
}

template <typename SubmitPart>
async_result_t<std::vector<message_t>> submit_request_parts_awaitable (
  std::vector<message_t> &parts_, std::function<void ()> progress_, SubmitPart submit_part_)
{
    std::unique_ptr<request_state_t> state (make_future_request_state ());
    std::future<std::vector<message_t>> future = state->promise->get_future ();

    const int raw_rc = submit_message_parts_close_on_failure (
      parts_, [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_, bool is_final_) {
          // The reply handler belongs to the FINAL part alone: that submission is the one
          // that builds the request spec, and the core rejects a non-final part carrying a
          // handler with EINVAL.
          return submit_part_ (part_out_, part_flag_,
                               is_final_ ? &request_callback_trampoline : NULL,
                               is_final_ ? state.get () : NULL);
      });
    if (raw_rc == -1)
        throw last_error ();
    const submit_result_t rc = static_cast<submit_result_t> (raw_rc);
    if (rc != submit_result_t::ok)
        throw submit_error_t (rc, zlink_errno ());

    state.release ();
    return async_result_t<std::vector<message_t>> (std::move (future), std::move (progress_));
}

template <typename SubmitPart>
bool submit_request_parts_callback (std::vector<message_t> &parts_,
                                    request_callback_t callback_,
                                    send_flags_t flags_,
                                    SubmitPart submit_part_)
{
    // HOT PATH: callback request submission must stay separate from the
    // awaitable path. Do not route this through std::promise/std::future; the
    // native completion callback already owns the reply vector handoff.
    std::unique_ptr<request_state_t> state (make_callback_request_state (std::move (callback_)));

    const int raw_rc = submit_message_parts_close_on_failure (
      parts_, [&] (zlink_msg_t *part_out_, zlink_part_flag_t part_flag_, bool is_final_) {
          // See submit_request_parts_awaitable: only the FINAL part may carry the handler.
          return submit_part_ (part_out_, part_flag_,
                               is_final_ ? &request_callback_trampoline : NULL,
                               is_final_ ? state.get () : NULL);
      });
    if (raw_rc == -1)
        throw last_error ();
    const submit_result_t rc = static_cast<submit_result_t> (raw_rc);
    if (rc != submit_result_t::ok) {
        if (flags_ == send_flags_t::dontwait && rc == submit_result_t::backpressured)
            return false;
        throw submit_error_t (rc, zlink_errno ());
    }

    state.release ();
    return true;
}

} // namespace detail
} // namespace zlink

#endif
