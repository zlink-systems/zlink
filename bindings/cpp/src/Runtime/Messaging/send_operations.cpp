/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/socket/api.h>
#include <Runtime/Core/duration_conversion.hpp>
#include <Runtime/Messaging/received_access.hpp>
#include "operation_detail.hpp"
#include "operation_submit.hpp"
#include "operation_state.hpp"
#include "async_operation_state.hpp"

#include <atomic>
#include <cerrno>
#include <memory>

namespace zlink
{

namespace
{

struct send_completion_anchor_t
{
    explicit send_completion_anchor_t (
      std::shared_ptr<detail::async_operation_state_t<void>> completion_) :
        completion (std::move (completion_))
    {
    }

    void release () noexcept
    {
        if (references.fetch_sub (1, std::memory_order_acq_rel) == 1)
            delete this;
    }

    // One reference belongs to submit_send_async until it has interpreted
    // op_id. The other belongs to Core only if the operation becomes pending;
    // on immediate admission or submit failure the submitter releases both.
    std::atomic<unsigned int> references{2};
    std::shared_ptr<detail::async_operation_state_t<void>> completion;
};

void release_anchor_without_callback (send_completion_anchor_t *anchor_) noexcept
{
    if (!anchor_)
        return;
    anchor_->release ();
    anchor_->release ();
}

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
                               void * /*userdata_*/) noexcept
{
    if (!event_)
        return;
    send_completion_anchor_t *const anchor =
      static_cast<send_completion_anchor_t *> (event_->userdata);
    if (!anchor || !anchor->completion)
        return;
    if (event_->result == ZLINK_SEND_ADMITTED)
        (void) anchor->completion->complete ();
    else
        (void) anchor->completion->fail (send_completion_failure (*event_));
    anchor->release ();
}

void ensure_send_completion_handler (detail::socket_callback_state_t &callbacks_,
                                     void *socket_)
{
    detail::ensure_async_continuation_dispatcher ();
    if (callbacks_.send_completion_handler_registered.load (
          std::memory_order_acquire))
        return;

    std::lock_guard<std::mutex> lock (callbacks_.send_completion_mutex);
    if (callbacks_.send_completion_handler_registered.load (
          std::memory_order_relaxed))
        return;

    const zlink_handler_result_t result = zlink_send_complete_handler (
      socket_, &send_complete_trampoline, &callbacks_);
    if (result != 0) {
        const int error = zlink_errno ();
        throw handler_error_t (detail::handler_result_from_errno (error), error);
    }
    callbacks_.send_completion_handler_registered.store (
      true, std::memory_order_release);
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
    if (!callbacks)
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);

    zlink_routed_submit_target_t target{};
    const zlink_routed_submit_target_t *target_ptr = nullptr;
    if (state_.kind == detail::operation_kind_t::raw_routed_send) {
        const zlink_routing_id_t *const rid =
          detail::target_first_rid_native (state_.raw.target);
        if (!rid)
            throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
        // A peer-only target asks zlink_send_async to snapshot the exact pair
        // in the same admission call. This preserves exact-pair FIFO while
        // avoiding a second Core boundary on every routed message.
        target.peer_rid = *rid;
        target_ptr = &target;
    }

    const uint32_t timeout_ms = state_.timeout > std::chrono::milliseconds::zero ()
                                  ? detail::native_timeout_ms (state_.timeout)
                                  : 0u;
    auto completion = std::make_shared<detail::async_operation_state_t<void>> ();
    const bool direct_single_part =
      state_.message.single_part.has_value ()
      || state_.message.single_part_source != nullptr;
    message_t *single_part = direct_single_part
                               ? &detail::send_single_part (state_)
                               : nullptr;
    std::vector<message_t> parts;
    if (single_part) {
        if (!single_part->valid ()) {
            detail::restore_single_send_part_to_source (state_);
            throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
        }
    } else {
        parts = detail::take_send_parts (state_);
        if (parts.empty ())
            throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    }

    send_completion_anchor_t *anchor = nullptr;
    try {
        ensure_send_completion_handler (*callbacks, state_.raw.socket);
        anchor = new send_completion_anchor_t (completion);
    }
    catch (...) {
        detail::restore_send_parts_to_sources (state_, parts);
        throw;
    }

    zlink_send_async_options_t options{};
    options.struct_size = sizeof (options);
    options.timeout_ms = timeout_ms;
    options.userdata = anchor;
    options.target = target_ptr;

    zlink_send_op_id_t op_id = 0;
    int result_errno = EINVAL;
    int raw_result = -1;
    try {
        const auto submit_native =
          [&] (zlink_msg_t *native_parts_, size_t part_count_) {
              const zlink_submit_result_t result = zlink_send_async (
                state_.raw.socket, native_parts_, part_count_, &options, &op_id);
              if (result != ZLINK_SUBMIT_OK && !single_part)
                  detail::restore_parts_from_native (parts, native_parts_, part_count_);
              return static_cast<int> (result);
          };
        if (single_part) {
            raw_result = detail::submit_one_message_part (
              *single_part,
              [&] (zlink_msg_t *native_part_, zlink_part_flag_t) {
                  return submit_native (native_part_, 1);
              });
        } else {
            raw_result = detail::with_moved_native_parts (parts, submit_native);
        }
        result_errno = zlink_errno ();
    }
    catch (...) {
        release_anchor_without_callback (anchor);
        if (single_part)
            detail::restore_single_send_part_to_source (state_);
        else
            detail::restore_send_parts_to_sources (state_, parts);
        throw;
    }

    if (raw_result != ZLINK_SUBMIT_OK) {
        release_anchor_without_callback (anchor);
        if (single_part)
            detail::restore_single_send_part_to_source (state_);
        else
            detail::restore_send_parts_to_sources (state_, parts);
        const submit_result_t result = raw_result == -1
                                         ? detail::submit_result_from_errno (result_errno)
                                         : static_cast<submit_result_t> (raw_result);
        throw submit_error_t (result, result_errno);
    }
    if (op_id == 0) {
        // Immediate admission has no Core completion record or callback. Drop
        // the callback anchor and complete locally so co_await observes the
        // ready fast path without suspension or callback re-entry.
        (void) completion->complete ();
        release_anchor_without_callback (anchor);
        return detail::async_result_access_t::make<void> (
          std::move (completion));
    }

    completion->set_cancel ([callbacks, socket = state_.raw.socket, op_id] () {
        return zlink_send_async_cancel (socket, op_id) == ZLINK_SUBMIT_OK;
    });
    // Drop the submitter reference. The Core callback may already have
    // released its reference if completion raced the return path.
    anchor->release ();
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
            try {
                const bool sent =
                  zlink::detail::received_access_t::submit_send (
                    *state.received.received, parts, state.flags);
                if (!sent)
                    detail::restore_send_parts_to_sources (state, parts);
                return sent;
            }
            catch (...) {
                detail::restore_send_parts_to_sources (state, parts);
                throw;
            }
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
