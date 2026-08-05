/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/errors/result.hpp>

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace zlink::framework
{

template <typename T> class task_t;

namespace detail
{

using task_scheduler_t = std::function<void (std::function<void ()>)>;

struct serial_resume_failure_t
{
    framework_error_kind_t kind;
    std::string message;
};

inline thread_local std::optional<serial_resume_failure_t>
  serial_resume_failure;

inline void set_serial_resume_failure (framework_error_kind_t kind,
                                       std::string message)
{
    serial_resume_failure = serial_resume_failure_t{kind, std::move (message)};
}

inline std::optional<serial_resume_failure_t> take_serial_resume_failure ()
{
    auto failure = std::move (serial_resume_failure);
    serial_resume_failure.reset ();
    return failure;
}

class deferred_barrier_t
{
  public:
    virtual ~deferred_barrier_t () = default;
    virtual result_t<void> activate (std::function<void ()> work) = 0;
    virtual void cancel () noexcept = 0;
};

using deferred_barrier_reserver_t =
  std::function<result_t<std::shared_ptr<deferred_barrier_t>> ()>;

class serial_turn_t
{
  public:
    virtual ~serial_turn_t () = default;
    virtual bool release () = 0;
    virtual bool released () const = 0;
    virtual task_scheduler_t resume_scheduler () = 0;
    virtual bool belongs_to (const void *owner) const noexcept = 0;
    // True for a turn released from the previous handler's after-active
    // phase. It still owns the serial queue, but it is no longer the handler
    // turn for lifecycle fences such as relocation readiness.
    virtual bool is_after_active_phase () const noexcept = 0;
    virtual bool allows_yield () const noexcept = 0;
    virtual result_t<void> defer (std::function<void ()> work,
                                  std::function<void ()> cancel = {}) = 0;
    virtual void cancel_deferred () noexcept = 0;
};

inline thread_local std::shared_ptr<serial_turn_t> current_serial_turn_handle;

inline std::shared_ptr<serial_turn_t> capture_current_serial_turn ()
{
    return current_serial_turn_handle;
}

class serial_turn_scope_t
{
  public:
    explicit serial_turn_scope_t (std::shared_ptr<serial_turn_t> turn) :
        _previous (std::move (current_serial_turn_handle))
    {
        current_serial_turn_handle = std::move (turn);
    }

    ~serial_turn_scope_t () { current_serial_turn_handle = std::move (_previous); }

    serial_turn_scope_t (const serial_turn_scope_t &) = delete;
    serial_turn_scope_t &operator= (const serial_turn_scope_t &) = delete;

  private:
    std::shared_ptr<serial_turn_t> _previous;
};

inline task_scheduler_t held_serial_turn_scheduler (std::shared_ptr<serial_turn_t> turn)
{
    return [turn = std::move (turn)] (std::function<void ()> work) mutable {
        serial_turn_scope_t scope (turn);
        if (work) {
            work ();
        }
    };
}

struct serial_turn_await_plan_t
{
    std::shared_ptr<serial_turn_t> turn;
    task_scheduler_t scheduler;
    bool holds_turn = false;
};

inline std::optional<serial_turn_await_plan_t>
prepare_serial_turn_await (bool release_turn)
{
    auto turn = capture_current_serial_turn ();
    if (!turn || turn->released ()) {
        return std::nullopt;
    }
    if (release_turn) {
        if (!turn->release ()) {
            return std::nullopt;
        }
        return serial_turn_await_plan_t{turn, turn->resume_scheduler (), false};
    }
    return serial_turn_await_plan_t{turn, held_serial_turn_scheduler (turn), true};
}

inline bool current_serial_turn_allows_yield ()
{
    const auto turn = capture_current_serial_turn ();
    return turn && !turn->released () && turn->allows_yield ();
}

inline result_t<void> defer_current_serial_turn (
  std::function<void ()> work,
  std::function<void ()> cancel = {})
{
    auto turn = capture_current_serial_turn ();
    if (!turn || turn->released ()) {
        return result_t<void>::failure (
          framework_error_kind_t::not_configured,
          "Actor join defer requires an open Framework handler turn");
    }
    return turn->defer (std::move (work), std::move (cancel));
}

template <typename T> task_t<T> unsupported_yield_task ();

/* Ambient dispatch-context propagation across coroutine suspension
 * (flow-correlation MFLOW-EXT-014). The runtime installs the hooks once; a
 * continuation or callback re-enters the context captured at registration
 * and the guard ends with the resume call, so a finished continuation never
 * leaks its context into unrelated work. Without hooks the machinery costs
 * one atomic pointer load. Both functions publish through a single atomic
 * table pointer so a concurrent task can never observe a half-installed
 * pair; installation runs from a dynamic initializer while other
 * initializers may already schedule tasks. An atomic plain pointer is
 * lock-free on every supported platform. */
struct ambient_context_hooks_t
{
    std::shared_ptr<void> (*capture) ();
    std::shared_ptr<void> (*enter) (const std::shared_ptr<void> &);
};

inline std::atomic<const ambient_context_hooks_t *> ambient_context_hooks{nullptr};

inline std::shared_ptr<void> capture_ambient_context ()
{
    const auto *hooks = ambient_context_hooks.load (std::memory_order_acquire);
    return hooks != nullptr ? hooks->capture () : nullptr;
}

inline std::shared_ptr<void> enter_ambient_context (const std::shared_ptr<void> &snapshot)
{
    const auto *hooks = ambient_context_hooks.load (std::memory_order_acquire);
    return hooks != nullptr && snapshot ? hooks->enter (snapshot) : nullptr;
}

template <typename T>
class task_shared_state_t : public std::enable_shared_from_this<task_shared_state_t<T>>
{
  public:
    explicit task_shared_state_t (task_scheduler_t scheduler = {}) :
        _scheduler (std::move (scheduler))
    {
    }

    void complete (result_t<T> result)
    {
        std::vector<std::pair<std::coroutine_handle<>, std::shared_ptr<void>>> continuations;
        std::vector<std::pair<std::function<void (const result_t<T> &)>, std::shared_ptr<void>>>
          callbacks;
        auto self = this->shared_from_this ();
        {
            std::lock_guard lock (_mutex);
            if (_result) {
                return;
            }
            _result = std::move (result);
            continuations = std::move (_continuations);
            callbacks = std::move (_callbacks);
        }
        _ready.notify_all ();
        for (auto &callback : callbacks) {
            schedule ([self, callback = std::move (callback)] {
                const auto ambient_guard = enter_ambient_context (callback.second);
                callback.first (*self->_result);
            });
        }
        for (auto &continuation : continuations) {
            schedule ([continuation] {
                const auto ambient_guard = enter_ambient_context (continuation.second);
                continuation.first.resume ();
            });
        }
    }

    bool is_ready () const
    {
        std::lock_guard lock (_mutex);
        return _result.has_value ();
    }

    void set_continuation (std::coroutine_handle<> continuation)
    {
        auto snapshot = capture_ambient_context ();
        bool resume_now = false;
        {
            std::lock_guard lock (_mutex);
            if (_result) {
                resume_now = true;
            } else {
                _continuations.emplace_back (continuation, std::move (snapshot));
            }
        }
        if (resume_now) {
            schedule ([continuation, snapshot = std::move (snapshot)] {
                const auto ambient_guard = enter_ambient_context (snapshot);
                continuation.resume ();
            });
        }
    }

    const result_t<T> &result ()
    {
        std::unique_lock lock (_mutex);
        _ready.wait (lock, [&] { return _result.has_value (); });
        return *_result;
    }

    void on_completed (std::function<void (const result_t<T> &)> callback)
    {
        auto self = this->shared_from_this ();
        bool completed = false;
        {
            std::lock_guard lock (_mutex);
            if (_result) {
                completed = true;
            } else {
                _callbacks.emplace_back (std::move (callback), capture_ambient_context ());
                return;
            }
        }
        if (completed) {
            /* The already-completed path may still defer through a scheduler,
             * so the registration-time context travels with the callback the
             * same way the pending path stores it. */
            schedule ([self, callback = std::move (callback),
                       snapshot = capture_ambient_context ()] {
                const auto ambient_guard = enter_ambient_context (snapshot);
                callback (*self->_result);
            });
        }
    }

  private:
    void schedule (std::function<void ()> work) const
    {
        if (!_scheduler) {
            work ();
            return;
        }
        try {
            _scheduler (std::move (work));
        }
        catch (...) {
        }
    }

    mutable std::mutex _mutex;
    std::condition_variable _ready;
    task_scheduler_t _scheduler;
    std::optional<result_t<T>> _result;
    std::vector<std::pair<std::coroutine_handle<>, std::shared_ptr<void>>> _continuations;
    std::vector<std::pair<std::function<void (const result_t<T> &)>, std::shared_ptr<void>>>
      _callbacks;
};

template <typename T> class task_completion_source_t
{
  public:
    explicit task_completion_source_t (task_scheduler_t scheduler = {}) :
        _state (std::make_shared<task_shared_state_t<T>> (std::move (scheduler)))
    {
    }

    task_t<T> task ();

    void complete (result_t<T> result) { _state->complete (std::move (result)); }

  private:
    std::shared_ptr<task_shared_state_t<T>> _state;
};

template <typename T, typename TCallback>
void observe_task_completion (task_t<T> &task, TCallback &&callback);

template <typename T> task_t<T> reschedule_task (task_t<T> task, task_scheduler_t scheduler);

} // namespace detail

template <typename T> class task_t
{
  public:
    struct promise_type
    {
        detail::task_completion_source_t<T> completion;

        task_t get_return_object () { return completion.task (); }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void unhandled_exception ()
        {
            try {
                throw;
            }
            catch (const framework_exception_t &error) {
                completion.complete (
                  detail::result_access_t::failure<T> (error));
            }
            catch (const std::exception &error) {
                completion.complete (
                  result_t<T>::failure (framework_error_kind_t::internal_failure, error.what ()));
            }
            catch (...) {
                completion.complete (result_t<T>::failure (framework_error_kind_t::internal_failure,
                                                           "unhandled coroutine exception"));
            }
        }

        void return_value (result_t<T> result) { completion.complete (std::move (result)); }

        template <typename U>
        requires (!std::is_same_v<std::remove_cvref_t<U>, result_t<T>>) void return_value (
          U &&value)
        {
            completion.complete (result_t<T>::success (T (std::forward<U> (value))));
        }
    };

    explicit task_t (result_t<T> result) :
        _state (std::make_shared<detail::task_shared_state_t<T>> ())
    {
        _state->complete (std::move (result));
    }

    task_t (task_t &&) noexcept = default;
    task_t &operator= (task_t &&) noexcept = default;
    task_t (const task_t &) = delete;
    task_t &operator= (const task_t &) = delete;
    ~task_t () = default;

    bool await_ready () const noexcept { return _state->is_ready (); }
    void await_suspend (std::coroutine_handle<> continuation)
    {
        _state->set_continuation (continuation);
    }
    T await_resume ()
    {
        if (const auto failure = detail::take_serial_resume_failure ())
            throw framework_exception_t (failure->kind, failure->message);
        return result ().value ();
    }

    const result_t<T> &result () const { return _state->result (); }

  private:
    explicit task_t (std::shared_ptr<detail::task_shared_state_t<T>> state) :
        _state (std::move (state))
    {
    }

    std::shared_ptr<detail::task_shared_state_t<T>> _state;

    friend class detail::task_completion_source_t<T>;
    template <typename TObserved, typename TCallback>
    friend void detail::observe_task_completion (task_t<TObserved> &, TCallback &&);
};

template <> class task_t<void>
{
  public:
    struct promise_type
    {
        detail::task_completion_source_t<void> completion;

        task_t get_return_object () { return completion.task (); }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void unhandled_exception ()
        {
            try {
                throw;
            }
            catch (const framework_exception_t &error) {
                completion.complete (
                  detail::result_access_t::failure<void> (error));
            }
            catch (const std::exception &error) {
                completion.complete (
                  result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ()));
            }
            catch (...) {
                completion.complete (result_t<void>::failure (
                  framework_error_kind_t::internal_failure, "unhandled coroutine exception"));
            }
        }
        void return_void () noexcept { completion.complete (result_t<void>::success ()); }
    };

    explicit task_t (result_t<void> result) :
        _state (std::make_shared<detail::task_shared_state_t<void>> ())
    {
        _state->complete (std::move (result));
    }

    task_t (task_t &&) noexcept = default;
    task_t &operator= (task_t &&) noexcept = default;
    task_t (const task_t &) = delete;
    task_t &operator= (const task_t &) = delete;
    ~task_t () = default;

    bool await_ready () const noexcept { return _state->is_ready (); }
    void await_suspend (std::coroutine_handle<> continuation)
    {
        _state->set_continuation (continuation);
    }
    void await_resume ()
    {
        if (const auto failure = detail::take_serial_resume_failure ())
            throw framework_exception_t (failure->kind, failure->message);
        result ().value ();
    }

    const result_t<void> &result () const { return _state->result (); }

  private:
    explicit task_t (std::shared_ptr<detail::task_shared_state_t<void>> state) :
        _state (std::move (state))
    {
    }

    std::shared_ptr<detail::task_shared_state_t<void>> _state;

    friend class detail::task_completion_source_t<void>;
    template <typename TObserved, typename TCallback>
    friend void detail::observe_task_completion (task_t<TObserved> &, TCallback &&);
};

namespace detail
{

template <typename T> task_t<T> task_completion_source_t<T>::task ()
{
    return task_t<T> (_state);
}

template <typename T, typename TCallback>
void observe_task_completion (task_t<T> &task, TCallback &&callback)
{
    task._state->on_completed (
      std::function<void (const result_t<T> &)> (std::forward<TCallback> (callback)));
}

template <typename T> task_t<T> reschedule_task (task_t<T> task, task_scheduler_t scheduler)
{
    auto source = std::make_shared<task_completion_source_t<T>> ();
    auto output = source->task ();
    auto observed = std::make_shared<task_t<T>> (std::move (task));
    auto target_scheduler = std::make_shared<task_scheduler_t> (std::move (scheduler));
    detail::observe_task_completion (
      *observed, [source, observed, target_scheduler] (const result_t<T> &result) mutable {
          auto complete = [source, result] () mutable { source->complete (std::move (result)); };
          if (*target_scheduler) {
              (*target_scheduler) (std::move (complete));
          } else {
              complete ();
          }
      });
    return output;
}

template <typename T> task_t<T> unsupported_yield_task ()
{
    return task_t<T> (result_t<T>::failure (
      framework_error_kind_t::not_configured,
      "yield is available only in a SpotWide User Spot or Instance Spot callback"));
}


} // namespace detail

template <typename T, typename TCallback>
void observe_task_completion (task_t<T> &task, TCallback &&callback)
{
    detail::observe_task_completion (task, std::forward<TCallback> (callback));
}

} // namespace zlink::framework
