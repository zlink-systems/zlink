/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"

#include "utils/fast_mutex.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
void test_fast_mutex_allows_same_thread_recursion ()
{
    zlink::fast_mutex_t mutex;

    mutex.lock ();
    TEST_ASSERT_TRUE (mutex.try_lock ());
    mutex.unlock ();
    mutex.unlock ();
}

void test_fast_mutex_blocks_other_threads_during_recursive_owner_hold ()
{
    zlink::fast_mutex_t mutex;
    std::atomic<bool> worker_ready (false);
    std::atomic<bool> worker_acquired (false);

    mutex.lock ();
    TEST_ASSERT_TRUE (mutex.try_lock ());

    std::thread worker ([&] {
        worker_ready.store (true, std::memory_order_release);
        worker_acquired.store (mutex.try_lock (), std::memory_order_release);
        if (worker_acquired.load (std::memory_order_acquire))
            mutex.unlock ();
    });

    while (!worker_ready.load (std::memory_order_acquire))
        std::this_thread::yield ();

    std::this_thread::sleep_for (std::chrono::milliseconds (25));
    TEST_ASSERT_FALSE (worker_acquired.load (std::memory_order_acquire));

    mutex.unlock ();
    mutex.unlock ();
    worker.join ();
}
} // namespace

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_fast_mutex_allows_same_thread_recursion);
    RUN_TEST (test_fast_mutex_blocks_other_threads_during_recursive_owner_hold);
    return UNITY_END ();
}
