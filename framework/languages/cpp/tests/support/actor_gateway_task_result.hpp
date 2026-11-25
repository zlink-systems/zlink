/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace zlink::framework::test::actor_gateway
{
template <typename T>
std::optional<zlink::framework::result_t<T>>
finite_task_result (zlink::framework::task_t<T> task,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds (500))
{
    struct wait_state_t
    {
        std::mutex mutex;
        std::condition_variable ready;
        std::optional<zlink::framework::result_t<T>> result;
    };

    auto wait_state = std::make_shared<wait_state_t> ();
    auto observed = std::make_shared<zlink::framework::task_t<T>> (std::move (task));
    zlink::framework::detail::observe_task_completion (
      *observed, [wait_state, observed] (const zlink::framework::result_t<T> &result) {
          {
              const std::lock_guard lock (wait_state->mutex);
              wait_state->result = result;
          }
          wait_state->ready.notify_one ();
      });
    std::unique_lock lock (wait_state->mutex);
    if (!wait_state->ready.wait_for (lock, timeout,
                                     [&] { return wait_state->result.has_value (); })) {
        return std::nullopt;
    }
    return wait_state->result;
}
} // namespace zlink::framework::test::actor_gateway
