/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_MESSAGING_ASYNC_OPERATION_STATE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_MESSAGING_ASYNC_OPERATION_STATE_HPP_INCLUDED

#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace zlink::detail
{

void dispatch_async_continuation (std::function<void ()> work_) noexcept;
void ensure_async_continuation_dispatcher ();

class async_resume_slot_t
{
  public:
    explicit async_resume_slot_t (std::coroutine_handle<> continuation_) :
        _continuation (continuation_)
    {
    }

    void abandon (std::coroutine_handle<> continuation_) noexcept
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_continuation == continuation_)
            _continuation = {};
    }

    void resume () noexcept
    {
        std::coroutine_handle<> continuation;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            continuation = std::exchange (_continuation, {});
        }
        if (continuation)
            continuation.resume ();
    }

  private:
    std::mutex _mutex;
    std::coroutine_handle<> _continuation{};
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
                  // A scheduler hook must either accept the continuation or
                  // throw before doing so. Direct resume prevents permanent
                  // suspension when a foreign promise violates that contract.
                  work ();
              }
          });
    }

    mutable std::mutex _mutex;
    std::optional<T> _value;
    std::exception_ptr _failure;
    cancel_fn_t _cancel;
    std::shared_ptr<async_resume_slot_t> _continuation;
    async_continuation_scheduler_t _scheduler;
    bool _terminal = false;
    bool _consumed = false;
    bool _cancel_requested = false;
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
