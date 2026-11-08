/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/channels/call.hpp>
#include <zlink/framework/detail/deadline_scheduler.hpp>

#include <memory>
#include <type_traits>
#include <utility>

namespace zlink::framework::runtime
{

template <typename T> task_t<result_t<T>> await_result (task_t<T> pending)
{
    auto completion = std::make_shared<task_completion_source_t<result_t<T>>> ();
    auto task = completion->task ();
    detail::observe_task_completion (pending, [completion] (const result_t<T> &result) {
        completion->complete (result_t<result_t<T>>::success (result));
    });
    return task;
}

// Lifecycle entry can create the first completion owner. Hand off the named
// coroutine frame directly: registering a task wait here would itself require
// that owner before the lifecycle operation has created it.
struct external_lifecycle_turn_t
{
    bool await_ready () const noexcept { return false; }
    void await_suspend (std::coroutine_handle<> continuation) const
    {
        auto ambient = detail::capture_ambient_context ();
        if (!detail::submit_blocking_call ([ambient = std::move (ambient), continuation] {
                const auto scope = detail::enter_ambient_context (ambient, false);
                continuation.resume ();
            })) {
            throw framework_exception_t (framework_error_kind_t::shutting_down,
                                         "Lifecycle executor is stopped");
        }
    }
    void await_resume () const noexcept {}
};

// Bounds the observation, not opaque provider I/O. Losing completions retain no
// runtime or coroutine owner and cannot resume the observer a second time.
template <typename T, typename Now = decltype (&std::chrono::steady_clock::now)>
task_t<result_t<T>> observe_until (task_t<T> pending,
                                   std::chrono::steady_clock::time_point deadline_at,
                                   std::stop_token cancellation = {},
                                   Now now = std::chrono::steady_clock::now)
{
    const auto observed = [deadline_at, cancellation, now = std::move (now)] (result_t<T> result) {
        if (cancellation.stop_requested ())
            return result_t<T>::failure (framework_error_kind_t::shutting_down,
                                         "Location coordinator stopped");
        if (now () >= deadline_at)
            return result_t<T>::failure (framework_error_kind_t::deadline_exceeded,
                                         "Location coordinator wait timed out");
        return result;
    };
    if (cancellation.stop_requested ())
        co_return observed (result_t<T>::failure (framework_error_kind_t::shutting_down,
                                                  "Location coordinator stopped"));
    if (pending.await_ready ())
        co_return observed (detail::task_result_access_t::read (pending));
    auto completion = std::make_shared<task_completion_source_t<result_t<T>>> ();
    auto deadline = std::make_shared<detail::deadline_state_t> ();
    auto settle = [weak = std::weak_ptr{completion}, observed] (result_t<T> result) {
        if (auto source = weak.lock ())
            source->complete (result_t<result_t<T>>::success (observed (std::move (result))));
    };
    std::stop_callback stopped (cancellation, [settle] {
        settle (result_t<T>::failure (framework_error_kind_t::shutting_down,
                                      "Location coordinator stopped"));
    });
    detail::observe_task_terminal (pending, settle);
    detail::deadline_scheduler_t::instance ().schedule (deadline_at, deadline, [settle] {
        settle (result_t<T>::failure (framework_error_kind_t::deadline_exceeded,
                                      "Location coordinator wait timed out"));
    });
    auto result = co_await completion->task ();
    deadline->cancelled.store (true, std::memory_order_release);
    detail::deadline_scheduler_t::instance ().wake ();
    co_return result;
}

/* Ready results continue on the caller's stack; only an unfinished task gets a
 * continuation frame (#2418). `next` maps the value to the next task and is a
 * coroutine parameter, so its captures outlive every suspended await. */
template <typename R, typename T, typename Next>
task_t<R> continue_task (task_t<T> pending, Next next)
{
    if (!pending.await_ready ())
        return [] (task_t<T> pending, Next next) -> task_t<R> {
            if constexpr (std::is_void_v<T>) {
                co_await pending;
                co_return co_await next ();
            } else {
                co_return co_await next (co_await pending);
            }
        }(std::move (pending), std::move (next));
    try {
        if constexpr (std::is_void_v<T>) {
            pending.await_resume ();
            return next ();
        } else {
            return next (pending.await_resume ());
        }
    }
    catch (...) {
        return task_t<R> (detail::current_exception_result<R> ());
    }
}

/* The one blocking wait for a Location or Relocation Store chain on an
 * infrastructure thread (execution gate §1, §13). `start` begins the chain
 * inside the scope, so every continuation it registers resumes on the thread
 * that completes the Store step instead of the handler executor; the wait
 * then does not depend on a free handler worker. */
template <typename Start> auto infrastructure_result (Start &&start)
{
    struct scope_t
    {
        bool previous = detail::exchange_infrastructure_wait (true);
        ~scope_t () { detail::exchange_infrastructure_wait (previous); }
    } scope;
    auto pending = std::forward<Start> (start) ();
    auto result = detail::task_result_access_t::read (pending);
    return result;
}

} // namespace zlink::framework::runtime
