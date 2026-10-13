/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

namespace
{
const int wait_ms = 5000;
const char payload[] = "events-preserve-data";
const char topic[] = "events";

int events (void *socket_)
{
    int value = 0;
    size_t size = sizeof (value);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_get_option (socket_, ZLINK_OPT_EVENTS, &value, &size));
    TEST_ASSERT_EQUAL_UINT (sizeof (value), size);
    TEST_ASSERT_EQUAL_INT (0, value & ~(ZLINK_POLLIN | ZLINK_POLLOUT));
    return value;
}

void configure (void *socket_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &wait_ms, sizeof (wait_ms)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &wait_ms, sizeof (wait_ms)));
}

void wait_pollin (void *socket_)
{
    zlink_pollitem_t item = {socket_, 0, ZLINK_POLLIN, 0};
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&item, 1, wait_ms, NULL));
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLIN, item.revents);
}

void expect_readable (void *socket_)
{
    wait_pollin (socket_);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLIN, events (socket_));
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLIN, events (socket_));
}

void expect_empty (void *socket_)
{
    TEST_ASSERT_BITS_LOW (ZLINK_POLLIN, events (socket_));
    zlink_pollitem_t item = {socket_, 0, ZLINK_POLLIN, 0};
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&item, 1, 0, NULL));
}

void make_payload (zlink_msg_t *part_)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (part_, sizeof (payload) - 1));
    memcpy (zlink_msg_data (part_), payload, sizeof (payload) - 1);
}

void expect_payload (zlink_msg_t *part_)
{
    TEST_ASSERT_EQUAL_UINT (sizeof (payload) - 1, zlink_msg_size (part_));
    TEST_ASSERT_EQUAL_MEMORY (payload, zlink_msg_data (part_), sizeof (payload) - 1);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (part_));
}

void run_data (zlink_socket_type_t receiver_type_)
{
    void *receiver = test_context_socket (receiver_type_);
    void *sender = test_context_socket (receiver_type_ == ZLINK_SOCKET_PAIR ? ZLINK_SOCKET_PAIR
                                                                            : ZLINK_SOCKET_DEALER);
    configure (receiver);
    configure (sender);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (receiver, "inproc://events-data"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (sender, "inproc://events-data"));
    expect_empty (receiver);
    zlink_msg_t sent;
    make_payload (&sent);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           zlink_send (sender, &sent, 1, ZLINK_SEND_FLAGS_NONE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&sent));
    expect_readable (receiver);
    zlink_msg_t received;
    size_t count = 0;
    const zlink_routing_id_t *rid = NULL;
    zlink_reply_token_t token = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      receiver_type_ == ZLINK_SOCKET_ROUTER
        ? zlink_router_recv (receiver, &rid, &token, &received, 1, &count,
                             ZLINK_RECV_FLAGS_DONTWAIT)
        : zlink_recv (receiver, &rid, &received, 1, &count, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT (1, count);
    TEST_ASSERT_EQUAL_UINT64 (0, token);
    expect_payload (&received);
    expect_empty (receiver);
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void receive_subscription (void *xpub_)
{
    const zlink_routing_id_t *rid = NULL;
    int subscribed = 0;
    char received_topic[sizeof (topic)];
    size_t length = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_xpub_recv (xpub_, &rid, &subscribed, received_topic,
                                                           sizeof (received_topic), &length,
                                                           ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (1, subscribed);
    TEST_ASSERT_EQUAL_UINT (sizeof (topic) - 1, length);
    TEST_ASSERT_EQUAL_MEMORY (topic, received_topic, length);
}

void run_subscription (zlink_socket_type_t type_)
{
    void *xpub = test_context_socket (ZLINK_SOCKET_XPUB);
    void *sub = test_context_socket (type_);
    configure (xpub);
    configure (sub);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (xpub, "inproc://events-subscription"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (sub, "inproc://events-subscription"));
    expect_empty (xpub);
    expect_empty (sub);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_subscription (sub, topic));
    wait_pollin (xpub);
    receive_subscription (xpub);
    expect_empty (xpub);
    zlink_msg_t sent;
    make_payload (&sent);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           zlink_publish (xpub, topic, &sent, 1, ZLINK_SEND_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&sent));
    expect_readable (sub);
    const zlink_routing_id_t *rid = NULL;
    char received_topic[sizeof (topic)];
    size_t length = 0;
    size_t count = 0;
    zlink_msg_t received;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK, zlink_subscribe (sub, &rid, received_topic, sizeof (received_topic), &length,
                                      &received, 1, &count, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT (sizeof (topic) - 1, length);
    TEST_ASSERT_EQUAL_MEMORY (topic, received_topic, length);
    TEST_ASSERT_EQUAL_UINT (1, count);
    expect_payload (&received);
    expect_empty (sub);
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (xpub);
}

void run_xpub ()
{
    void *xpub = test_context_socket (ZLINK_SOCKET_XPUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    configure (xpub);
    configure (sub);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (xpub, "inproc://events-xpub"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (sub, "inproc://events-xpub"));
    expect_empty (xpub);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_subscription (sub, topic));
    expect_readable (xpub);
    receive_subscription (xpub);
    expect_empty (xpub);
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (xpub);
}

void run_stream (zlink_stream_recv_mode_t mode_)
{
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    configure (stream);
    const int notify = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (stream, ZLINK_STREAM_OPT_NOTIFY, &notify, sizeof (notify)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (stream, ZLINK_STREAM_OPT_RECV_MODE, &mode_, sizeof (mode_)));
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (stream, endpoint, sizeof (endpoint));
    const fd_t client = connect_socket (endpoint);
    TEST_ASSERT_TRUE (client != retired_fd);
    expect_empty (stream);
    if (mode_ == ZLINK_STREAM_RECV_MODE_PACKET) {
        // Packet wire lengths are u16 header size followed by u32 body size.
        const unsigned char lengths[sizeof (uint16_t) + sizeof (uint32_t)] = {
          0, 0, 0, 0, 0, sizeof (payload) - 1};
        TEST_ASSERT_EQUAL_INT (
          sizeof (lengths),
          send (client, reinterpret_cast<const char *> (lengths), sizeof (lengths), 0));
    }
    TEST_ASSERT_EQUAL_INT (sizeof (payload) - 1, send (client, payload, sizeof (payload) - 1, 0));
    expect_readable (stream);
    const zlink_routing_id_t *rid = NULL;
    zlink_msg_t received;
    if (mode_ == ZLINK_STREAM_RECV_MODE_PACKET) {
        zlink_msg_t header;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&header));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&received));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_stream_recv_packet (stream, &rid, &header, &received, ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_UINT (0, zlink_msg_size (&header));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&header));
    } else {
        size_t count = 0;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_recv (stream, &rid, &received, 1, &count,
                                                          ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_UINT (1, count);
    }
    expect_payload (&received);
    expect_empty (stream);
    close (client);
    test_context_socket_close_zero_linger (stream);
}
}

void setUp ()
{
    setup_test_context ();
}
void tearDown ()
{
    teardown_test_context ();
}
void test_pair ()
{
    run_data (ZLINK_SOCKET_PAIR);
}
void test_dealer ()
{
    run_data (ZLINK_SOCKET_DEALER);
}
void test_router ()
{
    run_data (ZLINK_SOCKET_ROUTER);
}
void test_sub ()
{
    run_subscription (ZLINK_SOCKET_SUB);
}
void test_xsub ()
{
    run_subscription (ZLINK_SOCKET_XSUB);
}
void test_xpub ()
{
    run_xpub ();
}
void test_stream_raw ()
{
    run_stream (ZLINK_STREAM_RECV_MODE_RAW);
}
void test_stream_packet ()
{
    run_stream (ZLINK_STREAM_RECV_MODE_PACKET);
}
void test_pub_has_no_input ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    expect_empty (pub);
    test_context_socket_close_zero_linger (pub);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_pair);
    RUN_TEST (test_dealer);
    RUN_TEST (test_router);
    RUN_TEST (test_sub);
    RUN_TEST (test_xsub);
    RUN_TEST (test_xpub);
    RUN_TEST (test_stream_raw);
    RUN_TEST (test_stream_packet);
    RUN_TEST (test_pub_has_no_input);
    return UNITY_END ();
}
