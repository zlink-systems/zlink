/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <unity.h>
#include <cstring>

void setUp ()
{
    setup_test_context ();
}

void tearDown ()
{
    teardown_test_context ();
}

namespace
{
void *create_sync_socket (int type_)
{
    void *socket = zlink_socket (get_test_context (), static_cast<zlink_socket_type_t> (type_));
    TEST_ASSERT_NOT_NULL (socket);
    return socket;
}

void close_sync_socket (void *socket_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_));
}
}

static void test_pubsub_filter_transport (const char *endpoint_)
{
    void *pub = create_sync_socket (ZLINK_SOCKET_PUB);
    void *sub = create_sync_socket (ZLINK_SOCKET_SUB);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, endpoint_));

    char connect_endpoint[MAX_SOCKET_STRING];
    if (strncmp (endpoint_, "tcp://", 6) == 0 || strncmp (endpoint_, "ipc://", 6) == 0) {
        size_t len = sizeof (connect_endpoint);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_get_option (pub, ZLINK_OPT_LAST_ENDPOINT, connect_endpoint, &len));
    } else {
        strcpy (connect_endpoint, endpoint_);
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, connect_endpoint));

    // Subscribe only to "topicA"
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "topicA"));

    msleep (SETTLE_TIME);

    // Send messages with different topics
    send_string_expect_success (pub, "topicA hello", 0);
    send_string_expect_success (pub, "topicB world", 0);
    send_string_expect_success (pub, "topicA test", 0);

    // Should only receive topicA messages
    recv_string_expect_success (sub, "topicA hello", 0);
    recv_string_expect_success (sub, "topicA test", 0);

    close_sync_socket (sub);
    close_sync_socket (pub);
}

void test_pubsub_filter_tcp ()
{
    test_pubsub_filter_transport ("tcp://127.0.0.1:*");
}

void test_pubsub_xpub_xsub_inproc ()
{
    void *xpub = create_sync_socket (ZLINK_SOCKET_XPUB);
    void *xsub = create_sync_socket (ZLINK_SOCKET_XSUB);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (xpub, "inproc://test_xpub_xsub"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (xsub, "inproc://test_xpub_xsub"));

    // XSUB subscribe
    char sub_msg[] = {0x01, 0};
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (xsub, sub_msg, 1, 0));

    // Wait for subscription to propagate
    char sub_recv[16];
    const int sub_size =
      TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (xpub, sub_recv, sizeof (sub_recv), 0));
    TEST_ASSERT_TRUE (sub_size >= 1);
    TEST_ASSERT_EQUAL_HEX8 (0x01, (unsigned char) sub_recv[0]);

    // Test message flow
    const char *msg = "xpub_xsub_test";
    send_string_expect_success (xpub, msg, 0);
    recv_string_expect_success (xsub, msg, 0);

    close_sync_socket (xsub);
    close_sync_socket (xpub);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_pubsub_filter_tcp);
    RUN_TEST (test_pubsub_xpub_xsub_inproc);
    return UNITY_END ();
}
