/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"

#include <string.h>

namespace
{
void expect_monitor_sequence (test_monitor_probe_t *probe_,
                              const uint64_t *expected_,
                              int count_,
                              int timeout_ms_)
{
    int cursor = 0;
    for (int i = 0; i < count_; ++i) {
        int event_index = -1;
        TEST_ASSERT_TRUE (test_monitor_probe_wait_event_after (probe_, expected_[i], cursor,
                                                               timeout_ms_, &event_index));
        cursor = event_index + 1;
    }
}

bool endpoint_uses_ipv6 (const char *endpoint_)
{
    return endpoint_ && strchr (endpoint_, '[') != NULL;
}

void configure_socket_family_for_endpoint (void *socket_, const char *endpoint_)
{
    TEST_ASSERT_NOT_NULL (socket_);
    const int ipv6 = endpoint_uses_ipv6 (endpoint_) ? 1 : 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_IPV6, &ipv6, sizeof (ipv6)));
}

void run_reconnect_ivl_case (const char *bind_endpoint_, const char *connect_endpoint_)
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    configure_socket_family_for_endpoint (server, bind_endpoint_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, bind_endpoint_));

    void *client = test_context_socket (ZLINK_SOCKET_PAIR);
    configure_socket_family_for_endpoint (client, connect_endpoint_);
    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (client, ZLINK_EVENT_ALL, &probe);

    int interval = 100;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_RECONNECT_IVL, &interval, sizeof (interval)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, connect_endpoint_));

    const uint64_t initial_events[] = {ZLINK_EVENT_CONNECT_DELAYED, ZLINK_EVENT_CONNECTED,
                                       ZLINK_EVENT_CONNECTION_READY};
    expect_monitor_sequence (&probe, initial_events,
                             sizeof (initial_events) / sizeof (initial_events[0]), 3000);

    test_context_socket_close_zero_linger (server);

    int disconnected_index = -1;
    TEST_ASSERT_TRUE (test_monitor_probe_wait_event_after (&probe, ZLINK_EVENT_DISCONNECTED, 3,
                                                           3000, &disconnected_index));

    int retried_index = -1;
    TEST_ASSERT_TRUE (test_monitor_probe_wait_event_after (
      &probe, ZLINK_EVENT_CONNECT_RETRIED, disconnected_index + 1, 3000, &retried_index));

    bool saw_ready_reset = false;
    for (int i = disconnected_index + 1; i < retried_index; ++i) {
        if (test_monitor_probe_event_at (&probe, i) == ZLINK_EVENT_CONNECTION_READY) {
            saw_ready_reset = true;
            break;
        }
    }
    TEST_ASSERT_TRUE (saw_ready_reset);

    server = test_context_socket (ZLINK_SOCKET_PAIR);
    configure_socket_family_for_endpoint (server, connect_endpoint_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, connect_endpoint_));

    int reconnect_delayed_index = -1;
    TEST_ASSERT_TRUE (test_monitor_probe_wait_event_after (
      &probe, ZLINK_EVENT_CONNECT_DELAYED, retried_index + 1, 5000, &reconnect_delayed_index));

    int reconnect_connected_index = -1;
    TEST_ASSERT_TRUE (test_monitor_probe_wait_event_after (&probe, ZLINK_EVENT_CONNECTED,
                                                           reconnect_delayed_index + 1, 5000,
                                                           &reconnect_connected_index));

    int reconnect_ready_index = -1;
    TEST_ASSERT_TRUE (test_monitor_probe_wait_event_after (&probe, ZLINK_EVENT_CONNECTION_READY,
                                                           reconnect_connected_index + 1, 5000,
                                                           &reconnect_ready_index));

    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}
}

SETUP_TEARDOWN_TESTCONTEXT

#if defined(ZLINK_HAVE_IPC) && !defined(ZLINK_HAVE_GNU)
void test_reconnect_ivl_ipc (void)
{
    char my_endpoint[256];
    make_random_ipc_endpoint (my_endpoint);
    run_reconnect_ivl_case (my_endpoint, my_endpoint);
}
#endif

void test_reconnect_ivl_tcp_ipv4 ()
{
    char endpoint[MAX_SOCKET_STRING];
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    test_context_socket_close_zero_linger (server);
    run_reconnect_ivl_case (endpoint, endpoint);
}

void test_reconnect_ivl_tcp_ipv6 ()
{
    if (!is_ipv6_available ())
        TEST_FAIL_MESSAGE ("IPv6 must be available in this environment");

    char endpoint[MAX_SOCKET_STRING];
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    bind_loopback_ipv6 (server, endpoint, sizeof (endpoint));
    test_context_socket_close_zero_linger (server);
    run_reconnect_ivl_case (endpoint, endpoint);
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
#if defined(ZLINK_HAVE_IPC) && !defined(ZLINK_HAVE_GNU)
    RUN_TEST (test_reconnect_ivl_ipc);
#endif
    RUN_TEST (test_reconnect_ivl_tcp_ipv4);
    RUN_TEST (test_reconnect_ivl_tcp_ipv6);

    return UNITY_END ();
}
