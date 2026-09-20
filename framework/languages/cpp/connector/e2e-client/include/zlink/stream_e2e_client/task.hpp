/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/result.hpp>

#include <condition_variable>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace zlink::stream_e2e_client
{

using zlink::stream_connector::error_code_t;
using zlink::stream_connector::result_t;

namespace detail
{

/// Shared state of a task, and the one place a task's completion is decided.
///
/// A task has a single producer: the starter callback of an operation task, or
/// the coroutine frame of a coroutine task. `complete` is that producer's last
/// act. Until it runs the task carries no result, so a result is proof that the
/// producer has finished - waiters, awaiting coroutines and `~task_t` all read
/// the same fact and none of them can act on a producer that is still working.
template <typename T> struct task_state_t
{
    using callback_t = std::function<void (result_t<T>)>;
    using starter_t = std::function<void (callback_t)>;

    std::mutex mutex;
    std::condition_variable ready;
    std::optional<result_t<T>> result;
    std::coroutine_handle<> continuation;
    starter_t starter;
    bool started = false;

    void complete (result_t<T> value)
    {
        std::coroutine_handle<> resumed;
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (result.has_value ()) {
                return;
            }
            result = std::move (value);
            resumed = continuation;
            continuation = {};
        }
        ready.notify_all ();
        if (resumed) {
            resumed.resume ();
        }
    }

    /// Completes a task whose owner went away before its producer finished. The
    /// awaiting coroutine is dropped rather than resumed: its own frame is going
    /// away with the owner that held it.
    void cancel ()
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (result.has_value ()) {
                return;
            }
            continuation = {};
            result = result_t<T>::failure (error_code_t::disconnected,
                                           "stream e2e task was canceled");
        }
        ready.notify_all ();
    }
};

/// Final suspend of every coroutine task, and the only place a coroutine frame
/// is destroyed.
///
/// **A coroutine frame is owned by the frame itself.** The connector resumes a
/// client coroutine on its own thread - on Windows `connector_t::close`
/// completes pending operations from the IOCP thread - so the coroutine runs
/// its result production, its frame-local cleanup and its final suspend on a
/// thread the task's owner does not control. Nobody outside the frame can tell
/// when the frame has stopped touching itself, so nobody outside the frame may
/// free it. The frame carries its outcome out to here, destroys itself, and
/// only then completes the task; `task_t` holds no coroutine handle at all.
template <typename T, typename Promise> struct task_final_awaiter_t
{
    bool await_ready () noexcept { return false; }

    void await_suspend (std::coroutine_handle<Promise> handle) noexcept
    {
        /* Move everything off the frame first. After `destroy` nothing in the
         * frame may be read or written again - including `*this`, which lives
         * in the frame as the awaiter of this final suspend. */
        auto state = handle.promise ().state;
        auto outcome = std::move (handle.promise ().outcome);
        handle.destroy ();

        if (!outcome) {
            outcome = result_t<T>::failure (error_code_t::user_callback_failed,
                                            "stream e2e coroutine produced no result");
        }
        state->complete (std::move (*outcome));
    }

    void await_resume () noexcept {}
};

/// The half of a coroutine task's promise that is the same for every result
/// type: where the outcome waits, and how the frame ends.
template <typename T, typename Promise> struct task_promise_base_t
{
    std::shared_ptr<task_state_t<T>> state = std::make_shared<task_state_t<T>> ();
    std::optional<result_t<T>> outcome;

    std::suspend_never initial_suspend () noexcept { return {}; }
    task_final_awaiter_t<T, Promise> final_suspend () noexcept { return {}; }

    void unhandled_exception ()
    {
        std::string detail = "unhandled connector coroutine exception";
        try {
            throw;
        }
        catch (const std::exception &error) {
            detail += std::string (": ") + error.what ();
        }
        catch (...) {
        }
        outcome = result_t<T>::failure (error_code_t::user_callback_failed, std::move (detail));
    }
};

} // namespace detail

template <typename T> class task_t
{
  public:
    using state_t = detail::task_state_t<T>;
    using callback_t = typename state_t::callback_t;
    using starter_t = typename state_t::starter_t;

    struct promise_type : detail::task_promise_base_t<T, promise_type>
    {
        task_t get_return_object () { return task_t (this->state); }

        void return_value (result_t<T> value) { this->outcome = std::move (value); }
        template <typename U>
        requires (!std::is_same_v<std::remove_cvref_t<U>, result_t<T>>) void return_value (
          U &&value)
        {
            this->outcome = result_t<T>::success (T (std::forward<U> (value)));
        }
    };

    explicit task_t (result_t<T> result) : _state (std::make_shared<state_t> ())
    {
        _state->result = std::move (result);
    }

    explicit task_t (starter_t starter) : _state (std::make_shared<state_t> ())
    {
        _state->starter = std::move (starter);
    }

    task_t (task_t &&other) noexcept = default;
    task_t &operator= (task_t &&other) noexcept
    {
        if (this != &other) {
            cancel_pending ();
            _state = std::move (other._state);
        }
        return *this;
    }
    task_t (const task_t &) = delete;
    task_t &operator= (const task_t &) = delete;
    ~task_t () { cancel_pending (); }

    bool await_ready ()
    {
        start ();
        std::lock_guard<std::mutex> lock (_state->mutex);
        return _state->result.has_value ();
    }

    bool await_suspend (std::coroutine_handle<> continuation)
    {
        {
            std::lock_guard<std::mutex> lock (_state->mutex);
            if (_state->result.has_value ()) {
                return false;
            }
            _state->continuation = continuation;
        }
        start ();
        return true;
    }

    T await_resume ()
    {
        auto result = consume_result ();
        if (!result) {
            const auto message =
              result.error () ? result.error ()->message : "stream e2e task failed";
            throw std::runtime_error (message);
        }
        return std::move (result.value ());
    }

    void start () const { start_operation (); }

    const result_t<T> &result () const
    {
        start ();
        std::unique_lock<std::mutex> lock (_state->mutex);
        _state->ready.wait (lock, [this] { return _state->result.has_value (); });
        return *_state->result;
    }

    result_t<T> consume_result ()
    {
        start ();
        std::unique_lock<std::mutex> lock (_state->mutex);
        _state->ready.wait (lock, [this] { return _state->result.has_value (); });
        auto result = std::move (*_state->result);
        _state->result = result_t<T>::failure (error_code_t::disconnected,
                                               "stream e2e task result was consumed");
        return result;
    }

    void on_completed (std::function<void (result_t<T>)> callback)
    {
        if (callback) {
            callback (consume_result ());
        }
    }

  private:
    explicit task_t (std::shared_ptr<state_t> state) : _state (std::move (state)) {}

    void start_operation () const
    {
        starter_t starter;
        {
            std::lock_guard<std::mutex> lock (_state->mutex);
            if (_state->started || !_state->starter) {
                return;
            }
            _state->started = true;
            starter = _state->starter;
        }
        starter ([state = _state] (result_t<T> result) { state->complete (std::move (result)); });
    }

    void cancel_pending ()
    {
        if (_state) {
            _state->cancel ();
        }
    }

    std::shared_ptr<state_t> _state;
};

template <> class task_t<void>
{
  public:
    using state_t = detail::task_state_t<void>;
    using callback_t = state_t::callback_t;
    using starter_t = state_t::starter_t;

    struct promise_type : detail::task_promise_base_t<void, promise_type>
    {
        task_t get_return_object () { return task_t (this->state); }

        void return_void () { this->outcome = result_t<void>::success (); }
    };

    explicit task_t (result_t<void> result) : _state (std::make_shared<state_t> ())
    {
        _state->result = std::move (result);
    }

    explicit task_t (starter_t starter) : _state (std::make_shared<state_t> ())
    {
        _state->starter = std::move (starter);
    }

    task_t (task_t &&other) noexcept = default;
    task_t &operator= (task_t &&other) noexcept
    {
        if (this != &other) {
            cancel_pending ();
            _state = std::move (other._state);
        }
        return *this;
    }
    task_t (const task_t &) = delete;
    task_t &operator= (const task_t &) = delete;
    ~task_t () { cancel_pending (); }

    bool await_ready ()
    {
        start ();
        std::lock_guard<std::mutex> lock (_state->mutex);
        return _state->result.has_value ();
    }

    bool await_suspend (std::coroutine_handle<> continuation)
    {
        {
            std::lock_guard<std::mutex> lock (_state->mutex);
            if (_state->result.has_value ()) {
                return false;
            }
            _state->continuation = continuation;
        }
        start ();
        return true;
    }

    void await_resume ()
    {
        auto result = consume_result ();
        if (!result) {
            const auto message =
              result.error () ? result.error ()->message : "stream e2e task failed";
            throw std::runtime_error (message);
        }
    }

    void start () const { start_operation (); }

    const result_t<void> &result () const
    {
        start ();
        std::unique_lock<std::mutex> lock (_state->mutex);
        _state->ready.wait (lock, [this] { return _state->result.has_value (); });
        return *_state->result;
    }

    result_t<void> consume_result ()
    {
        start ();
        std::unique_lock<std::mutex> lock (_state->mutex);
        _state->ready.wait (lock, [this] { return _state->result.has_value (); });
        auto result = std::move (*_state->result);
        _state->result = result_t<void>::failure (error_code_t::disconnected,
                                                  "stream e2e task result was consumed");
        return result;
    }

    void on_completed (std::function<void (result_t<void>)> callback)
    {
        if (callback) {
            callback (consume_result ());
        }
    }

  private:
    explicit task_t (std::shared_ptr<state_t> state) : _state (std::move (state)) {}

    void start_operation () const
    {
        starter_t starter;
        {
            std::lock_guard<std::mutex> lock (_state->mutex);
            if (_state->started || !_state->starter) {
                return;
            }
            _state->started = true;
            starter = _state->starter;
        }
        starter ([state = _state] (result_t<void> result) { state->complete (std::move (result)); });
    }

    void cancel_pending ()
    {
        if (_state) {
            _state->cancel ();
        }
    }

    std::shared_ptr<state_t> _state;
};

} // namespace zlink::stream_e2e_client
