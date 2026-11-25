/* SPDX-License-Identifier: Elastic-2.0 */

#include "execution_wait.hpp"
#include <chrono>
#include <thread>

namespace zlink::framework::test::execution
{
bool wait_until (const std::function<bool ()> &predicate)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (predicate ()) {
            return true;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    return false;
}
}
