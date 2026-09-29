/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <memory>
#include <utility>

namespace zlink::framework::runtime
{

template <typename T> task_t<result_t<T>> await_result (task_t<T> pending)
{
    auto completion = std::make_shared<detail::task_completion_source_t<result_t<T>>> ();
    auto task = completion->task ();
    detail::observe_task_completion (pending, [completion] (const result_t<T> &result) {
        completion->complete (result_t<result_t<T>>::success (result));
    });
    return task;
}

} // namespace zlink::framework::runtime
