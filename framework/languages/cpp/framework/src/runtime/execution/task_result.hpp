/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <memory>
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

/* Ready results continue on the caller's stack; only an unfinished task gets a
 * continuation frame (#2418). `next` maps the value to the next task and is a
 * coroutine parameter, so its captures outlive every suspended await. */
template <typename R, typename T, typename Next>
task_t<R> continue_task (task_t<T> pending, Next next)
{
    if (!pending.await_ready ())
        return [] (task_t<T> pending, Next next) -> task_t<R> {
            co_return co_await next (co_await pending);
        }(std::move (pending), std::move (next));
    try {
        return next (pending.await_resume ());
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
    auto result = pending.result ();
    return result;
}

} // namespace zlink::framework::runtime
