/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <chrono>
#include <functional>
#include <thread>

namespace zlink::framework::test::host_lifecycle
{
inline bool wait_until (const std::function<bool ()> &condition, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (!condition () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    return condition ();
}
}
