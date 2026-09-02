/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_MESSAGING_ASYNC_OPERATION_STATE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_MESSAGING_ASYNC_OPERATION_STATE_HPP_INCLUDED

#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include <atomic>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace zlink::detail
{

class async_resume_slot_t
{
  public:
    explicit async_resume_slot_t (std::coroutine_handle<> continuation_) :
        _continuation (continuation_.address ())
    {
    }

    void abandon (std::coroutine_handle<> continuation_) noexcept
    {
        void *expected = continuation_.address ();
        (void) _continuation.compare_exchange_strong (
          expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void resume () noexcept
    {
        void *const address = _continuation.exchange (nullptr, std::memory_order_acq_rel);
        if (address)
            std::coroutine_handle<>::from_address (address).resume ();
    }

  private:
    std::atomic<void *> _continuation;
};

inline void resume_async_slot (std::shared_ptr<async_resume_slot_t> slot_,
                               async_continuation_scheduler_t scheduler_) noexcept
{
    if (!slot_)
        return;
    if (!scheduler_) {
        slot_->resume ();
        return;
    }
    try {
        const std::shared_ptr<async_resume_slot_t> scheduled_slot = slot_;
        scheduler_ ([scheduled_slot] { scheduled_slot->resume (); });
    }
    catch (...) {
        slot_->resume ();
    }
}

template <typename T>
class async_operation_state_t final : public async_result_state_t<T>
{
  public:
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

    void detach () noexcept override
    {
        std::lock_guard<std::mutex> lock (_mutex);
        _detached = true;
        _continuation.reset ();
        _scheduler = {};
    }

    void abandon (std::coroutine_handle<> continuation_) noexcept override
    {
        std::shared_ptr<async_resume_slot_t> slot;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            _detached = true;
            slot = _continuation;
            _continuation.reset ();
            _scheduler = {};
        }
        if (slot)
            slot->abandon (continuation_);
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
    template <typename Store> bool finish (Store &&store_) noexcept
    {
        std::shared_ptr<async_resume_slot_t> slot;
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
            if (!_detached) {
                slot = _continuation;
                scheduler = std::move (_scheduler);
            }
        }
        resume_async_slot (std::move (slot), std::move (scheduler));
        return true;
    }

    mutable std::mutex _mutex;
    std::optional<T> _value;
    std::exception_ptr _failure;
    std::shared_ptr<async_resume_slot_t> _continuation;
    async_continuation_scheduler_t _scheduler;
    bool _terminal = false;
    bool _consumed = false;
    bool _detached = false;
    bool _consumer_registered = false;
};

template <>
class async_operation_state_t<void> final : public async_result_state_t<void>
{
  public:
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

    void detach () noexcept override
    {
        std::lock_guard<std::mutex> lock (_mutex);
        _detached = true;
        _continuation.reset ();
        _scheduler = {};
    }

    void abandon (std::coroutine_handle<> continuation_) noexcept override
    {
        std::shared_ptr<async_resume_slot_t> slot;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            _detached = true;
            slot = _continuation;
            _continuation.reset ();
            _scheduler = {};
        }
        if (slot)
            slot->abandon (continuation_);
    }

    bool complete () noexcept { return finish (nullptr); }
    bool fail (std::exception_ptr failure_) noexcept
    {
        return finish (std::move (failure_));
    }

  private:
    bool finish (std::exception_ptr failure_) noexcept
    {
        std::shared_ptr<async_resume_slot_t> slot;
        async_continuation_scheduler_t scheduler;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_terminal)
                return false;
            _failure = std::move (failure_);
            _terminal = true;
            if (!_detached) {
                slot = _continuation;
                scheduler = std::move (_scheduler);
            }
        }
        resume_async_slot (std::move (slot), std::move (scheduler));
        return true;
    }

    mutable std::mutex _mutex;
    std::exception_ptr _failure;
    std::shared_ptr<async_resume_slot_t> _continuation;
    async_continuation_scheduler_t _scheduler;
    bool _terminal = false;
    bool _consumed = false;
    bool _detached = false;
    bool _consumer_registered = false;
};

} // namespace zlink::detail

#endif
