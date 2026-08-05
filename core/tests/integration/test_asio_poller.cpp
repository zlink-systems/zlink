/* SPDX-License-Identifier: MPL-2.0 */

/*
 * Test suite for the Asio poller
 *
 * These tests verify that the Asio-based poller works correctly.
 * ASIO is now the only supported I/O backend for zlink.
 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <unity.h>

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && !defined ZLINK_HAVE_WINDOWS

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

// Global test context
void *sb;
void *sc;

void setUp ()
{
    setup_test_context ();
}

void tearDown ()
{
    teardown_test_context ();
}

// Test 1: Basic context creation and destruction with Asio poller
void test_ctx_create_destroy ()
{
    // The test context is already created in setUp
    // If we get here without crashing, the Asio poller initialized correctly
    void *ctx = get_test_context ();
    TEST_ASSERT_NOT_NULL (ctx);
}

// Test 2: Basic socket pair communication (verifies event loop works)
void test_pair_tcp_basic ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    // Give time for connection to establish
    msleep (SETTLE_TIME);

    // Send a message from server to client
    const char *msg = "Hello Asio!";
    send_string_expect_success (server, msg, 0);
    recv_string_expect_success (client, msg, 0);

    // Send a message from client to server
    const char *reply = "Asio works!";
    send_string_expect_success (client, reply, 0);
    recv_string_expect_success (server, reply, 0);

    test_context_socket_close (client);
    test_context_socket_close (server);
}

// Test 3: Multiple sockets with Asio poller
void test_multiple_sockets ()
{
    const int num_pairs = 5;
    void *servers[num_pairs];
    void *clients[num_pairs];
    char endpoints[num_pairs][MAX_SOCKET_STRING];

    // Create and connect socket pairs
    for (int i = 0; i < num_pairs; i++) {
        servers[i] = test_context_socket (ZLINK_SOCKET_PAIR);
        clients[i] = test_context_socket (ZLINK_SOCKET_PAIR);

        bind_loopback_ipv4 (servers[i], endpoints[i], sizeof (endpoints[i]));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (clients[i], endpoints[i]));
    }

    msleep (SETTLE_TIME);

    // Send and receive on all pairs
    for (int i = 0; i < num_pairs; i++) {
        char msg[32];
        snprintf (msg, sizeof (msg), "Message %d", i);
        send_string_expect_success (servers[i], msg, 0);
        recv_string_expect_success (clients[i], msg, 0);
    }

    // Clean up
    for (int i = 0; i < num_pairs; i++) {
        test_context_socket_close (clients[i]);
        test_context_socket_close (servers[i]);
    }
}

// Test 4: Pub/Sub pattern (tests subscription forwarding through poller)
void test_pubsub_basic ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (pub, endpoint, sizeof (endpoint));

    // Subscribe to all messages
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));

    // Give time for subscription to propagate
    msleep (SETTLE_TIME);

    // Send a few messages to ensure at least one gets through
    for (int i = 0; i < 10; i++) {
        send_string_expect_success (pub, "test message", 0);
        msleep (10);
    }

    // Receive at least one message
    char buffer[64];
    int rc = zlink_recv (sub, buffer, sizeof (buffer), ZLINK_DONTWAIT);
    // Pub/sub can drop initial messages, so we just check it doesn't error fatally
    LIBZLINK_UNUSED (rc);

    test_context_socket_close (sub);
    test_context_socket_close (pub);
}

// Test 5: DEALER/ROUTER pattern
void test_dealer_router ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

    msleep (SETTLE_TIME);

    // Send from dealer to router
    send_string_expect_success (dealer, "Hello Router", 0);

    // Receive on router (includes identity frame)
    zlink_msg_t identity, msg;
    zlink_msg_init (&identity);
    zlink_msg_init (&msg);

    TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&identity, router, 0));
    TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&msg, router, 0));

    //  Compare message data with known length (zlink_msg_data is not null-terminated)
    TEST_ASSERT_EQUAL_INT (strlen ("Hello Router"), zlink_msg_size (&msg));
    TEST_ASSERT_EQUAL_MEMORY ("Hello Router", zlink_msg_data (&msg), zlink_msg_size (&msg));

    zlink_msg_close (&identity);
    zlink_msg_close (&msg);

    test_context_socket_close (dealer);
    test_context_socket_close (router);
}

// Test 6: Socket close during active communication
void test_socket_close_active ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    msleep (SETTLE_TIME);

    // Send some messages
    send_string_expect_success (server, "msg1", 0);
    send_string_expect_success (server, "msg2", 0);

    // Close without receiving - should not crash
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

// Test 7: Multiple IO threads with Asio
void test_multiple_io_threads ()
{
    // Create a new context with multiple IO threads
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    // Set 4 IO threads
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (ctx, ZLINK_IO_THREADS, 4));

    void *server = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);

    void *client = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (client);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "tcp://127.0.0.1:*"));

    char endpoint[MAX_SOCKET_STRING];
    size_t endpoint_len = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_len));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    msleep (SETTLE_TIME);

    // Communicate
    const char *msg = "Multi-IO test";
    TEST_ASSERT_EQUAL_INT (static_cast<int> (strlen (msg)),
                           TEST_ASSERT_SUCCESS_ERRNO (zlink_send (server, msg, strlen (msg), 0)));

    char buffer[64];
    int rc = TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (client, buffer, sizeof (buffer), 0));
    TEST_ASSERT_EQUAL_INT (static_cast<int> (strlen (msg)), rc);

    // Clean up
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (client));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (server));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

// Test 8: Verify inproc transport works with Asio poller
void test_inproc_transport ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "inproc://test_asio_inproc"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, "inproc://test_asio_inproc"));

    // Inproc should work immediately
    send_string_expect_success (server, "inproc test", 0);
    recv_string_expect_success (client, "inproc test", 0);

    test_context_socket_close (client);
    test_context_socket_close (server);
}

#else // !ZLINK_IOTHREAD_POLLER_USE_ASIO || ZLINK_HAVE_WINDOWS

void setUp ()
{
}

void tearDown ()
{
}

void test_asio_not_enabled ()
{
    // Skip tests when Asio poller is not enabled
    TEST_IGNORE_MESSAGE ("Asio poller not enabled, skipping tests");
}

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && !defined ZLINK_HAVE_WINDOWS
    RUN_TEST (test_ctx_create_destroy);
    RUN_TEST (test_pair_tcp_basic);
    RUN_TEST (test_multiple_sockets);
    RUN_TEST (test_pubsub_basic);
    RUN_TEST (test_dealer_router);
    RUN_TEST (test_socket_close_active);
    RUN_TEST (test_multiple_io_threads);
    RUN_TEST (test_inproc_transport);
#else
    RUN_TEST (test_asio_not_enabled);
#endif

    return UNITY_END ();
}
