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

template <typename T> class task_t
{
  public:
    using callback_t = std::function<void (result_t<T>)>;
    using starter_t = std::function<void (callback_t)>;

    struct state_t
    {
        std::mutex mutex;
        std::condition_variable ready;
        std::optional<result_t<T>> result;
        std::coroutine_handle<> continuation;
        starter_t starter;
        bool started = false;
    };

    struct promise_type
    {
        std::shared_ptr<state_t> state = std::make_shared<state_t> ();

        task_t get_return_object ()
        {
            return task_t (std::coroutine_handle<promise_type>::from_promise (*this), state);
        }
        std::suspend_never initial_suspend () noexcept { return {}; }
        struct final_awaiter
        {
            bool await_ready () noexcept { return false; }

            std::coroutine_handle<>
            await_suspend (std::coroutine_handle<promise_type> handle) noexcept
            {
                auto state = handle.promise ().state;
                state->ready.notify_all ();
                std::coroutine_handle<> continuation;
                {
                    std::lock_guard<std::mutex> lock (state->mutex);
                    continuation = state->continuation;
                    state->continuation = {};
                }
                return continuation ? continuation : std::noop_coroutine ();
            }

            void await_resume () noexcept {}
        };

        final_awaiter final_suspend () noexcept
        {
            return {};
        }
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
            set_result (result_t<T>::failure (error_code_t::user_callback_failed, detail));
        }
        void return_value (result_t<T> value) { set_result (std::move (value)); }
        template <typename U>
        requires (!std::is_same_v<std::remove_cvref_t<U>, result_t<T>>) void return_value (
          U &&value)
        {
            set_result (result_t<T>::success (T (std::forward<U> (value))));
        }

      private:
        void set_result (result_t<T> value)
        {
            std::lock_guard<std::mutex> lock (state->mutex);
            state->result = std::move (value);
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

    task_t (task_t &&other) noexcept :
        _handle (other._handle), _state (std::move (other._state))
    {
        other._handle = {};
    }
    task_t &operator= (task_t &&other) noexcept
    {
        if (this != &other) {
            if (_handle) {
                _handle.destroy ();
            }
            _handle = other._handle;
            _state = std::move (other._state);
            other._handle = {};
        }
        return *this;
    }
    task_t (const task_t &) = delete;
    task_t &operator= (const task_t &) = delete;
    ~task_t ()
    {
        cancel_pending ();
        if (_handle) {
            _handle.destroy ();
        }
    }

    bool await_ready ()
    {
        start ();
        std::lock_guard<std::mutex> lock (_state->mutex);
        return _state->result.has_value ();
    }

    void await_suspend (std::coroutine_handle<> continuation)
    {
        {
            std::lock_guard<std::mutex> lock (_state->mutex);
            _state->continuation = continuation;
        }
        start ();
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
        _state->result = result_t<T>::failure (error_code_t::canceled,
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
    task_t (std::coroutine_handle<promise_type> handle, std::shared_ptr<state_t> state) :
        _handle (handle), _state (std::move (state))
    {
    }

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
        starter ([state = _state] (result_t<T> result) {
            std::coroutine_handle<> continuation;
            {
                std::lock_guard<std::mutex> lock (state->mutex);
                if (state->result.has_value ()) {
                    return;
                }
                state->result = std::move (result);
                continuation = state->continuation;
                state->continuation = {};
            }
            state->ready.notify_all ();
            if (continuation) {
                continuation.resume ();
            }
        });
    }

    void cancel_pending ()
    {
        if (!_state) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock (_state->mutex);
            if (_state->result.has_value ()) {
                return;
            }
            _state->continuation = {};
            _state->result = result_t<T>::failure (error_code_t::canceled,
                                                   "stream e2e task was canceled");
        }
        _state->ready.notify_all ();
    }

    std::coroutine_handle<promise_type> _handle{};
    std::shared_ptr<state_t> _state;
};

template <> class task_t<void>
{
  public:
    using callback_t = std::function<void (result_t<void>)>;
    using starter_t = std::function<void (callback_t)>;

    struct state_t
    {
        std::mutex mutex;
        std::condition_variable ready;
        std::optional<result_t<void>> result;
        std::coroutine_handle<> continuation;
        starter_t starter;
        bool started = false;
    };

    struct promise_type
    {
        std::shared_ptr<state_t> state = std::make_shared<state_t> ();

        task_t get_return_object ()
        {
            return task_t (std::coroutine_handle<promise_type>::from_promise (*this), state);
        }
        std::suspend_never initial_suspend () noexcept { return {}; }
        struct final_awaiter
        {
            bool await_ready () noexcept { return false; }

            std::coroutine_handle<>
            await_suspend (std::coroutine_handle<promise_type> handle) noexcept
            {
                auto state = handle.promise ().state;
                state->ready.notify_all ();
                std::coroutine_handle<> continuation;
                {
                    std::lock_guard<std::mutex> lock (state->mutex);
                    continuation = state->continuation;
                    state->continuation = {};
                }
                return continuation ? continuation : std::noop_coroutine ();
            }

            void await_resume () noexcept {}
        };

        final_awaiter final_suspend () noexcept
        {
            return {};
        }
        void unhandled_exception ()
        {
            set_result (result_t<void>::failure (error_code_t::user_callback_failed,
                                                 "unhandled connector coroutine exception"));
        }
        void return_void () { set_result (result_t<void>::success ()); }

      private:
        void set_result (result_t<void> value)
        {
            std::lock_guard<std::mutex> lock (state->mutex);
            state->result = std::move (value);
        }

    };

    explicit task_t (result_t<void> result) : _state (std::make_shared<state_t> ())
    {
        _state->result = std::move (result);
    }

    explicit task_t (starter_t starter) : _state (std::make_shared<state_t> ())
    {
        _state->starter = std::move (starter);
    }

    task_t (task_t &&other) noexcept :
        _handle (other._handle), _state (std::move (other._state))
    {
        other._handle = {};
    }
    task_t &operator= (task_t &&other) noexcept
    {
        if (this != &other) {
            if (_handle) {
                _handle.destroy ();
            }
            _handle = other._handle;
            _state = std::move (other._state);
            other._handle = {};
        }
        return *this;
    }
    task_t (const task_t &) = delete;
    task_t &operator= (const task_t &) = delete;
    ~task_t ()
    {
        cancel_pending ();
        if (_handle) {
            _handle.destroy ();
        }
    }

    bool await_ready ()
    {
        start ();
        std::lock_guard<std::mutex> lock (_state->mutex);
        return _state->result.has_value ();
    }

    void await_suspend (std::coroutine_handle<> continuation)
    {
        {
            std::lock_guard<std::mutex> lock (_state->mutex);
            _state->continuation = continuation;
        }
        start ();
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
        _state->result = result_t<void>::failure (error_code_t::canceled,
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
    task_t (std::coroutine_handle<promise_type> handle, std::shared_ptr<state_t> state) :
        _handle (handle), _state (std::move (state))
    {
    }

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
        starter ([state = _state] (result_t<void> result) {
            std::coroutine_handle<> continuation;
            {
                std::lock_guard<std::mutex> lock (state->mutex);
                if (state->result.has_value ()) {
                    return;
                }
                state->result = std::move (result);
                continuation = state->continuation;
                state->continuation = {};
            }
            state->ready.notify_all ();
            if (continuation) {
                continuation.resume ();
            }
        });
    }

    void cancel_pending ()
    {
        if (!_state) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock (_state->mutex);
            if (_state->result.has_value ()) {
                return;
            }
            _state->continuation = {};
            _state->result = result_t<void>::failure (error_code_t::canceled,
                                                      "stream e2e task was canceled");
        }
        _state->ready.notify_all ();
    }

    std::coroutine_handle<promise_type> _handle{};
    std::shared_ptr<state_t> _state;
};

} // namespace zlink::stream_e2e_client
