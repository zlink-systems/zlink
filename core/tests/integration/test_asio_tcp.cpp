/* SPDX-License-Identifier: MPL-2.0 */

/*
 * Test suite for the ASIO TCP engine (True Proactor Mode)
 *
 * These tests verify that the ASIO-based engine using async_read/async_write
 * works correctly for ZMTP message exchange.
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

// Test 1: Basic message send/receive with PAIR sockets
void test_pair_basic_message ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    msleep (SETTLE_TIME);

    //  Test bidirectional communication
    const char *msg1 = "Hello from server";
    const char *msg2 = "Hello from client";

    send_string_expect_success (server, msg1, 0);
    recv_string_expect_success (client, msg1, 0);

    send_string_expect_success (client, msg2, 0);
    recv_string_expect_success (server, msg2, 0);

    test_context_socket_close (client);
    test_context_socket_close (server);
}

// Test 2: Multi-part message
void test_multipart_message ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    msleep (SETTLE_TIME);

    //  Send multi-part message
    const char *part1 = "Part 1";
    const char *part2 = "Part 2";
    const char *part3 = "Part 3";

    send_string_expect_success (server, part1, ZLINK_SNDMORE);
    send_string_expect_success (server, part2, ZLINK_SNDMORE);
    send_string_expect_success (server, part3, 0);

    //  Receive and verify all parts
    zlink_msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
    TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&msg, client, 0));
    TEST_ASSERT_EQUAL_STRING (part1, static_cast<const char *> (zlink_msg_data (&msg)));
    TEST_ASSERT_TRUE (test_msg_has_more (&msg));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
    TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&msg, client, 0));
    TEST_ASSERT_EQUAL_STRING (part2, static_cast<const char *> (zlink_msg_data (&msg)));
    TEST_ASSERT_TRUE (test_msg_has_more (&msg));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
    TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&msg, client, 0));
    TEST_ASSERT_EQUAL_STRING (part3, static_cast<const char *> (zlink_msg_data (&msg)));
    TEST_ASSERT_FALSE (test_msg_has_more (&msg));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));

    test_context_socket_close (client);
    test_context_socket_close (server);
}


// Test 3: Large message transfer
void test_large_message ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    msleep (SETTLE_TIME);

    //  Create a large message (1 MB)
    const size_t msg_size = 1024 * 1024;
    char *large_msg = static_cast<char *> (malloc (msg_size));
    TEST_ASSERT_NOT_NULL (large_msg);

    //  Fill with pattern
    for (size_t i = 0; i < msg_size; i++) {
        large_msg[i] = static_cast<char> ('A' + (i % 26));
    }

    //  Send large message
    TEST_ASSERT_EQUAL_INT (static_cast<int> (msg_size),
                           zlink_send (server, large_msg, msg_size, 0));

    //  Receive and verify
    char *recv_buf = static_cast<char *> (malloc (msg_size));
    TEST_ASSERT_NOT_NULL (recv_buf);

    TEST_ASSERT_EQUAL_INT (static_cast<int> (msg_size), zlink_recv (client, recv_buf, msg_size, 0));

    TEST_ASSERT_EQUAL_MEMORY (large_msg, recv_buf, msg_size);

    free (large_msg);
    free (recv_buf);

    test_context_socket_close (client);
    test_context_socket_close (server);
}

// Test 4: PUB/SUB message pattern
void test_pubsub_pattern ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (pub, endpoint, sizeof (endpoint));

    //  Subscribe to all messages
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));

    //  Wait for subscription to propagate
    msleep (SETTLE_TIME * 2);

    //  Send messages
    const char *topics[] = {"Topic A", "Topic B", "Topic C"};
    for (int i = 0; i < 3; i++) {
        send_string_expect_success (pub, topics[i], 0);
    }

    //  Receive messages
    for (int i = 0; i < 3; i++) {
        recv_string_expect_success (sub, topics[i], 0);
    }

    test_context_socket_close (sub);
    test_context_socket_close (pub);
}

// Test 5: DEALER/ROUTER message pattern
void test_dealer_router_pattern ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    //  Set identity for dealer
    const char *identity = "TestDealer";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, identity, strlen (identity)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

    msleep (SETTLE_TIME);

    //  Dealer sends message
    const char *msg = "Request from dealer";
    send_string_expect_success (dealer, msg, 0);

    //  Router receives message with identity
    zlink_msg_t recv_identity, recv_msg;
    zlink_msg_init (&recv_identity);
    zlink_msg_init (&recv_msg);

    TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&recv_identity, router, 0));
    TEST_ASSERT_EQUAL_INT (static_cast<int> (strlen (identity)), zlink_msg_size (&recv_identity));

    TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&recv_msg, router, 0));
    TEST_ASSERT_EQUAL_INT (static_cast<int> (strlen (msg)), zlink_msg_size (&recv_msg));

    //  Router replies to dealer
    const char *reply = "Reply from router";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (router, zlink_msg_data (&recv_identity),
                                           zlink_msg_size (&recv_identity), ZLINK_SNDMORE));
    send_string_expect_success (router, reply, 0);

    //  Dealer receives reply
    recv_string_expect_success (dealer, reply, 0);

    zlink_msg_close (&recv_identity);
    zlink_msg_close (&recv_msg);

    test_context_socket_close (dealer);
    test_context_socket_close (router);
}

// Test 6: Multiple message burst
void test_message_burst ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    msleep (SETTLE_TIME);

    //  Send burst of messages
    const int num_messages = 100;
    for (int i = 0; i < num_messages; i++) {
        char msg[32];
        snprintf (msg, sizeof (msg), "Message %d", i);
        send_string_expect_success (server, msg, 0);
    }

    //  Receive all messages
    for (int i = 0; i < num_messages; i++) {
        char expected[32];
        snprintf (expected, sizeof (expected), "Message %d", i);
        recv_string_expect_success (client, expected, 0);
    }

    test_context_socket_close (client);
    test_context_socket_close (server);
}

// Test 7: Non-blocking send/receive
void test_nonblocking_io ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    msleep (SETTLE_TIME);

    //  Non-blocking receive should return -1 with EAGAIN when no message
    char buf[32];
    int rc = zlink_recv (client, buf, sizeof (buf), ZLINK_DONTWAIT);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    //  Send a message
    const char *msg = "Non-blocking test";
    send_string_expect_success (server, msg, 0);

    //  Now receive should succeed
    msleep (10);
    recv_string_expect_success (client, msg, 0);

    test_context_socket_close (client);
    test_context_socket_close (server);
}

// Test 8: XPUB/XSUB pattern with subscription message
void test_xpub_xsub_pattern ()
{
    void *xpub = test_context_socket (ZLINK_SOCKET_XPUB);
    void *xsub = test_context_socket (ZLINK_SOCKET_XSUB);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (xpub, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (xsub, endpoint));

    msleep (SETTLE_TIME);

    //  Subscribe via XSUB (send subscription message)
    const char sub_msg[] = {1, 'A'}; //  Subscribe to "A"
    TEST_ASSERT_EQUAL_INT (2, zlink_send (xsub, sub_msg, sizeof (sub_msg), 0));

    //  XPUB receives subscription notification
    char sub_recv[32];
    int sub_size = zlink_recv (xpub, sub_recv, sizeof (sub_recv), 0);
    TEST_ASSERT_EQUAL_INT (2, sub_size);
    TEST_ASSERT_EQUAL_INT (1, sub_recv[0]); //  Subscribe
    TEST_ASSERT_EQUAL_INT ('A', sub_recv[1]);

    //  Publish message
    const char *topic_msg = "ABC";
    send_string_expect_success (xpub, topic_msg, 0);

    //  XSUB receives message
    recv_string_expect_success (xsub, topic_msg, 0);

    test_context_socket_close (xsub);
    test_context_socket_close (xpub);
}

// Test 9: High-water mark behavior
void test_hwm_behavior ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    //  Set low HWM
    const int hwm_message_count = 5;
    const uint64_t hwm = hwm_message_count * (32u + sizeof (zlink_msg_t));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    msleep (SETTLE_TIME);

    //  Simple test: send and receive within HWM
    for (int i = 0; i < hwm_message_count; i++) {
        char msg[32];
        snprintf (msg, sizeof (msg), "HWM msg %d", i);
        send_string_expect_success (server, msg, 0);
    }

    for (int i = 0; i < hwm; i++) {
        char expected[32];
        snprintf (expected, sizeof (expected), "HWM msg %d", i);
        recv_string_expect_success (client, expected, 0);
    }

    test_context_socket_close (client);
    test_context_socket_close (server);
}

// Test 10: Socket bounce (full duplex test)
void test_socket_bounce ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    msleep (SETTLE_TIME);

    //  Use the standard bounce test
    bounce (server, client);

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

void test_asio_tcp_not_enabled ()
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
    RUN_TEST (test_pair_basic_message);
    RUN_TEST (test_multipart_message);
    RUN_TEST (test_large_message);
    RUN_TEST (test_pubsub_pattern);
    RUN_TEST (test_dealer_router_pattern);
    RUN_TEST (test_message_burst);
    RUN_TEST (test_nonblocking_io);
    RUN_TEST (test_xpub_xsub_pattern);
    RUN_TEST (test_hwm_behavior);
    RUN_TEST (test_socket_bounce);
#else
    RUN_TEST (test_asio_tcp_not_enabled);
#endif

    return UNITY_END ();
}
