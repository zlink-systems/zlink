/* SPDX-License-Identifier: MPL-2.0 */

/*
 * Test suite for the ASIO TCP listener and connecter
 *
 * These tests verify that the ASIO-based async_accept and async_connect
 * work correctly for connection establishment.
 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <unity.h>

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO

#include <string.h>

void setUp ()
{
    setup_test_context ();
}

void tearDown ()
{
    teardown_test_context ();
}

// Test 1: Basic connect/disconnect
void test_connect_disconnect ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    msleep (SETTLE_TIME);

    //  Verify connection works by sending a message
    const char *msg = "Hello ASIO connect!";
    send_string_expect_success (server, msg, 0);
    recv_string_expect_success (client, msg, 0);

    test_context_socket_close (client);
    test_context_socket_close (server);
}

// Test 2: Multiple rapid connect/disconnect cycles
void test_connect_stress ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int iterations = 20;
    for (int i = 0; i < iterations; i++) {
        void *client = test_context_socket (ZLINK_SOCKET_PAIR);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
        msleep (10);
        test_context_socket_close_zero_linger (client);
    }

    test_context_socket_close (server);
}

// Test 3: Connect before bind (reconnect behavior)
void test_connect_before_bind ()
{
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);

    //  Set reconnect interval
    int reconnect_ivl = 100;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_RECONNECT_IVL, &reconnect_ivl, sizeof (int)));

    //  Connect first (before server binds)
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, "tcp://127.0.0.1:15560"));

    msleep (50);

    //  Now bind the server
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "tcp://127.0.0.1:15560"));

    //  Wait for reconnect
    msleep (300);

    //  Verify connection established
    const char *msg = "reconnected!";
    send_string_expect_success (server, msg, 0);
    recv_string_expect_success (client, msg, 0);

    test_context_socket_close (client);
    test_context_socket_close (server);
}

// Test 4: Multiple clients connecting to same server
void test_multiple_clients ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int num_clients = 5;
    void *clients[num_clients];

    //  Create and connect all clients
    for (int i = 0; i < num_clients; i++) {
        clients[i] = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (clients[i], endpoint));
    }

    msleep (SETTLE_TIME);

    //  Each client sends a message
    for (int i = 0; i < num_clients; i++) {
        char msg[32];
        snprintf (msg, sizeof (msg), "Message from client %d", i);
        send_string_expect_success (clients[i], msg, 0);
    }

    //  Server receives all messages (with identity frame)
    for (int i = 0; i < num_clients; i++) {
        zlink_msg_t identity, msg;
        zlink_msg_init (&identity);
        zlink_msg_init (&msg);

        TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&identity, server, 0));
        TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&msg, server, 0));

        zlink_msg_close (&identity);
        zlink_msg_close (&msg);
    }

    //  Clean up
    for (int i = 0; i < num_clients; i++) {
        test_context_socket_close (clients[i]);
    }
    test_context_socket_close (server);
}

// Test 5: IPv6 connection (if supported)
void test_ipv6_connect ()
{
    if (!is_ipv6_available ()) {
        TEST_IGNORE_MESSAGE ("IPv6 not available, skipping test");
        return;
    }

    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    int ipv6 = 1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_IPV6, &ipv6, sizeof (int)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_IPV6, &ipv6, sizeof (int)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "tcp://[::1]:15561"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, "tcp://[::1]:15561"));

    msleep (SETTLE_TIME);

    const char *msg = "IPv6 test";
    send_string_expect_success (server, msg, 0);
    recv_string_expect_success (client, msg, 0);

    test_context_socket_close (client);
    test_context_socket_close (server);
}

// Test 6: Connect with immediate flag (should not block)
void test_connect_immediate ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    int immediate = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_IMMEDIATE, &immediate, sizeof (int)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    msleep (SETTLE_TIME);

    const char *msg = "immediate test";
    send_string_expect_success (server, msg, 0);
    recv_string_expect_success (client, msg, 0);

    test_context_socket_close (client);
    test_context_socket_close (server);
}

// Test 7: Wildcard port binding
void test_wildcard_port ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    //  Bind to wildcard port
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "tcp://127.0.0.1:*"));

    //  Get the actual bound endpoint
    char endpoint[MAX_SOCKET_STRING];
    size_t endpoint_len = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_len));

    //  Connect using the resolved endpoint
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    msleep (SETTLE_TIME);

    const char *msg = "wildcard test";
    send_string_expect_success (server, msg, 0);
    recv_string_expect_success (client, msg, 0);

    test_context_socket_close (client);
    test_context_socket_close (server);
}

// Test 8: Connection with TCP keepalive options
void test_tcp_keepalive ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    //  Set TCP keepalive options
    int tcp_keepalive = 1;
    int tcp_keepalive_idle = 100;
    int tcp_keepalive_cnt = 5;
    int tcp_keepalive_intvl = 10;

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TCP_KEEPALIVE, &tcp_keepalive, sizeof (int)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TCP_KEEPALIVE_IDLE, &tcp_keepalive_idle, sizeof (int)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TCP_KEEPALIVE_CNT, &tcp_keepalive_cnt, sizeof (int)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TCP_KEEPALIVE_INTVL, &tcp_keepalive_intvl, sizeof (int)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    msleep (SETTLE_TIME);

    const char *msg = "keepalive test";
    send_string_expect_success (server, msg, 0);
    recv_string_expect_success (client, msg, 0);

    test_context_socket_close (client);
    test_context_socket_close (server);
}

#else // !ZLINK_IOTHREAD_POLLER_USE_ASIO

void setUp ()
{
}

void tearDown ()
{
}

void test_asio_connect_not_enabled ()
{
    //  Skip tests when Asio poller is not enabled
    TEST_IGNORE_MESSAGE ("Asio poller not enabled, skipping tests");
}

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO
    RUN_TEST (test_connect_disconnect);
    RUN_TEST (test_connect_stress);
    RUN_TEST (test_connect_before_bind);
    RUN_TEST (test_multiple_clients);
    RUN_TEST (test_ipv6_connect);
    RUN_TEST (test_connect_immediate);
    RUN_TEST (test_wildcard_port);
    RUN_TEST (test_tcp_keepalive);
#else
    RUN_TEST (test_asio_connect_not_enabled);
#endif

    return UNITY_END ();
}
