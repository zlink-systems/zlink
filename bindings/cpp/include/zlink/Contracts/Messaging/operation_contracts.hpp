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
    virtual void detach () noexcept = 0;
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
    virtual void detach () noexcept = 0;
    virtual void abandon (std::coroutine_handle<> continuation_) noexcept = 0;
};

struct async_result_access_t;
} // namespace detail

/// A move-only, single-consumer completion object for a binding operation.
/// Awaiting suspends the coroutine without occupying the calling thread.
/// Destroying an unconsumed result detaches only the caller-side waiter. Core
/// keeps ownership of an accepted operation and the socket-local completion
/// drain releases a later native result.
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
                _state->detach ();
            _state = std::move (other_._state);
        }
        return *this;
    }

    async_result_t (const async_result_t &) = delete;
    async_result_t &operator= (const async_result_t &) = delete;

    ~async_result_t ()
    {
        if (_state)
            _state->detach ();
    }

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
class publish_operation_t;
class received_t;
class router_socket_t;
class stream_socket_t;
class xpub_socket_t;

namespace detail
{
struct operation_state_t;
} // namespace detail

/// @brief Accepts further parts and the blocking or awaitable send terminal.
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
    /// Submits the part sequence to Core on the calling thread. The wait
    /// bound is `SNDTIMEO` on the socket; the binding owns no deadline.
    void submit () &&;
    /// Uses Core DONTWAIT admission and completes after the socket-local
    /// completion drain has processed the native result.
    async_result_t<void> async () &&;

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class send_operation_t;
};

/// @brief Accepts further parts, flags, and the synchronous publish submit.
/// @note PUB/XPUB deliberately expose no async terminal. Their default lossy
///       semantics do not wait at HWM; NODROP reports synchronous backpressure.
class publish_submit_operation_t : private detail::operation_builder_base_t<
                                     detail::operation_state_t,
                                     detail::pooled_operation_state_policy_t>
{
    using base_t = detail::operation_builder_base_t<detail::operation_state_t,
                                                    detail::pooled_operation_state_policy_t>;

  public:
    ~publish_submit_operation_t ();
    publish_submit_operation_t (publish_submit_operation_t &&) noexcept;
    publish_submit_operation_t &operator= (publish_submit_operation_t &&) noexcept;

    publish_submit_operation_t &&message (message_t &part_) &&;
    publish_submit_operation_t &&message (message_t &&part_) &&;
    publish_submit_operation_t &&flags (int flags_) &&;
    bool submit () &&;

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class publish_operation_t;
};

/// @brief Builds a PUB/XPUB publish whose only terminal is submit().
class publish_operation_t : private detail::operation_builder_base_t<
                              detail::operation_state_t,
                              detail::pooled_operation_state_policy_t>
{
    using base_t = detail::operation_builder_base_t<detail::operation_state_t,
                                                    detail::pooled_operation_state_policy_t>;

  public:
    ~publish_operation_t ();
    publish_operation_t (publish_operation_t &&) noexcept;
    publish_operation_t &operator= (publish_operation_t &&) noexcept;

    publish_submit_operation_t message (message_t &part_) &&;
    publish_submit_operation_t message (message_t &&part_) &&;

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class zlink::pub_socket_t;
    friend class zlink::xpub_socket_t;
};

/**
 * @brief Builds a multipart send: add one or more parts, then submit().
 * @note Submitting consumes the added message_t parts; on success each part is
 *       moved into the transport and left invalid. On failure, the binding's
 *       staging/restore policy returns ownership to an lvalue caller even though
 *       Core consumes the native part passed to a synchronous submit.
 * @note This binding-level restoration covers every part added from an lvalue,
 *       in a multipart sequence as well as a single-part one: each such
 *       message_t holds its payload again after a failed submit. A part added
 *       from an rvalue has no caller object left to restore and stays consumed.
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
    friend class zlink::received_t;
};

/// @brief Accepts further parts, reply timeout, and the two C++ request terminals.
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
    /// Submits the request to one exact target on the calling thread and
    /// returns a suspension that Core completes from its reply handler
    /// callback. The reply deadline is Core-owned
    /// (`ZLINK_REQUEST_TIMED_OUT`); the binding owns no timer and no worker.
    async_result_t<std::vector<message_t>> async () &&;
    /// Blocks the caller until Core's reply callback completes. The caller
    /// owns this wait; the binding creates no thread. Destroying the socket or
    /// context from a resumed continuation or callback deadlocks.
    std::vector<message_t> submit () &&;

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

/// @brief Accepts further parts and the synchronous terminal of a reply builder.
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
