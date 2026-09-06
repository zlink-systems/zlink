/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil.hpp"
#include "testutil_unity.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS && defined ZLINK_HAVE_WS
#include "transports/ws/ws_transport.hpp"
#if defined ZLINK_HAVE_WSS
#include "transports/tls/wss_transport.hpp"
#endif
#endif
#include <array>
#include <atomic>
#include <thread>
#include <vector>

void setUp () {}
void tearDown () {}
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS && defined ZLINK_HAVE_WS
void test_ws_transport_config_initialization_is_thread_safe ()
{
    const size_t worker_count = 16;
    std::atomic<size_t> ready (0);
    std::atomic<bool> start (false);
    std::vector<std::array<size_t, 4> > values (worker_count);
    std::vector<std::thread> workers;
    workers.reserve (worker_count);

    for (size_t i = 0; i != worker_count; ++i) {
        workers.push_back (std::thread ([&, i] () {
            ready.fetch_add (1, std::memory_order_release);
            while (!start.load (std::memory_order_acquire))
                std::this_thread::yield ();

            values[i][0] = zlink::test_ws_write_buffer_bytes ();
            values[i][1] = zlink::test_ws_read_message_max ();
#if defined ZLINK_HAVE_WSS
            values[i][2] = zlink::test_wss_write_buffer_bytes ();
            values[i][3] = zlink::test_wss_read_message_max ();
#else
            values[i][2] = 0;
            values[i][3] = 0;
#endif
        }));
    }

    while (ready.load (std::memory_order_acquire) != worker_count)
        std::this_thread::yield ();
    start.store (true, std::memory_order_release);

    for (size_t i = 0; i != workers.size (); ++i)
        workers[i].join ();

    TEST_ASSERT_GREATER_THAN_UINT64 (0, values[0][0]);
    TEST_ASSERT_GREATER_THAN_UINT64 (0, values[0][1]);
    for (size_t i = 1; i != worker_count; ++i) {
        TEST_ASSERT_EQUAL_UINT64 (values[0][0], values[i][0]);
        TEST_ASSERT_EQUAL_UINT64 (values[0][1], values[i][1]);
#if defined ZLINK_HAVE_WSS
        TEST_ASSERT_EQUAL_UINT64 (values[0][2], values[i][2]);
        TEST_ASSERT_EQUAL_UINT64 (values[0][3], values[i][3]);
#endif
    }
}

#endif
int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS && defined ZLINK_HAVE_WS
    RUN_TEST (test_ws_transport_config_initialization_is_thread_safe);
#endif
    return UNITY_END ();
}
