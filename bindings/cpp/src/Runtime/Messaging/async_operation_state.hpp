/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_MESSAGING_ASYNC_OPERATION_STATE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_MESSAGING_ASYNC_OPERATION_STATE_HPP_INCLUDED

#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace zlink::detail
{

void ensure_async_continuation_dispatcher ();
void dispatch_async_continuation (std::function<void ()> work_) noexcept;

class async_resume_slot_t
{
  public:
    explicit async_resume_slot_t (std::coroutine_handle<> continuation_) :
        _continuation (continuation_.address ())
    {
    }

    // Completion and coroutine destruction can race. The handle is a single
    // claim token: exactly one side exchanges it for null and may act on it.
    void abandon (std::coroutine_handle<> continuation_) noexcept
    {
        void *expected = continuation_.address ();
        (void) _continuation.compare_exchange_strong (
          expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void resume () noexcept
    {
        void *const address =
          _continuation.exchange (nullptr, std::memory_order_acq_rel);
        if (address)
            std::coroutine_handle<>::from_address (address).resume ();
    }

  private:
    std::atomic<void *> _continuation;
};

template <typename T>
class async_operation_state_t final : public async_result_state_t<T>
{
  public:
    using cancel_fn_t = std::function<bool ()>;

    bool ready () const noexcept override
    {
        std::lock_guard<std::mutex> lock (_mutex);
        return _terminal;
    }

    bool suspend (std::coroutine_handle<> continuation_,
                  async_continuation_scheduler_t scheduler_) override
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_terminal)
            return false;
        if (_consumer_registered)
            throw std::logic_error ("async result already has a consumer");
        _consumer_registered = true;
        _continuation = std::make_shared<async_resume_slot_t> (continuation_);
        _scheduler = std::move (scheduler_);
        return true;
    }

    T take () override
    {
        std::exception_ptr failure;
        std::optional<T> value;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (!_terminal)
                throw std::logic_error ("async result is not complete");
            if (_consumed)
                throw std::logic_error ("async result was already consumed");
            _consumed = true;
            failure = _failure;
            value = std::move (_value);
            _continuation.reset ();
        }
        if (failure)
            std::rethrow_exception (failure);
        if (!value)
            throw std::logic_error ("async result completed without a value");
        return std::move (*value);
    }

    bool cancel () noexcept override
    {
        cancel_fn_t cancel;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_terminal || _cancel_requested || !_cancel)
                return false;
            _cancel_requested = true;
            cancel = _cancel;
        }
        try {
            if (cancel ())
                return true;
        }
        catch (...) {
        }
        std::lock_guard<std::mutex> lock (_mutex);
        if (!_terminal)
            _cancel_requested = false;
        return false;
    }

    void abandon (std::coroutine_handle<> continuation_) noexcept override
    {
        std::shared_ptr<async_resume_slot_t> continuation;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            continuation = _continuation;
            _scheduler = {};
        }
        if (continuation) {
            continuation->abandon (continuation_);
            (void) cancel ();
        }
    }

    void set_cancel (cancel_fn_t cancel_)
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (!_terminal)
            _cancel = std::move (cancel_);
    }

    // A Core callback receives only a raw userdata pointer. Keep this state
    // alive until Core either invokes that callback or rejects submission,
    // avoiding a separate bridge allocation solely for shared ownership.
    void retain_for_core (std::shared_ptr<async_operation_state_t> self_)
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (!_terminal)
            _core_lifetime = std::move (self_);
    }

    void release_from_core () noexcept
    {
        // Hold the anchor locally through the unlock: it may be the last
        // reference when a callback completed after the awaiting result was
        // abandoned.
        std::shared_ptr<async_operation_state_t> keep_alive;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            keep_alive = std::move (_core_lifetime);
        }
    }

    bool complete (T value_) noexcept
    {
        return finish ([&] { _value.emplace (std::move (value_)); });
    }

    bool fail (std::exception_ptr failure_) noexcept
    {
        return finish ([&] { _failure = std::move (failure_); });
    }

  private:
    template <typename TStore> bool finish (TStore &&store_) noexcept
    {
        std::shared_ptr<async_resume_slot_t> continuation;
        async_continuation_scheduler_t scheduler;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_terminal)
                return false;
            try {
                store_ ();
            }
            catch (...) {
                _failure = std::current_exception ();
            }
            _terminal = true;
            _cancel = {};
            continuation = _continuation;
            scheduler = std::move (_scheduler);
        }
        resume (continuation, std::move (scheduler));
        return true;
    }

    // The binding owns no dispatcher: the suspension resumes in the context
    // that delivered the completion (for a request that is Core's reply
    // handler callback). Handing the continuation to another execution model
    // is the framework's job through the optional promise scheduler hook.
    static void resume (std::shared_ptr<async_resume_slot_t> continuation_,
                        async_continuation_scheduler_t scheduler_) noexcept
    {
        if (!continuation_)
            return;
        if (!scheduler_) {
            continuation_->resume ();
            return;
        }
        try {
            scheduler_ ([continuation_] { continuation_->resume (); });
        }
        catch (...) {
            // A scheduler hook must either accept the continuation or throw
            // before doing so. Direct resume prevents permanent suspension
            // when a foreign promise violates that contract.
            continuation_->resume ();
        }
    }

    mutable std::mutex _mutex;
    std::optional<T> _value;
    std::exception_ptr _failure;
    cancel_fn_t _cancel;
    std::shared_ptr<async_resume_slot_t> _continuation;
    std::shared_ptr<async_operation_state_t> _core_lifetime;
    async_continuation_scheduler_t _scheduler;
    bool _terminal = false;
    bool _consumed = false;
    bool _cancel_requested = false;
    bool _consumer_registered = false;
};

// Accepted requests are owned by Core until the reply handler reaches its
// terminal event. Unlike send admission, they have no binding-side cancellation
// function: async_result_t::cancel() has always returned false after admission.
// Keep their result state separate so the hot reply path does not carry the
// generic std::function cancellation storage and its state transitions.
class async_request_operation_state_t final
  : public async_result_state_t<std::vector<message_t>>
{
  public:
    using value_t = std::vector<message_t>;

    bool ready () const noexcept override
    {
        std::lock_guard<std::mutex> lock (_mutex);
        return _terminal;
    }

    bool suspend (std::coroutine_handle<> continuation_,
                  async_continuation_scheduler_t scheduler_) override
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_terminal)
            return false;
        if (_consumer_registered)
            throw std::logic_error ("async result already has a consumer");
        _consumer_registered = true;
        _continuation = std::make_shared<async_resume_slot_t> (continuation_);
        _scheduler = std::move (scheduler_);
        return true;
    }

    value_t take () override
    {
        std::exception_ptr failure;
        std::optional<value_t> value;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (!_terminal)
                throw std::logic_error ("async result is not complete");
            if (_consumed)
                throw std::logic_error ("async result was already consumed");
            _consumed = true;
            failure = _failure;
            value = std::move (_value);
            _continuation.reset ();
        }
        if (failure)
            std::rethrow_exception (failure);
        if (!value)
            throw std::logic_error ("async result completed without a value");
        return std::move (*value);
    }

    // Once accepted, Core owns the terminal callback and no C cancellation
    // primitive exists for this request path.
    bool cancel () noexcept override { return false; }

    void abandon (std::coroutine_handle<> continuation_) noexcept override
    {
        std::shared_ptr<async_resume_slot_t> continuation;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            continuation = _continuation;
            _scheduler = {};
        }
        if (continuation)
            continuation->abandon (continuation_);
    }

    void retain_for_core (std::shared_ptr<async_request_operation_state_t> self_)
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (!_terminal)
            _core_lifetime = std::move (self_);
    }

    void release_from_core () noexcept
    {
        // Retain through the unlock: this can drop the final reference after a
        // caller discarded its async_result_t while Core was still in flight.
        std::shared_ptr<async_request_operation_state_t> keep_alive;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            keep_alive = std::move (_core_lifetime);
        }
    }

    bool complete (value_t value_) noexcept
    {
        return finish ([&] { _value.emplace (std::move (value_)); });
    }

    bool fail (std::exception_ptr failure_) noexcept
    {
        return finish ([&] { _failure = std::move (failure_); });
    }

  private:
    template <typename TStore> bool finish (TStore &&store_) noexcept
    {
        std::shared_ptr<async_resume_slot_t> continuation;
        async_continuation_scheduler_t scheduler;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_terminal)
                return false;
            try {
                store_ ();
            }
            catch (...) {
                _failure = std::current_exception ();
            }
            _terminal = true;
            continuation = _continuation;
            scheduler = std::move (_scheduler);
        }
        resume (continuation, std::move (scheduler));
        return true;
    }

    static void resume (std::shared_ptr<async_resume_slot_t> continuation_,
                        async_continuation_scheduler_t scheduler_) noexcept
    {
        if (!continuation_)
            return;
        if (!scheduler_) {
            continuation_->resume ();
            return;
        }
        try {
            scheduler_ ([continuation_] { continuation_->resume (); });
        }
        catch (...) {
            continuation_->resume ();
        }
    }

    mutable std::mutex _mutex;
    std::optional<value_t> _value;
    std::exception_ptr _failure;
    std::shared_ptr<async_resume_slot_t> _continuation;
    std::shared_ptr<async_request_operation_state_t> _core_lifetime;
    async_continuation_scheduler_t _scheduler;
    bool _terminal = false;
    bool _consumed = false;
    bool _consumer_registered = false;
};

template <>
class async_operation_state_t<void> final : public async_result_state_t<void>
{
  public:
    using cancel_fn_t = std::function<bool ()>;

    bool ready () const noexcept override
    {
        std::lock_guard<std::mutex> lock (_mutex);
        return _terminal;
    }

    bool suspend (std::coroutine_handle<> continuation_,
                  async_continuation_scheduler_t scheduler_) override
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_terminal)
            return false;
        if (_consumer_registered)
            throw std::logic_error ("async result already has a consumer");
        _consumer_registered = true;
        _continuation = std::make_shared<async_resume_slot_t> (continuation_);
        _scheduler = std::move (scheduler_);
        return true;
    }

    void take () override
    {
        std::exception_ptr failure;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (!_terminal)
                throw std::logic_error ("async result is not complete");
            if (_consumed)
                throw std::logic_error ("async result was already consumed");
            _consumed = true;
            failure = _failure;
            _continuation.reset ();
        }
        if (failure)
            std::rethrow_exception (failure);
    }

    bool cancel () noexcept override
    {
        cancel_fn_t cancel;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_terminal || _cancel_requested || !_cancel)
                return false;
            _cancel_requested = true;
            cancel = _cancel;
        }
        try {
            if (cancel ())
                return true;
        }
        catch (...) {
        }
        std::lock_guard<std::mutex> lock (_mutex);
        if (!_terminal)
            _cancel_requested = false;
        return false;
    }

    void abandon (std::coroutine_handle<> continuation_) noexcept override
    {
        std::shared_ptr<async_resume_slot_t> continuation;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            continuation = _continuation;
            _scheduler = {};
        }
        if (continuation) {
            continuation->abandon (continuation_);
            (void) cancel ();
        }
    }

    void set_cancel (cancel_fn_t cancel_)
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (!_terminal)
            _cancel = std::move (cancel_);
    }

    bool complete () noexcept { return finish (nullptr); }

    bool fail (std::exception_ptr failure_) noexcept
    {
        return finish (std::move (failure_));
    }

  private:
    bool finish (std::exception_ptr failure_) noexcept
    {
        std::shared_ptr<async_resume_slot_t> continuation;
        async_continuation_scheduler_t scheduler;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_terminal)
                return false;
            _failure = std::move (failure_);
            _terminal = true;
            _cancel = {};
            continuation = _continuation;
            scheduler = std::move (_scheduler);
        }
        resume (continuation, std::move (scheduler));
        return true;
    }

    // Immediate admission completes before a consumer is registered and never
    // reaches this function. A suspended void operation was Core-pending, so
    // its callback must hand the continuation outside Core's completion scope
    // before the coroutine can submit its next operation.
    static void resume (std::shared_ptr<async_resume_slot_t> continuation_,
                        async_continuation_scheduler_t scheduler_) noexcept
    {
        if (!continuation_)
            return;
        auto work = [continuation_] { continuation_->resume (); };
        dispatch_async_continuation (
          [work = std::move (work), scheduler = std::move (scheduler_)] () mutable {
              if (!scheduler) {
                  work ();
                  return;
              }
              try {
                  scheduler (work);
              }
              catch (...) {
                  work ();
              }
          });
    }

    mutable std::mutex _mutex;
    std::exception_ptr _failure;
    cancel_fn_t _cancel;
    std::shared_ptr<async_resume_slot_t> _continuation;
    async_continuation_scheduler_t _scheduler;
    bool _terminal = false;
    bool _consumed = false;
    bool _cancel_requested = false;
    bool _consumer_registered = false;
};

} // namespace zlink::detail

#endif
