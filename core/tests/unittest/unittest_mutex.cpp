/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"

#include "utils/mutex.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
//  mutex_t is the plain (non-recursive) default. Only the recursive variant
//  admits a second acquisition on the owning thread.
void test_recursive_mutex_allows_same_thread_recursion ()
{
    zlink::recursive_mutex_t mutex;

    mutex.lock ();
    TEST_ASSERT_TRUE (mutex.try_lock ());
    mutex.unlock ();
    mutex.unlock ();
}

void test_recursive_mutex_blocks_other_threads_during_owner_hold ()
{
    zlink::recursive_mutex_t mutex;
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

void test_plain_mutex_refuses_second_acquisition_on_owner ()
{
    zlink::mutex_t mutex;

    mutex.lock ();
    //  A plain mutex never grants a second acquisition, not even to its owner.
    TEST_ASSERT_FALSE (mutex.try_lock ());
    mutex.unlock ();
}

void test_plain_mutex_blocks_other_threads_during_owner_hold ()
{
    zlink::mutex_t mutex;
    std::atomic<bool> worker_ready (false);
    std::atomic<bool> worker_acquired (false);

    mutex.lock ();

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
    worker.join ();
}

//  A recursive mutex is usable wherever a plain one is expected.
void test_recursive_mutex_is_usable_through_scoped_lock ()
{
    zlink::recursive_mutex_t mutex;
    {
        zlink::scoped_lock_t lock (mutex);
        TEST_ASSERT_TRUE (mutex.try_lock ());
        mutex.unlock ();
    }
    {
        zlink::scoped_optional_lock_t taken (&mutex);
        zlink::scoped_optional_lock_t skipped (NULL);
    }
    TEST_ASSERT_TRUE (mutex.try_lock ());
    mutex.unlock ();
}
} // namespace

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_recursive_mutex_allows_same_thread_recursion);
    RUN_TEST (test_recursive_mutex_blocks_other_threads_during_owner_hold);
    RUN_TEST (test_plain_mutex_refuses_second_acquisition_on_owner);
    RUN_TEST (test_plain_mutex_blocks_other_threads_during_owner_hold);
    RUN_TEST (test_recursive_mutex_is_usable_through_scoped_lock);
    return UNITY_END ();
}
