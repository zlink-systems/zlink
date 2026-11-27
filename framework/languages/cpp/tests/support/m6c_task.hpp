/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <chrono>
#include <thread>
#include <utility>

namespace m6c_test
{

template <class T> T await_task (zlink::framework::task_t<T> task)
{
    return std::move (task).result ().value ();
}

void await_task (zlink::framework::task_t<void> task);

template <typename Predicate>
bool wait_until_bounded (Predicate &&condition, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (condition ())
            return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return condition ();
}

} // namespace m6c_test
