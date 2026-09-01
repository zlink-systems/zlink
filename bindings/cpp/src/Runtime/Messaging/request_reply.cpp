/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <Runtime/Core/duration_conversion.hpp>
#include "operation_detail.hpp"
#include "operation_submit.hpp"
#include "operation_state.hpp"
#include "async_operation_state.hpp"
#include <Runtime/Options/socket_options_detail.hpp>

#include <cerrno>
#include <condition_variable>
#include <optional>

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

int request_result_errno (request_result_t result_) noexcept
{
    switch (result_) {
        case request_result_t::timed_out:
            return ETIMEDOUT;
        case request_result_t::not_found:
            return ENOENT;
        case request_result_t::terminated:
            return ETERM;
        case request_result_t::protocol_error:
            return EPROTO;
        case request_result_t::rejected:
            return EACCES;
        case request_result_t::conflict:
            return ESTALE;
        case request_result_t::busy:
            return EBUSY;
        case request_result_t::not_connected:
            return ENOTCONN;
        case request_result_t::invalid_argument:
            return EINVAL;
        case request_result_t::invalid_state:
            return EFSM;
        case request_result_t::not_supported:
            return ENOTSUP;
        default:
            return EIO;
    }
}

// Bridges the Core reply handler callback to the suspension. Core drives the
// completion; the binding owns no retry queue, timer, or worker for it. The
// suspension is resumed in the context Core delivered the reply on.
struct managed_request_bridge_t
{
    std::shared_ptr<detail::async_operation_state_t<std::vector<message_t>>> completion;
    request_callback_t callback;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<std::vector<message_t>> value;
    std::exception_ptr failure;
    request_result_t result = request_result_t::internal_error;
    bool armed = false;
    bool blocking = false;
    bool terminal = false;
    bool delivered = false;

    void finish (zlink_request_result_t result_,
                 zlink_msg_t *parts_, size_t part_count_) noexcept
    {
        std::optional<std::vector<message_t>> result_parts;
        std::exception_ptr result_failure;
        request_result_t result_kind = request_result_t::internal_error;
        if (result_ != ZLINK_REQUEST_OK) {
            detail::close_message_array (parts_, part_count_);
            result_kind = static_cast<request_result_t> (result_);
            result_failure = std::make_exception_ptr (
              request_error_t (result_kind, request_result_errno (result_kind)));
        } else {
            result_kind = request_result_t::ok;
            try {
                result_parts.emplace (
                  detail::take_parts_from_native (parts_, part_count_));
            }
            catch (...) {
                detail::close_message_array (parts_, part_count_);
                result_failure = std::current_exception ();
                result_kind = request_result_t::internal_error;
            }
        }

        bool deliver = false;
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (terminal)
                return;
            value = std::move (result_parts);
            failure = std::move (result_failure);
            result = result_kind;
            terminal = true;
            deliver = armed;
        }
        if (deliver)
            deliver_terminal ();
    }

    std::vector<message_t> wait ()
    {
        std::unique_lock<std::mutex> lock (mutex);
        changed.wait (lock, [this] { return armed && terminal; });
        std::exception_ptr result_failure = failure;
        std::optional<std::vector<message_t>> result_parts = std::move (value);
        lock.unlock ();
        if (result_failure)
            std::rethrow_exception (result_failure);
        if (!result_parts)
            throw request_error_t (request_result_t::internal_error, EIO);
        return std::move (*result_parts);
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
        request_callback_t result_callback;
        request_result_t result_kind = request_result_t::internal_error;
        bool notify_blocking = false;
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (!armed || !terminal || delivered)
                return;
            delivered = true;
            if (blocking) {
                notify_blocking = true;
            } else {
                result_parts = std::move (value);
                result_callback = std::move (callback);
            }
            result_failure = failure;
            result_kind = result;
        }
        if (notify_blocking) {
            changed.notify_all ();
            return;
        }
        if (result_callback) {
            try {
                if (result_failure || !result_parts)
                    result_callback (result_kind, {});
                else
                    result_callback (result_kind, std::move (*result_parts));
            }
            catch (...) {
                // A C callback cannot propagate an application exception back
                // through Core. Completion remains exactly once; the callback
                // owns successful parts and is responsible for closing them.
            }
            return;
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

using async_request_completion_t = detail::async_request_operation_state_t;

void release_async_request_completion (void *userdata_) noexcept
{
    auto *completion = static_cast<async_request_completion_t *> (userdata_);
    if (completion)
        completion->release_from_core ();
}

void delete_managed_request_bridge_ref (void *userdata_) noexcept
{
    delete static_cast<std::shared_ptr<managed_request_bridge_t> *> (userdata_);
}

void async_request_trampoline (zlink_request_result_t result_,
                               zlink_msg_t *parts_, size_t part_count_,
                               void *userdata_)
{
    auto *completion = static_cast<async_request_completion_t *> (userdata_);
    if (!completion) {
        detail::close_message_array (parts_, part_count_);
        return;
    }
    if (result_ != ZLINK_REQUEST_OK) {
        detail::close_message_array (parts_, part_count_);
        const request_result_t result_kind = static_cast<request_result_t> (result_);
        completion->fail (std::make_exception_ptr (
          request_error_t (result_kind, request_result_errno (result_kind))));
    } else {
        try {
            completion->complete (detail::take_parts_from_native (parts_, part_count_));
        }
        catch (...) {
            detail::close_message_array (parts_, part_count_);
            completion->fail (std::current_exception ());
        }
    }
    completion->release_from_core ();
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

// Submits the request part sequence on the calling thread. A DEALER leaves
// target selection to Core at submit time; a ROUTER snapshots the exact
// transport pair for its explicit peer. Core owns the send-side HWM wait
// (SNDTIMEO) and the reply deadline through ZLINK_REQUEST_TIMED_OUT. The
// caller chooses whether the bridge is consumed by a coroutine, a blocking
// caller, or an application callback.
void submit_raw_request (detail::operation_state_t &state_,
                         zlink_reply_handler_fn reply_handler_, void *reply_userdata_,
                         void (*delete_userdata_) (void *) noexcept)
{
    ensure_raw_request_state (state_);
    if (!reply_handler_ || !reply_userdata_ || !delete_userdata_)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    detail::socket_callback_state_t *const callbacks =
      detail::live_callback_state (state_.raw);
    if (!callbacks)
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);

    const bool dealer = state_.kind == detail::operation_kind_t::raw_request;
    const zlink_routing_id_t *const router_rid =
      dealer ? nullptr : detail::target_first_rid_native (state_.raw.target);
    const uint32_t timeout = detail::native_timeout_ms (
      resolved_request_timeout (state_, dealer));

    submit_result_t result = submit_result_t::internal_error;
    int result_errno = EINVAL;
    {
        std::optional<zlink_routed_submit_target_t> target;
        if (!dealer)
            target.emplace (
              select_routed_submit_target (state_.raw.socket, router_rid));

        std::vector<message_t> parts;
        bool multipart = false;
        int raw_rc = -1;
        errno = 0;
        try {
            if (state_.message.single_part.has_value ()
                || state_.message.single_part_source) {
                // request().message(part) is by far the common public path.
                // Submit a one-part borrowed native view directly instead of
                // allocating a vector and routing it through the generic
                // multipart adapter. Core consumes the supplied native part
                // on both success and failure, so it must never receive the
                // caller's handle: failure leaves the original untouched,
                // while success closes that original shared reference before
                // marking the public message consumed.
                message_t &part = detail::send_single_part (state_);
                if (!part.valid ())
                    throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
                zlink_msg_t native_view;
                const bool native_view_initialized = zlink_msg_init (&native_view) == 0;
                if (!native_view_initialized
                    || zlink_msg_copy (&native_view, detail::native_handle (part)) != 0) {
                    if (native_view_initialized)
                        (void) zlink_msg_close (&native_view);
                } else {
                    if (dealer) {
                        raw_rc = zlink_dealer_request_part (
                          state_.raw.socket, &native_view,
                          static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
                          ZLINK_PART_FINAL, timeout,
                          reply_handler_, reply_userdata_);
                    } else {
                        raw_rc = zlink_router_request_transport_pair_part (
                          state_.raw.socket, &target->peer_rid,
                          target->transport_pair_id,
                          target->transport_pair_generation,
                          &native_view,
                          static_cast<zlink_send_flags_t> (static_cast<int> (state_.flags)),
                          ZLINK_PART_FINAL, timeout,
                          reply_handler_, reply_userdata_);
                    }
                    (void) zlink_msg_close (&native_view);
                    if (raw_rc == ZLINK_SUBMIT_OK) {
                        detail::message_access_t::close_noexcept (part);
                        detail::mark_sent (part);
                    }
                }
            } else {
                multipart = true;
                parts = detail::take_send_parts (state_);
                raw_rc = detail::submit_borrowed_message_array (
                  parts, [&] (zlink_msg_t *native_parts_, size_t part_count_) {
                      size_t failed_index = 0;
                      return detail::submit_native_parts (
                        native_parts_, part_count_, failed_index,
                        [&] (zlink_msg_t *part_, zlink_part_flag_t part_flag_,
                             bool is_final_) {
                            zlink_reply_handler_fn handler =
                              is_final_ ? reply_handler_ : nullptr;
                            void *userdata = is_final_ ? reply_userdata_ : nullptr;
                            if (dealer) {
                                return zlink_dealer_request_part (
                                  state_.raw.socket, part_,
                                  static_cast<zlink_send_flags_t> (
                                    static_cast<int> (state_.flags)), part_flag_,
                                  is_final_ ? timeout : 0u, handler, userdata);
                            }
                            return zlink_router_request_transport_pair_part (
                              state_.raw.socket, &target->peer_rid,
                              target->transport_pair_id,
                              target->transport_pair_generation, part_,
                              static_cast<zlink_send_flags_t> (
                                static_cast<int> (state_.flags)), part_flag_,
                              is_final_ ? timeout : 0u, handler, userdata);
                        });
                  });
            }
        }
        catch (...) {
            delete_userdata_ (reply_userdata_);
            if (multipart)
                detail::restore_send_parts_to_sources (state_, parts);
            throw;
        }
        result_errno = zlink_errno ();
        if (raw_rc == ZLINK_SUBMIT_OK) {
            result = submit_result_t::ok;
        } else {
            delete_userdata_ (reply_userdata_);
            if (multipart)
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
    // the selected bridge, so there is no pending binding record to cancel.
}

async_result_t<std::vector<message_t>>
submit_raw_request_awaitable (detail::operation_state_t &state_)
{
    const auto completion = std::make_shared<async_request_completion_t> ();
    completion->retain_for_core (completion);
    submit_raw_request (state_, &async_request_trampoline, completion.get (),
                        &release_async_request_completion);
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
    return request_submit_operation_t (release_state_ptr ());
}

request_submit_operation_t &&request_submit_operation_t::flags (int flags_) &&
{
    state ().flags = send_flags_t (flags_);
    return std::move (*this);
}

async_result_t<std::vector<message_t>> request_submit_operation_t::async () &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    if (state.flags != send_flags_t::none)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    if (is_raw_request_kind (state.kind))
        return submit_raw_request_awaitable (state);

    throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
}

std::vector<message_t> request_submit_operation_t::submit () &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    if (!is_raw_request_kind (state.kind))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    const auto bridge = std::make_shared<managed_request_bridge_t> ();
    bridge->blocking = true;
    // Register the blocking consumer before Core can deliver an inline reply.
    // `arm()` rechecks `terminal` while holding the bridge mutex, so a reply
    // that wins the race with this registration is still delivered exactly
    // once and remains visible to the predicate in `wait()`.
    bridge->arm ();
    auto *bridge_ref = new std::shared_ptr<managed_request_bridge_t> (bridge);
    submit_raw_request (state, &managed_request_trampoline, bridge_ref,
                        &delete_managed_request_bridge_ref);
    return bridge->wait ();
}

bool request_submit_operation_t::submit (request_callback_t callback_) &&
{
    auto &state = this->state ();
    if (!detail::has_send_parts (state))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    if (!is_raw_request_kind (state.kind))
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);
    if (!callback_)
        throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

    const auto bridge = std::make_shared<managed_request_bridge_t> ();
    bridge->callback = std::move (callback_);
    bridge->arm ();
    auto *bridge_ref = new std::shared_ptr<managed_request_bridge_t> (bridge);
    submit_raw_request (state, &managed_request_trampoline, bridge_ref,
                        &delete_managed_request_bridge_ref);
    return true;
}

} // namespace zlink
