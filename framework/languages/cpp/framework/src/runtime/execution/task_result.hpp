/* SPDX-License-Identifier: FSL-1.1-ALv2 */
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
