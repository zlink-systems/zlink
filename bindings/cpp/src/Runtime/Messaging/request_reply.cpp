/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <Runtime/Core/duration_conversion.hpp>
#include "operation_detail.hpp"
#include "operation_submit.hpp"
#include "operation_state.hpp"
#include "async_operation_state.hpp"
#include <Runtime/Options/socket_options_detail.hpp>

#include <cerrno>

namespace zlink
{

namespace
{

void ensure_raw_request_state (const detail::operation_state_t &state_);

// Policy-free snapshot of one exact routed target. It claims no credit; the
// request submit that follows still observes the Core HWM contract.
zlink_routed_submit_target_t select_routed_submit_target (
  void *socket_, const zlink_routing_id_t *router_rid_or_null_)
{
    zlink_routed_submit_target_t target{};
    const zlink_submit_result_t rc =
      zlink_select_routed_submit_target (socket_, router_rid_or_null_, &target);
    if (rc != ZLINK_SUBMIT_OK)
        throw submit_error_t (static_cast<submit_result_t> (rc), zlink_errno ());
    return target;
}

request_result_t request_result_from_submit (submit_result_t result_,
                                             int error_) noexcept
{
    if (error_ == ETIMEDOUT)
        return request_result_t::timed_out;
    if (error_ == ECANCELED || result_ == submit_result_t::terminated)
        return request_result_t::terminated;
    switch (result_) {
        case submit_result_t::not_found:
            return request_result_t::not_found;
        case submit_result_t::not_connected:
            return request_result_t::not_connected;
        case submit_result_t::invalid_argument:
            return request_result_t::invalid_argument;
        case submit_result_t::not_supported:
            return request_result_t::not_supported;
        case submit_result_t::invalid_state:
            return request_result_t::invalid_state;
        default:
            return request_result_t::internal_error;
    }
}

// Bridges the Core reply handler callback to the suspension. Core drives the
// completion; the binding owns no retry queue, timer, or worker for it. The
// suspension is resumed in the context Core delivered the reply on.
struct managed_request_bridge_t
{
    std::shared_ptr<detail::async_operation_state_t<std::vector<message_t>>> completion;
    std::mutex mutex;
    std::optional<std::vector<message_t>> value;
    std::exception_ptr failure;
    bool armed = false;
    bool terminal = false;
    bool delivered = false;

    void finish (zlink_request_result_t result_,
                 zlink_msg_t *parts_, size_t part_count_) noexcept
    {
        std::optional<std::vector<message_t>> result_parts;
        std::exception_ptr result_failure;
        if (result_ != ZLINK_REQUEST_OK) {
            detail::close_message_array (parts_, part_count_);
            result_failure = std::make_exception_ptr (
              request_error_t (static_cast<request_result_t> (result_)));
        } else {
            try {
                result_parts.emplace (
                  detail::take_parts_from_native (parts_, part_count_));
            }
            catch (...) {
                detail::close_message_array (parts_, part_count_);
                result_failure = std::current_exception ();
            }
        }

        bool deliver = false;
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (terminal)
                return;
            value = std::move (result_parts);
            failure = std::move (result_failure);
            terminal = true;
            deliver = armed;
        }
        if (deliver)
            deliver_terminal ();
    }

    void arm () noexcept
    {
        bool deliver = false;
        {
            std::lock_guard<std::mutex> lock (mutex);
            armed = true;
            deliver = terminal;
        }
        if (deliver)
            deliver_terminal ();
    }

  private:
    void deliver_terminal () noexcept
    {
        std::optional<std::vector<message_t>> result_parts;
        std::exception_ptr result_failure;
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (!armed || !terminal || delivered)
                return;
            delivered = true;
            result_parts = std::move (value);
            result_failure = failure;
        }
        const auto target = completion;
        if (result_failure) {
            target->fail (std::move (result_failure));
            return;
        }
        if (!result_parts) {
            target->fail (std::make_exception_ptr (
              request_error_t (request_result_t::internal_error)));
            return;
        }
        target->complete (std::move (*result_parts));
    }
};

void managed_request_trampoline (zlink_request_result_t result_,
                                 zlink_msg_t *parts_, size_t part_count_,
                                 void *userdata_)
{
    std::unique_ptr<std::shared_ptr<managed_request_bridge_t>> bridge_ref (
      static_cast<std::shared_ptr<managed_request_bridge_t> *> (userdata_));
    if (!bridge_ref || !*bridge_ref) {
        detail::close_message_array (parts_, part_count_);
        return;
    }
    (*bridge_ref)->finish (result_, parts_, part_count_);
}

std::chrono::milliseconds resolved_request_timeout (
  const detail::operation_state_t &state_, bool dealer_)
{
    if (state_.timeout > std::chrono::milliseconds::zero ())
        return state_.timeout;
    const int configured = dealer_
                             ? detail::get_typed_option_value<int> (
                                 state_.raw.socket,
                                 detail::dealer_option_id::request_timeout_ms)
                             : detail::get_typed_option_value<int> (
                                 state_.raw.socket,
                                 detail::router_option_id::request_timeout_ms);
    return std::chrono::milliseconds (configured > 0 ? configured : 5000);
}

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

// Submits the request part sequence to one exact Core target on the calling
// thread. Core owns the send-side HWM wait (SNDTIMEO) exactly as it does for a
// routed send, and owns the reply deadline through ZLINK_REQUEST_TIMED_OUT.
async_result_t<std::vector<message_t>>
submit_raw_request_awaitable (detail::operation_state_t &state_)
{
    ensure_raw_request_state (state_);
    detail::socket_callback_state_t *const callbacks =
      detail::live_callback_state (state_.raw);
    if (!callbacks || callbacks->socket_closed.load (std::memory_order_acquire))
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);

    const bool dealer = state_.kind == detail::operation_kind_t::raw_request;
    const zlink_routing_id_t *const router_rid =
      dealer ? nullptr : detail::target_first_rid_native (state_.raw.target);
    const uint32_t timeout = detail::native_timeout_ms (
      resolved_request_timeout (state_, dealer));

    const auto completion = std::make_shared<
      detail::async_operation_state_t<std::vector<message_t>>> ();
    const auto bridge = std::make_shared<managed_request_bridge_t> ();
    bridge->completion = completion;

    submit_result_t result = submit_result_t::internal_error;
    int result_errno = EINVAL;
    {
        std::lock_guard<std::mutex> attempt_lock (
          callbacks->outbound_record_attempt_mutex);
        if (callbacks->socket_closed.load (std::memory_order_acquire))
            throw submit_error_t (submit_result_t::terminated, ETERM);

        const zlink_routed_submit_target_t target =
          select_routed_submit_target (state_.raw.socket, router_rid);

        std::vector<message_t> parts = detail::take_send_parts (state_);
        auto *bridge_ref =
          new std::shared_ptr<managed_request_bridge_t> (bridge);
        int raw_rc = -1;
        errno = 0;
        try {
            raw_rc = detail::submit_borrowed_message_array (
              parts, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
                  size_t failed_index = 0;
                  return detail::submit_native_parts (
                    native_parts_, part_count_, failed_index,
                    [&] (zlink_msg_t *part_, zlink_part_flag_t part_flag_,
                         bool is_final_) {
                        zlink_reply_handler_fn handler =
                          is_final_ ? &managed_request_trampoline : nullptr;
                        void *userdata = is_final_ ? bridge_ref : nullptr;
                        if (dealer) {
                            return zlink_dealer_request_transport_pair_part (
                              state_.raw.socket, &target, part_,
                              ZLINK_SEND_FLAGS_NONE, part_flag_,
                              is_final_ ? timeout : 0u, handler, userdata);
                        }
                        return zlink_router_request_transport_pair_part (
                          state_.raw.socket, &target.peer_rid,
                          target.transport_pair_id,
                          target.transport_pair_generation, part_,
                          ZLINK_SEND_FLAGS_NONE, part_flag_,
                          is_final_ ? timeout : 0u, handler, userdata);
                    });
              });
        }
        catch (...) {
            delete bridge_ref;
            // The submit adapter borrows the parts, so every one of them is
            // still intact here; hand the caller-owned ones back before the
            // builder recycles the state and drops `parts`.
            detail::restore_send_parts_to_sources (state_, parts);
            throw;
        }
        result_errno = zlink_errno ();
        if (raw_rc == ZLINK_SUBMIT_OK) {
            result = submit_result_t::ok;
        } else {
            delete bridge_ref;
            detail::restore_send_parts_to_sources (state_, parts);
            result = raw_rc == -1 ? submit_result_t::internal_error
                                  : static_cast<submit_result_t> (raw_rc);
        }
    }

    if (result != submit_result_t::ok) {
        const request_result_t request_result =
          request_result_from_submit (result, result_errno);
        if (result_errno == ETIMEDOUT || result_errno == ECANCELED) {
            throw request_error_t (request_result, result_errno);
        }
        throw submit_error_t (result, result_errno);
    }

    // Core owns the request from here: the reply handler callback completes
    // the suspension, so there is no pending binding record to cancel.
    bridge->arm ();
    return detail::async_result_access_t::make<std::vector<message_t>> (
      completion);
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
        detail::append_send_part_from (state (), part_, &part_);
    return request_submit_operation_t (release_state_ptr ());
}

request_submit_operation_t request_operation_t::message (message_t &&part_) &&
{
    state ().message.single_part.emplace (std::move (part_));
    state ().message.single_part_source = nullptr;
    state ().message.discard_single_part_on_backpressure = true;
    return request_submit_operation_t (release_state_ptr ());
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

} // namespace zlink
