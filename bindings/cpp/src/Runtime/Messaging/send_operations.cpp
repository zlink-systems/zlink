/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <Runtime/Messaging/received_access.hpp>
#include "operation_detail.hpp"
#include "operation_submit.hpp"
#include "operation_state.hpp"
#include "async_operation_state.hpp"
#include "routed_admission_state.hpp"
#include "publish_admission_state.hpp"
#include <Runtime/Options/socket_options_detail.hpp>

#include <cerrno>

namespace zlink
{

namespace
{

struct managed_send_attempt_t
{
    void *socket = nullptr;
    std::shared_ptr<detail::socket_callback_state_t> callbacks;
    zlink_routed_submit_target_t target{};
    bool dealer = false;
    bool router = false;
    std::vector<message_t> parts;

    detail::routed_attempt_result_t attempt ()
    {
        errno = 0;
        std::unique_lock<std::mutex> attempt_lock (
          callbacks->outbound_record_attempt_mutex);
        if (callbacks->socket_closed.load (std::memory_order_acquire)) {
            return {submit_result_t::terminated, ETERM};
        }
        const int raw_rc = detail::submit_borrowed_message_array (
          parts, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
              size_t failed_index = 0;
              return detail::submit_native_parts (
                native_parts_, part_count_, failed_index,
                [&] (zlink_msg_t *part_, zlink_part_flag_t part_flag_, bool) {
                    if (dealer) {
                        return zlink_dealer_send_transport_pair_part (
                          socket, &target, part_,
                          ZLINK_SEND_FLAGS_DONTWAIT, part_flag_);
                    }
                    if (router) {
                        return zlink_send_part_transport_pair (
                          socket, &target.peer_rid,
                          target.transport_pair_id,
                          target.transport_pair_generation, part_,
                          ZLINK_SEND_FLAGS_DONTWAIT, part_flag_);
                    }
                    return ZLINK_SUBMIT_INVALID_ARGUMENT;
                });
          });
        const int result_errno = zlink_errno ();
        if (raw_rc == -1)
            return {submit_result_t::internal_error, result_errno};
        return {static_cast<submit_result_t> (raw_rc), result_errno};
    }
};

struct managed_send_start_t
{
    detail::routed_admission_ticket_t ticket;
};

struct managed_publish_attempt_t
{
    void *socket = nullptr;
    std::shared_ptr<detail::socket_callback_state_t> callbacks;
    std::string topic;
    std::vector<message_t> parts;

    detail::publish_attempt_result_t attempt ()
    {
        errno = 0;
        const int raw_rc = detail::submit_borrowed_message_array (
          parts, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
              std::lock_guard<std::mutex> attempt_lock (
                callbacks->outbound_record_attempt_mutex);
              if (callbacks->socket_closed.load (std::memory_order_acquire)) {
                  errno = ETERM;
                  return static_cast<int> (ZLINK_SUBMIT_TERMINATED);
              }
              size_t failed_index = 0;
              return detail::submit_native_parts (
                native_parts_, part_count_, failed_index,
                [&] (zlink_msg_t *part_, zlink_part_flag_t part_flag_, bool) {
                    return zlink_publish_part (
                      socket, topic.c_str (), part_, ZLINK_SEND_FLAGS_DONTWAIT,
                      part_flag_);
                });
          });
        const int result_errno = zlink_errno ();
        if (raw_rc == -1)
            return {submit_result_t::internal_error, result_errno};
        return {static_cast<submit_result_t> (raw_rc), result_errno};
    }
};

struct managed_publish_start_t
{
    detail::publish_admission_ticket_t ticket;
};

managed_send_start_t start_managed_send (
  detail::operation_state_t &state_,
  const std::shared_ptr<detail::async_operation_state_t<void>> &completion_)
{
    const auto operation_started = std::chrono::steady_clock::now ();
    const std::shared_ptr<detail::socket_callback_state_t> callbacks =
      detail::share_callback_state (state_.raw);
    if (!state_.raw.socket)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    if (!callbacks || callbacks->socket_closed.load (std::memory_order_acquire))
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);
    const bool dealer = state_.kind == detail::operation_kind_t::raw_send;
    const bool router =
      state_.kind == detail::operation_kind_t::raw_routed_send;
    const zlink_routing_id_t *router_rid =
      router ? detail::target_first_rid_native (state_.raw.target) : nullptr;
    if ((!dealer && !router) || (router && !router_rid))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    detail::ensure_native_send_ready_handler (
      state_.raw.socket, *callbacks);
    const std::shared_ptr<detail::routed_admission_state_t> admission =
      detail::ensure_routed_admission_state (state_.raw.socket,
                                             *callbacks);
    zlink_routed_submit_target_t target{};
    {
        std::lock_guard<std::mutex> attempt_lock (
          callbacks->outbound_record_attempt_mutex);
        if (callbacks->socket_closed.load (std::memory_order_acquire))
            throw submit_error_t (submit_result_t::terminated, ETERM);
        target = detail::select_routed_submit_target (
          state_.raw.socket, router ? router_rid : nullptr);
    }
    const int socket_timeout = detail::get_typed_option_value<int> (
      state_.raw.socket, detail::socket_option_id::sndtimeo);
    const auto configured_timeout = state_.timeout_explicit
                                      ? state_.timeout.count ()
                                      : socket_timeout;
    const auto deadline = configured_timeout < 0
                            ? std::chrono::steady_clock::time_point::max ()
                            : operation_started
                                + std::chrono::milliseconds (configured_timeout);

    std::shared_ptr<managed_send_attempt_t> operation =
      std::make_shared<managed_send_attempt_t> ();
    operation->socket = state_.raw.socket;
    operation->callbacks = callbacks;
    operation->target = target;
    operation->dealer = dealer;
    operation->router = router;
    operation->parts = detail::take_send_parts (state_);
    try {
        const detail::routed_admission_ticket_t ticket =
          detail::enqueue_routed_admission (
            admission, target,
            [operation] { return operation->attempt (); },
            [operation, completion_] {
                operation->parts.clear ();
                completion_->complete ();
            },
            [operation, completion_] (submit_result_t result_, int error_) {
                operation->parts.clear ();
                completion_->fail (std::make_exception_ptr (
                  submit_error_t (result_, error_)));
            },
            deadline, configured_timeout == 0);
        return {ticket};
    }
    catch (...) {
        detail::restore_single_send_part_to_source (state_, operation->parts);
        throw;
    }
}

managed_publish_start_t start_managed_publish (
  detail::operation_state_t &state_,
  const std::shared_ptr<detail::async_operation_state_t<void>> &completion_)
{
    const auto started = std::chrono::steady_clock::now ();
    const auto callbacks = detail::share_callback_state (state_.raw);
    if (!state_.raw.socket)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    if (!callbacks || callbacks->socket_closed.load (std::memory_order_acquire))
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);
    detail::ensure_native_send_ready_handler (
      state_.raw.socket, *callbacks);
    const int socket_timeout = detail::get_typed_option_value<int> (
      state_.raw.socket, detail::socket_option_id::sndtimeo);
    const auto timeout = state_.timeout_explicit ? state_.timeout.count ()
                                                 : socket_timeout;
    const auto deadline = timeout < 0
                            ? std::chrono::steady_clock::time_point::max ()
                            : started + std::chrono::milliseconds (timeout);
    auto operation = std::make_shared<managed_publish_attempt_t> ();
    operation->socket = state_.raw.socket;
    operation->callbacks = callbacks;
    operation->topic = state_.raw.topic;
    operation->parts = detail::take_send_parts (state_);
    try {
        const auto ticket = detail::enqueue_publish_admission (
          detail::ensure_publish_admission_state (*callbacks),
          [operation] { return operation->attempt (); },
          [operation, completion_] {
              operation->parts.clear ();
              completion_->complete ();
          },
          [operation, completion_] (submit_result_t result_, int error_) {
              operation->parts.clear ();
              completion_->fail (std::make_exception_ptr (
                submit_error_t (result_, error_)));
          },
          deadline);
        return {ticket};
    }
    catch (...) {
        detail::restore_single_send_part_to_source (state_, operation->parts);
        throw;
    }
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
    state ().message.single_part.emplace (std::move (part_));
    state ().message.single_part_source = nullptr;
    state ().message.discard_single_part_on_backpressure = true;
    return std::move (*this);
}

send_submit_operation_t &&send_submit_operation_t::timeout (
  std::chrono::milliseconds timeout_) &&
{
    state ().timeout = timeout_;
    state ().timeout_explicit = true;
    return std::move (*this);
}

send_submit_operation_t &&send_submit_operation_t::flags (int flags_) &&
{
    state ().flags = send_flags_t (flags_);
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
    state ().message.discard_single_part_on_backpressure = true;
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
    state ().message.single_part.emplace (std::move (part_));
    state ().message.single_part_source = nullptr;
    state ().message.discard_single_part_on_backpressure = true;
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
    state ().message.discard_single_part_on_backpressure = true;
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

async_result_t<void> send_submit_operation_t::async () &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state)) {
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    }
    if (state.flags != send_flags_t::none) {
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    }
    const auto completion =
      std::make_shared<detail::async_operation_state_t<void>> ();
    if (state.kind == detail::operation_kind_t::raw_send
        || state.kind == detail::operation_kind_t::raw_routed_send) {
        const managed_send_start_t start = start_managed_send (state, completion);
        completion->set_cancel ([ticket = start.ticket] { return ticket.cancel (); });
    } else if (state.kind == detail::operation_kind_t::raw_publish) {
        const managed_publish_start_t start =
          start_managed_publish (state, completion);
        completion->set_cancel ([ticket = start.ticket] { return ticket.cancel (); });
    } else {
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    }
    return detail::async_result_access_t::make<void> (completion);
}

async_result_t<void> routed_send_submit_operation_t::async () &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    if (state.flags != send_flags_t::none)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    const auto completion =
      std::make_shared<detail::async_operation_state_t<void>> ();
    const managed_send_start_t start = start_managed_send (state, completion);
    completion->set_cancel ([ticket = start.ticket] { return ticket.cancel (); });
    return detail::async_result_access_t::make<void> (completion);
}

} // namespace zlink
