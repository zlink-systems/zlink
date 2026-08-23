/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/socket/api.h>
#include <Runtime/Core/duration_conversion.hpp>
#include <Runtime/Messaging/received_access.hpp>
#include "operation_detail.hpp"
#include "operation_submit.hpp"
#include "operation_state.hpp"
#include "async_operation_state.hpp"

#include <cerrno>
#include <memory>

namespace zlink
{

namespace
{

struct send_completion_anchor_t
{
    std::shared_ptr<detail::async_operation_state_t<void>> completion;
};

std::exception_ptr send_completion_failure (const zlink_send_complete_event_t &event_)
{
    const int error = event_.terminal_errno != 0
                        ? event_.terminal_errno
                        : (event_.result == ZLINK_SEND_TIMED_OUT ? ETIMEDOUT : EIO);
    // C++ has no second admission result domain. `not_admitted` is the
    // binding result for a Core operation that reached the completion lane
    // without becoming admitted; the Core errno preserves timeout/cancel/
    // close/route detail for callers.
    return std::make_exception_ptr (
      submit_error_t (submit_result_t::not_admitted, error));
}

void send_complete_trampoline (void * /*subject_*/,
                               const zlink_send_complete_event_t *event_,
                               void *userdata_) noexcept
{
    auto *const callbacks = static_cast<detail::socket_callback_state_t *> (userdata_);
    if (!callbacks || !event_)
        return;

    std::shared_ptr<void> opaque_anchor;
    {
        std::lock_guard<std::mutex> lock (callbacks->send_completion_mutex);
        const auto it = callbacks->send_completion_anchors.find (event_->userdata);
        if (it == callbacks->send_completion_anchors.end ())
            return;
        opaque_anchor = std::move (it->second);
        callbacks->send_completion_anchors.erase (it);
    }

    const auto anchor = std::static_pointer_cast<send_completion_anchor_t> (
      std::move (opaque_anchor));
    if (!anchor || !anchor->completion)
        return;
    if (event_->result == ZLINK_SEND_ADMITTED)
        (void) anchor->completion->complete ();
    else
        (void) anchor->completion->fail (send_completion_failure (*event_));
}

void ensure_send_completion_handler (detail::socket_callback_state_t &callbacks_,
                                     void *socket_)
{
    std::lock_guard<std::mutex> lock (callbacks_.send_completion_mutex);
    if (callbacks_.send_completion_handler_registered)
        return;

    const zlink_handler_result_t result = zlink_send_complete_handler (
      socket_, &send_complete_trampoline, &callbacks_);
    if (result != 0) {
        const int error = zlink_errno ();
        throw handler_error_t (detail::handler_result_from_errno (error), error);
    }
    callbacks_.send_completion_handler_registered = true;
}

void discard_send_completion_anchor (detail::socket_callback_state_t &callbacks_,
                                     void *key_) noexcept
{
    std::lock_guard<std::mutex> lock (callbacks_.send_completion_mutex);
    callbacks_.send_completion_anchors.erase (key_);
}

async_result_t<void> submit_send_async (detail::operation_state_t &state_)
{
    if (state_.kind != detail::operation_kind_t::raw_send
        && state_.kind != detail::operation_kind_t::raw_routed_send)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    if (state_.flags != send_flags_t::none)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    if (!state_.raw.socket)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    const std::shared_ptr<detail::socket_callback_state_t> callbacks =
      detail::share_callback_state (state_.raw);
    if (!callbacks || callbacks->socket_closed.load (std::memory_order_acquire))
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);

    zlink_routed_submit_target_t target{};
    const zlink_routed_submit_target_t *target_ptr = nullptr;
    if (state_.kind == detail::operation_kind_t::raw_routed_send) {
        const zlink_routing_id_t *const rid =
          detail::target_first_rid_native (state_.raw.target);
        if (!rid)
            throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
        const zlink_submit_result_t target_result =
          zlink_select_routed_submit_target (state_.raw.socket, rid, &target);
        if (target_result != ZLINK_SUBMIT_OK)
            throw submit_error_t (static_cast<submit_result_t> (target_result), zlink_errno ());
        target_ptr = &target;
    }

    const uint32_t timeout_ms = state_.timeout > std::chrono::milliseconds::zero ()
                                  ? detail::native_timeout_ms (state_.timeout)
                                  : 0u;
    auto completion = std::make_shared<detail::async_operation_state_t<void>> ();
    auto anchor = std::make_shared<send_completion_anchor_t> ();
    anchor->completion = completion;
    std::vector<message_t> parts = detail::take_send_parts (state_);
    if (parts.empty ())
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    try {
        ensure_send_completion_handler (*callbacks, state_.raw.socket);
        std::lock_guard<std::mutex> lock (callbacks->send_completion_mutex);
        callbacks->send_completion_anchors.emplace (anchor.get (), anchor);
    }
    catch (...) {
        detail::restore_send_parts_to_sources (state_, parts);
        throw;
    }

    zlink_send_async_options_t options{};
    options.struct_size = sizeof (options);
    options.timeout_ms = timeout_ms;
    options.userdata = anchor.get ();
    options.target = target_ptr;

    zlink_send_op_id_t op_id = 0;
    int result_errno = EINVAL;
    int raw_result = -1;
    try {
        raw_result = detail::with_moved_native_parts (
          parts, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
              const zlink_submit_result_t result = zlink_send_async (
                state_.raw.socket, native_parts_, part_count_, &options, &op_id);
              if (result != ZLINK_SUBMIT_OK)
                  detail::restore_parts_from_native (parts, native_parts_, part_count_);
              return static_cast<int> (result);
          });
        result_errno = zlink_errno ();
    }
    catch (...) {
        discard_send_completion_anchor (*callbacks, anchor.get ());
        detail::restore_send_parts_to_sources (state_, parts);
        throw;
    }

    if (raw_result != ZLINK_SUBMIT_OK) {
        discard_send_completion_anchor (*callbacks, anchor.get ());
        detail::restore_send_parts_to_sources (state_, parts);
        const submit_result_t result = raw_result == -1
                                         ? detail::submit_result_from_errno (result_errno)
                                         : static_cast<submit_result_t> (raw_result);
        throw submit_error_t (result, result_errno);
    }
    if (op_id == 0) {
        // Core promises a non-zero id for every accepted operation. Surface an
        // impossible contract as a binding failure instead of installing an
        // un-cancellable awaitable.
        discard_send_completion_anchor (*callbacks, anchor.get ());
        throw submit_error_t (submit_result_t::internal_error, EPROTO);
    }

    completion->set_cancel ([callbacks, socket = state_.raw.socket, op_id] () {
        return zlink_send_async_cancel (socket, op_id) == ZLINK_SUBMIT_OK;
    });
    return detail::async_result_access_t::make<void> (std::move (completion));
}

} // namespace

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
    detail::append_send_part (state (), std::move (part_));
    return std::move (*this);
}

send_submit_operation_t &&send_submit_operation_t::flags (int flags_) &&
{
    state ().flags = send_flags_t (flags_);
    return std::move (*this);
}

send_submit_operation_t &&
send_submit_operation_t::timeout (std::chrono::milliseconds timeout_) &&
{
    state ().timeout = timeout_;
    return std::move (*this);
}

send_operation_t::~send_operation_t () = default;
send_operation_t::send_operation_t (send_operation_t &&) noexcept = default;
send_operation_t &send_operation_t::operator= (send_operation_t &&) noexcept = default;

routed_send_submit_operation_t::~routed_send_submit_operation_t () = default;
routed_send_submit_operation_t::routed_send_submit_operation_t (
  routed_send_submit_operation_t &&) noexcept = default;
routed_send_submit_operation_t &routed_send_submit_operation_t::operator= (
  routed_send_submit_operation_t &&) noexcept = default;

routed_send_operation_t::~routed_send_operation_t () = default;
routed_send_operation_t::routed_send_operation_t (routed_send_operation_t &&) noexcept = default;
routed_send_operation_t &
routed_send_operation_t::operator= (routed_send_operation_t &&) noexcept = default;

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
    return send_submit_operation_t (release_state_ptr ());
}

routed_send_submit_operation_t &&
routed_send_submit_operation_t::message (message_t &part_) &&
{
    detail::append_send_part (state (), part_);
    return std::move (*this);
}

routed_send_submit_operation_t &&
routed_send_submit_operation_t::message (message_t &&part_) &&
{
    detail::append_send_part (state (), std::move (part_));
    return std::move (*this);
}

routed_send_submit_operation_t
routed_send_operation_t::message (message_t &part_) &&
{
    state ().message.single_part_source = &part_;
    if (!detail::can_borrow_single_send_part (state ().kind))
        state ().message.single_part.emplace (std::move (part_));
    return routed_send_submit_operation_t (release_state_ptr ());
}

routed_send_submit_operation_t
routed_send_operation_t::message (message_t &&part_) &&
{
    state ().message.single_part.emplace (std::move (part_));
    state ().message.single_part_source = nullptr;
    return routed_send_submit_operation_t (release_state_ptr ());
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

void routed_send_submit_operation_t::submit () &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    // Core owns the HWM contract for this submit: it blocks and resumes on its
    // own signal, bounds the wait with SNDTIMEO, and reports backpressure. The
    // routed builder exposes no flags stage, so the socket options are the
    // single owner of that policy.
    (void) detail::submit_raw_send_state (state);
}

async_result_t<void> send_submit_operation_t::async () &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    return submit_send_async (state);
}

routed_send_submit_operation_t &&
routed_send_submit_operation_t::timeout (std::chrono::milliseconds timeout_) &&
{
    state ().timeout = timeout_;
    return std::move (*this);
}

async_result_t<void> routed_send_submit_operation_t::async () &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    return submit_send_async (state);
}

publish_submit_operation_t::~publish_submit_operation_t () = default;
publish_submit_operation_t::publish_submit_operation_t (
  publish_submit_operation_t &&) noexcept = default;
publish_submit_operation_t &publish_submit_operation_t::operator= (
  publish_submit_operation_t &&) noexcept = default;

publish_submit_operation_t &&publish_submit_operation_t::message (message_t &part_) &&
{
    detail::append_send_part (state (), part_);
    return std::move (*this);
}

publish_submit_operation_t &&publish_submit_operation_t::message (message_t &&part_) &&
{
    detail::append_send_part (state (), std::move (part_));
    return std::move (*this);
}

publish_submit_operation_t &&publish_submit_operation_t::flags (int flags_) &&
{
    state ().flags = send_flags_t (flags_);
    return std::move (*this);
}

bool publish_submit_operation_t::submit () &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state)
        || state.kind != detail::operation_kind_t::raw_publish)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    return detail::submit_raw_send_state (state);
}

publish_operation_t::~publish_operation_t () = default;
publish_operation_t::publish_operation_t (publish_operation_t &&) noexcept = default;
publish_operation_t &publish_operation_t::operator= (publish_operation_t &&) noexcept = default;

publish_submit_operation_t publish_operation_t::message (message_t &part_) &&
{
    state ().message.single_part_source = &part_;
    return publish_submit_operation_t (release_state_ptr ());
}

publish_submit_operation_t publish_operation_t::message (message_t &&part_) &&
{
    state ().message.single_part.emplace (std::move (part_));
    state ().message.single_part_source = nullptr;
    return publish_submit_operation_t (release_state_ptr ());
}

} // namespace zlink
