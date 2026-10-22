/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <functional>
#include <memory>
#include <utility>

namespace zlink::framework::runtime
{

/* Starts the task that `work` returns and reports its terminal result.
 *
 * A coroutine lambda's frame refers to the lambda's captures by the address of
 * the closure object. std::function may store that closure in its own buffer,
 * and moving the std::function then moves the closure to another address. The
 * callable therefore stays in the shared object passed here from the call
 * until the task reaches its terminal state, and nothing moves it in between.
 *
 * An exception thrown by the call itself propagates to the caller. */
template <typename T, typename TCallback>
void start_owned_task (std::shared_ptr<std::function<task_t<T> ()>> work, TCallback on_terminal)
{
    auto running = std::make_shared<task_t<T>> ((*work) ());
    detail::observe_task_terminal (
      *running, [running, work = std::move (work), on_terminal = std::move (on_terminal)] (
                  const result_t<T> &result) mutable { on_terminal (result); });
}

} // namespace zlink::framework::runtime
