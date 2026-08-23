/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <Runtime/Core/duration_conversion.hpp>
#include "operation_detail.hpp"
#include "operation_submit.hpp"
#include "operation_state.hpp"
#include "async_operation_state.hpp"
#include "routed_admission_state.hpp"
#include <Runtime/Options/socket_options_detail.hpp>

#include <cerrno>

namespace zlink
{

namespace
{

void ensure_raw_request_state (const detail::operation_state_t &state_);

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
        try {
            detail::post_routed_completion (
              [target, result_parts = std::move (result_parts),
               result_failure = std::move (result_failure)] () mutable {
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
              });
        }
        catch (...) {
            target->fail (std::current_exception ());
        }
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

struct managed_request_attempt_t final : detail::routed_admission_request_t
{
    void *socket = nullptr;
    std::shared_ptr<detail::socket_callback_state_t> callbacks;
    zlink_routed_submit_target_t target{};
    bool dealer = false;
    std::chrono::steady_clock::time_point deadline_at;
    std::vector<message_t> parts;
    std::shared_ptr<detail::async_operation_state_t<std::vector<message_t>>> completion;

    detail::routed_attempt_result_t attempt () override
    {
        const auto now = std::chrono::steady_clock::now ();
        if (now >= deadline_at)
            return {submit_result_t::backpressured, ETIMEDOUT};
        const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds> (deadline_at - now);
        const uint32_t timeout = detail::native_timeout_ms (
          remaining.count () > 0 ? remaining : std::chrono::milliseconds (1));

        int raw_rc = -1;
        errno = 0;
        std::unique_lock<std::mutex> attempt_lock (
          callbacks->outbound_record_attempt_mutex);
        if (callbacks->socket_closed.load (std::memory_order_acquire))
            return {submit_result_t::terminated, ETERM};
        std::shared_ptr<managed_request_bridge_t> bridge =
          std::make_shared<managed_request_bridge_t> ();
        bridge->completion = completion;
        auto *bridge_ref =
          new std::shared_ptr<managed_request_bridge_t> (bridge);
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
                              socket, &target, part_,
                              ZLINK_SEND_FLAGS_DONTWAIT, part_flag_,
                              is_final_ ? timeout : 0u, handler, userdata);
                        }
                        return zlink_router_request_transport_pair_part (
                          socket, &target.peer_rid,
                          target.transport_pair_id,
                          target.transport_pair_generation, part_,
                          ZLINK_SEND_FLAGS_DONTWAIT, part_flag_,
                          is_final_ ? timeout : 0u, handler,
                          userdata);
                    });
              });
        }
        catch (...) {
            delete bridge_ref;
            throw;
        }
        const int result_errno = zlink_errno ();
        if (raw_rc == ZLINK_SUBMIT_OK)
            bridge->arm ();
        else
            delete bridge_ref;
        if (raw_rc == -1)
            return {submit_result_t::internal_error, result_errno};
        return {static_cast<submit_result_t> (raw_rc), result_errno};
    }

    // The reply is delivered by the armed bridge, so admission only has to
    // release the request parts once Core owns them.
    void accepted () override { parts.clear (); }

    void terminal (submit_result_t result_, int error_) override
    {
        parts.clear ();
        const request_result_t request_result =
          request_result_from_submit (result_, error_);
        if (error_ == ETIMEDOUT || error_ == ECANCELED) {
            completion->fail (std::make_exception_ptr (
              request_error_t (request_result, error_)));
        } else {
            completion->fail (
              std::make_exception_ptr (submit_error_t (result_, error_)));
        }
    }

    std::chrono::steady_clock::time_point deadline () override
    {
        return deadline_at;
    }
};

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

struct managed_request_start_t
{
    detail::routed_admission_ticket_t ticket;
};

managed_request_start_t start_managed_request (
  detail::operation_state_t &state_,
  const std::shared_ptr<detail::async_operation_state_t<std::vector<message_t>>> &completion_)
{
    const auto operation_started = std::chrono::steady_clock::now ();
    ensure_raw_request_state (state_);
    const std::shared_ptr<detail::socket_callback_state_t> callbacks =
      detail::share_callback_state (state_.raw);
    if (!callbacks || callbacks->socket_closed.load (std::memory_order_acquire))
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);
    const bool dealer = state_.kind == detail::operation_kind_t::raw_request;
    const zlink_routing_id_t *router_rid = dealer
                                             ? nullptr
                                             : detail::target_first_rid_native (
                                                 state_.raw.target);
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
          state_.raw.socket, dealer ? nullptr : router_rid);
    }
    const std::chrono::milliseconds timeout =
      resolved_request_timeout (state_, dealer);
    const auto deadline = operation_started + timeout;

    std::shared_ptr<managed_request_attempt_t> operation =
      std::make_shared<managed_request_attempt_t> ();
    operation->socket = state_.raw.socket;
    operation->callbacks = callbacks;
    operation->target = target;
    operation->dealer = dealer;
    operation->deadline_at = deadline;
    operation->completion = completion_;
    operation->parts = detail::take_send_parts (state_);
    try {
        return {detail::enqueue_routed_admission (admission, target, operation)};
    }
    catch (...) {
        detail::restore_single_send_part_to_source (state_, operation->parts);
        throw;
    }
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

async_result_t<std::vector<message_t>>
submit_raw_request_awaitable (detail::operation_state_t &state_)
{
    ensure_raw_request_state (state_);
    const auto completion = std::make_shared<
      detail::async_operation_state_t<std::vector<message_t>>> ();
    const managed_request_start_t start =
      start_managed_request (state_, completion);
    // A request Core already accepted has no pending admission record left to
    // cancel; its reply is owned by the armed bridge.
    if (start.ticket.valid ())
        completion->set_cancel ([ticket = start.ticket] { return ticket.cancel (); });
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
    state ().message.single_part.emplace (std::move (part_));
    state ().message.single_part_source = nullptr;
    state ().message.discard_single_part_on_backpressure = true;
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
