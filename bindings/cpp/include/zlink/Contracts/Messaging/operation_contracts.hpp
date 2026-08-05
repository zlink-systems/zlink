/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "message.hpp"
#include "request_result.hpp"
#include "../Sockets/results.hpp"
#include "operation_builder_base.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <utility>
#include <vector>

namespace zlink
{

template <typename T> class async_result_t
{
  public:
    explicit async_result_t (std::future<T> future_) :
        async_result_t (std::move (future_), std::function<void ()> ())
    {
    }

    async_result_t (std::future<T> future_, std::function<void ()> progress_) :
        _state (std::make_shared<shared_state_t> (std::move (future_)))
    {
        _state->progress = std::move (progress_);
    }

    async_result_t (async_result_t &&) noexcept = default;
    async_result_t &operator= (async_result_t &&) noexcept = default;

    async_result_t (const async_result_t &) = delete;
    async_result_t &operator= (const async_result_t &) = delete;

    [[nodiscard]] bool valid () const { return _state && _state->future.valid (); }

    void wait () const { wait_until_ready (*_state); }

    template <typename Rep, typename Period>
    [[nodiscard]] std::future_status
    wait_for (const std::chrono::duration<Rep, Period> &timeout_) const
    {
        if (!_state->progress)
            return _state->future.wait_for (timeout_);

        if (timeout_ <= timeout_.zero ()) {
            if (_state->future.wait_for (std::chrono::milliseconds (0))
                == std::future_status::ready)
                return std::future_status::ready;
            pump_progress_once ();
            return _state->future.wait_for (std::chrono::milliseconds (0));
        }

        const std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::now ()
          + std::chrono::duration_cast<std::chrono::steady_clock::duration> (timeout_);

        while (true) {
            if (_state->future.wait_for (std::chrono::milliseconds (0))
                == std::future_status::ready)
                return std::future_status::ready;

            pump_progress_once ();

            const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ();
            if (now >= deadline)
                return std::future_status::timeout;

            const std::chrono::steady_clock::duration remaining = deadline - now;
            const std::chrono::steady_clock::duration slice =
              remaining < progress_slice () ? remaining : progress_slice ();
            if (_state->future.wait_for (slice) == std::future_status::ready)
                return std::future_status::ready;
        }
    }

    template <typename Clock, typename Duration>
    [[nodiscard]] std::future_status
    wait_until (const std::chrono::time_point<Clock, Duration> &deadline_) const
    {
        if (!_state->progress)
            return _state->future.wait_until (deadline_);

        const std::chrono::time_point<Clock, Duration> now = Clock::now ();
        if (deadline_ <= now)
            return _state->future.wait_for (std::chrono::milliseconds (0));

        return wait_for (deadline_ - now);
    }

    [[nodiscard]] T get ()
    {
        wait ();
        return _state->future.get ();
    }

  private:
    struct shared_state_t
    {
        explicit shared_state_t (std::future<T> future_) : future (std::move (future_)) {}

        std::future<T> future;
        std::function<void ()> progress;
    };

    static std::chrono::milliseconds progress_slice () { return std::chrono::milliseconds (1); }

    static void wait_until_ready (shared_state_t &state_)
    {
        if (!state_.progress) {
            state_.future.wait ();
            return;
        }

        while (state_.future.wait_for (std::chrono::milliseconds (0))
               != std::future_status::ready) {
            state_.progress ();
            (void) state_.future.wait_for (progress_slice ());
        }
    }

    void pump_progress_once () const
    {
        if (_state->progress)
            _state->progress ();
    }

    std::shared_ptr<shared_state_t> _state;
};

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
    bool submit () &&;

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class send_operation_t;
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

class request_callback_submit_operation_t;

/// @brief Accepts further parts, timeout, flags, and the terminal submit of a request.
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
    request_submit_operation_t &&timeout (std::chrono::milliseconds timeout_) &&;
    request_callback_submit_operation_t flags (int flags_) &&;
    async_result_t<std::vector<message_t>> async () &&;
    bool submit (request_callback_t callback_) &&;

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class request_operation_t;
    friend class request_callback_submit_operation_t;
};

/// @brief Builds a request: add the request parts, then submit and await a reply.
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

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class zlink::dealer_socket_t;
    friend class zlink::router_socket_t;
};

/// @brief Callback-submission stage of a request builder (reached after flags()).
class request_callback_submit_operation_t : private detail::operation_builder_base_t<
                                              detail::operation_state_t,
                                              detail::pooled_operation_state_policy_t>
{
    using base_t = detail::operation_builder_base_t<detail::operation_state_t,
                                                    detail::pooled_operation_state_policy_t>;

  public:
    ~request_callback_submit_operation_t ();
    request_callback_submit_operation_t (request_callback_submit_operation_t &&) noexcept;
    request_callback_submit_operation_t &
    operator= (request_callback_submit_operation_t &&) noexcept;

    request_callback_submit_operation_t &&message (message_t &part_) &&;
    request_callback_submit_operation_t &&timeout (std::chrono::milliseconds timeout_) &&;
    request_callback_submit_operation_t &&flags (int flags_) &&;
    bool submit (request_callback_t callback_) &&;

  private:
    using base_t::base_t;
    using base_t::release_state_ptr;
    using base_t::state;

    friend class request_submit_operation_t;
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
