/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "message.hpp"
#include "request_result.hpp"
#include "../Sockets/results.hpp"
#include "operation_builder_base.hpp"

#include <chrono>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace zlink
{

namespace detail
{
using async_continuation_scheduler_t =
  std::function<void (std::function<void ()>)>;

template <typename T> class async_result_state_t
{
  public:
    virtual ~async_result_state_t () = default;
    virtual bool ready () const noexcept = 0;
    virtual bool suspend (std::coroutine_handle<> continuation_,
                          async_continuation_scheduler_t scheduler_) = 0;
    virtual T take () = 0;
    virtual bool cancel () noexcept = 0;
    virtual void abandon (std::coroutine_handle<> continuation_) noexcept = 0;
};

template <> class async_result_state_t<void>
{
  public:
    virtual ~async_result_state_t () = default;
    virtual bool ready () const noexcept = 0;
    virtual bool suspend (std::coroutine_handle<> continuation_,
                          async_continuation_scheduler_t scheduler_) = 0;
    virtual void take () = 0;
    virtual bool cancel () noexcept = 0;
    virtual void abandon (std::coroutine_handle<> continuation_) noexcept = 0;
};

struct async_result_access_t;
} // namespace detail

/// A move-only, single-consumer completion object for a binding operation.
/// Awaiting suspends the coroutine without occupying the calling thread. If an
/// unconsumed result or a suspended awaiter is destroyed, the binding requests
/// cancellation of the operation. A request already accepted by Core may still
/// run to its Core-owned terminal event so native reply resources can be released.
template <typename T> class async_result_t
{
  private:
    class awaiter_t
    {
      public:
        explicit awaiter_t (std::shared_ptr<detail::async_result_state_t<T>> state_) :
            _state (std::move (state_))
        {
        }

        awaiter_t (const awaiter_t &) = delete;
        awaiter_t &operator= (const awaiter_t &) = delete;
        awaiter_t (awaiter_t &&) = delete;
        awaiter_t &operator= (awaiter_t &&) = delete;

        ~awaiter_t ()
        {
            if (_state && _continuation)
                _state->abandon (_continuation);
        }

        bool await_ready () const noexcept { return _state->ready (); }

        template <typename TPromise>
        bool await_suspend (std::coroutine_handle<TPromise> continuation_)
        {
            _continuation = continuation_;
            detail::async_continuation_scheduler_t scheduler;
            if constexpr (requires (TPromise &promise_) {
                              detail::async_continuation_scheduler_t (
                                promise_.zlink_continuation_scheduler ());
                          }) {
                scheduler = detail::async_continuation_scheduler_t (
                  continuation_.promise ().zlink_continuation_scheduler ());
            }

            // A completion may resume and destroy this awaiter before suspend()
            // returns. Keep the state alive locally and do not touch members after
            // publishing the continuation.
            const auto state = _state;
            return state->suspend (continuation_, std::move (scheduler));
        }

        T await_resume () { return _state->take (); }

      private:
        std::shared_ptr<detail::async_result_state_t<T>> _state;
        std::coroutine_handle<> _continuation{};
    };

  public:
    async_result_t (async_result_t &&other_) noexcept :
        _state (std::move (other_._state))
    {
    }

    async_result_t &operator= (async_result_t &&other_) noexcept
    {
        if (this != &other_) {
            if (_state)
                (void) _state->cancel ();
            _state = std::move (other_._state);
        }
        return *this;
    }

    async_result_t (const async_result_t &) = delete;
    async_result_t &operator= (const async_result_t &) = delete;

    ~async_result_t ()
    {
        if (_state)
            (void) _state->cancel ();
    }

    /// Requests cancellation before Core admission. Returns false when another
    /// terminal event already won or Core owns the admitted request lifecycle.
    bool cancel () noexcept { return _state && _state->cancel (); }

    awaiter_t operator co_await () &&
    {
        if (!_state)
            throw std::logic_error ("async result has no operation");
        return awaiter_t (std::move (_state));
    }

    awaiter_t operator co_await () & = delete;

  private:
    explicit async_result_t (std::shared_ptr<detail::async_result_state_t<T>> state_) :
        _state (std::move (state_))
    {
    }

    std::shared_ptr<detail::async_result_state_t<T>> _state;

    friend struct detail::async_result_access_t;
};

namespace detail
{
struct async_result_access_t
{
    template <typename T>
    static async_result_t<T>
    make (std::shared_ptr<async_result_state_t<T>> state_)
    {
        return async_result_t<T> (std::move (state_));
    }
};
} // namespace detail

class dealer_socket_t;
class pair_socket_t;
class pub_socket_t;
class received_t;
class router_socket_t;
class stream_socket_t;
class xpub_socket_t;

namespace detail
{
struct operation_state_t;
} // namespace detail

/// @brief Accepts further parts, flags, and the terminal submit of a send builder.
/// @note Parts are consumed on a successful submit (see @ref send_operation_t).
class send_submit_operation_t : private detail::operation_builder_base_t<
                                  detail::operation_state_t,
                                  detail::pooled_operation_state_policy_t>
{
    using base_t = detail::operation_builder_base_t<detail::operation_state_t,
                                                    detail::pooled_operation_state_policy_t>;

  public:
    ~send_submit_operation_t ();
    send_submit_operation_t (send_submit_operation_t &&) noexcept;
    send_submit_operation_t &operator= (send_submit_operation_t &&) noexcept;

    send_submit_operation_t &&message (message_t &part_) &&;
    send_submit_operation_t &&message (message_t &&part_) &&;
    send_submit_operation_t &&flags (int flags_) &&;
    send_submit_operation_t &&timeout (std::chrono::milliseconds timeout_) &&;
    bool submit () &&;
    async_result_t<void> async () &&;
  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class send_operation_t;
};

/// @brief Accepts further parts and starts an exact-target routed send.
/// @note This stage deliberately exposes only async(); DEALER/ROUTER sends do
///       not have a second blocking, callback, flags, or progress terminal.
class routed_send_submit_operation_t : private detail::operation_builder_base_t<
                                         detail::operation_state_t,
                                         detail::pooled_operation_state_policy_t>
{
    using base_t = detail::operation_builder_base_t<detail::operation_state_t,
                                                    detail::pooled_operation_state_policy_t>;

  public:
    ~routed_send_submit_operation_t ();
    routed_send_submit_operation_t (routed_send_submit_operation_t &&) noexcept;
    routed_send_submit_operation_t &
    operator= (routed_send_submit_operation_t &&) noexcept;

    routed_send_submit_operation_t &&message (message_t &part_) &&;
    routed_send_submit_operation_t &&message (message_t &&part_) &&;

    /// Starts event-driven exact-target admission. The returned result owns
    /// the pending record and suspends a coroutine without occupying a caller
    /// or runtime worker thread while HWM credit is unavailable.
    async_result_t<void> async () &&;

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class routed_send_operation_t;
};

/// @brief Builds a DEALER/ROUTER send whose only terminal is async().
class routed_send_operation_t : private detail::operation_builder_base_t<
                                  detail::operation_state_t,
                                  detail::pooled_operation_state_policy_t>
{
    using base_t = detail::operation_builder_base_t<detail::operation_state_t,
                                                    detail::pooled_operation_state_policy_t>;

  public:
    ~routed_send_operation_t ();
    routed_send_operation_t (routed_send_operation_t &&) noexcept;
    routed_send_operation_t &operator= (routed_send_operation_t &&) noexcept;

    routed_send_submit_operation_t message (message_t &part_) &&;
    routed_send_submit_operation_t message (message_t &&part_) &&;

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class zlink::dealer_socket_t;
    friend class zlink::router_socket_t;
};

/**
 * @brief Builds a multipart send: add one or more parts, then submit().
 * @note Submitting consumes the added message_t parts; on success each part is
 *       moved into the transport and left invalid; on failure ownership returns to the caller.
 */
class send_operation_t : private detail::operation_builder_base_t<
                           detail::operation_state_t,
                           detail::pooled_operation_state_policy_t>
{
    using base_t = detail::operation_builder_base_t<detail::operation_state_t,
                                                    detail::pooled_operation_state_policy_t>;

  public:
    ~send_operation_t ();
    send_operation_t (send_operation_t &&) noexcept;
    send_operation_t &operator= (send_operation_t &&) noexcept;

    send_submit_operation_t message (message_t &part_) &&;
    send_submit_operation_t message (message_t &&part_) &&;

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class zlink::pair_socket_t;
    friend class zlink::dealer_socket_t;
    friend class zlink::router_socket_t;
    friend class zlink::stream_socket_t;
    friend class zlink::pub_socket_t;
    friend class zlink::xpub_socket_t;
    friend class zlink::received_t;
};

/// @brief Accepts further parts, timeout, and the terminal async execution of a request.
/// @note Parts are consumed on a successful submit (see @ref send_operation_t for the ownership model).
class request_submit_operation_t : private detail::operation_builder_base_t<
                                     detail::operation_state_t,
                                     detail::pooled_operation_state_policy_t>
{
    using base_t = detail::operation_builder_base_t<detail::operation_state_t,
                                                    detail::pooled_operation_state_policy_t>;

  public:
    ~request_submit_operation_t ();
    request_submit_operation_t (request_submit_operation_t &&) noexcept;
    request_submit_operation_t &operator= (request_submit_operation_t &&) noexcept;

    request_submit_operation_t &&message (message_t &part_) &&;
    request_submit_operation_t &&message (message_t &&part_) &&;
    request_submit_operation_t &&timeout (std::chrono::milliseconds timeout_) &&;
    /// Starts event-driven exact-target admission and then waits through the
    /// Core-owned reply lifecycle using the original absolute deadline.
    async_result_t<std::vector<message_t>> async () &&;

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class request_operation_t;
};

/// @brief Builds a request: add the request parts, then start and await a reply.
class request_operation_t : private detail::operation_builder_base_t<
                              detail::operation_state_t,
                              detail::pooled_operation_state_policy_t>
{
    using base_t = detail::operation_builder_base_t<detail::operation_state_t,
                                                    detail::pooled_operation_state_policy_t>;

  public:
    ~request_operation_t ();
    request_operation_t (request_operation_t &&) noexcept;
    request_operation_t &operator= (request_operation_t &&) noexcept;

    request_submit_operation_t message (message_t &part_) &&;
    request_submit_operation_t message (message_t &&part_) &&;

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class zlink::dealer_socket_t;
    friend class zlink::router_socket_t;
};

/// @brief Accepts further parts, flags, and the terminal submit of a reply builder.
/// @note Parts are consumed on a successful submit (see @ref send_operation_t for the ownership model).
class reply_submit_operation_t : private detail::operation_builder_base_t<
                                   detail::operation_state_t,
                                   detail::pooled_operation_state_policy_t>
{
    using base_t = detail::operation_builder_base_t<detail::operation_state_t,
                                                    detail::pooled_operation_state_policy_t>;

  public:
    ~reply_submit_operation_t ();
    reply_submit_operation_t (reply_submit_operation_t &&) noexcept;
    reply_submit_operation_t &operator= (reply_submit_operation_t &&) noexcept;

    reply_submit_operation_t &&message (message_t &part_) &&;
    reply_submit_operation_t &&flags (int flags_) &&;
    void submit () &&;

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class reply_operation_t;
};

/// @brief Builds a reply to a received request: add the reply parts, then submit().
class reply_operation_t : private detail::operation_builder_base_t<
                            detail::operation_state_t,
                            detail::pooled_operation_state_policy_t>
{
    using base_t = detail::operation_builder_base_t<detail::operation_state_t,
                                                    detail::pooled_operation_state_policy_t>;

  public:
    ~reply_operation_t ();
    reply_operation_t (reply_operation_t &&) noexcept;
    reply_operation_t &operator= (reply_operation_t &&) noexcept;

    reply_submit_operation_t message (message_t &part_) &&;

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class zlink::received_t;
    friend class zlink::router_socket_t;
};

} // namespace zlink
