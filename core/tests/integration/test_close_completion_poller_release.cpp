/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <cstdlib>
#include <future>
#include <thread>

void setUp ()
{
}

void tearDown ()
{
}

namespace
{
void close_with_pending_writable_wait (bool with_monitor_)
{
    for (int iteration = 0; iteration != 20; ++iteration) {
        void *ctx = zlink_ctx_new ();
        TEST_ASSERT_NOT_NULL (ctx);
        void *socket = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
        TEST_ASSERT_NOT_NULL (socket);
        const int linger = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (socket, ZLINK_OPT_LINGER, &linger, sizeof (linger)));

        void *monitor = NULL;
        if (with_monitor_) {
            zlink_socket_monitor_open_options_t options = {};
            options.events =
              ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
            monitor = zlink_socket_monitor_open (socket, &options);
            TEST_ASSERT_NOT_NULL (monitor);
        }

        void *poller = zlink_poller_new ();
        TEST_ASSERT_NOT_NULL (poller);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (poller, socket, socket, ZLINK_POLLCOMPLETION));

        zlink_msg_t msg;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&msg, 1));
        zlink_completion_id_t token = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_BACKPRESSURED,
          zlink_send_part (socket, &msg, ZLINK_SEND_FLAGS_DONTWAIT,
                           ZLINK_PART_FINAL, NULL, &token));
        TEST_ASSERT_TRUE (token != 0);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));
        if (monitor)
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                                   zlink_monitor_close (&monitor));

        std::promise<zlink_close_result_t> close_result;
        std::future<zlink_close_result_t> closed = close_result.get_future ();
        std::thread closer ([&] { close_result.set_value (zlink_close (socket)); });

        // POLLERR observes accepted close while the registration still pins
        // the socket. Releasing its completion owner must not restart it.
        zlink_poller_event_t event = {};
        zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
        TEST_ASSERT_EQUAL_INT (
          1, zlink_poller_wait (poller, &event, 1, 1000, &error));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        TEST_ASSERT_EQUAL_INT (ZLINK_POLLERR, event.events);
        TEST_ASSERT_EQUAL_PTR (socket, event.socket);
        TEST_ASSERT_EQUAL_INT (
          0, zlink_poller_wait (poller, &event, 1, 0, &error));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
        TEST_ASSERT_NULL (poller);

        if (closed.wait_for (std::chrono::seconds (1))
            != std::future_status::ready) {
            fprintf (stderr,
                     "close stuck after completion poller release: monitor=%d iteration=%d\n",
                     with_monitor_, iteration);
            // A stuck close thread cannot be joined or left using this fixture.
            std::abort ();
        }
        closer.join ();
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, closed.get ());
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (ctx));
    }
}

void test_close_completion_poller_release_with_monitor ()
{
    close_with_pending_writable_wait (true);
}

void test_close_completion_poller_release_without_monitor ()
{
    close_with_pending_writable_wait (false);
}
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_close_completion_poller_release_with_monitor);
    RUN_TEST (test_close_completion_poller_release_without_monitor);
    return UNITY_END ();
}
