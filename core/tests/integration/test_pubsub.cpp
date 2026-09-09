/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

SETUP_TEARDOWN_TESTCONTEXT

void test (const char *address)
{
    //  Create a publisher
    void *publisher = test_context_socket (ZLINK_SOCKET_PUB);
    char my_endpoint[MAX_SOCKET_STRING];

    //  Bind publisher
    test_bind (publisher, address, my_endpoint, MAX_SOCKET_STRING);

    //  Create a subscriber
    void *subscriber = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (subscriber, my_endpoint));

    //  Subscribe to all messages.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (subscriber, ""));

    //  Wait a bit till the subscription gets to the publisher
    msleep (SETTLE_TIME);

    //  Publish and receive the same payload through the public topic API.
    send_published_string_expect_success (publisher, "test", "test");

    //  Receive the message in the subscriber
    recv_subscribed_string_expect_success (subscriber, "test", "test");

    //  Clean up.
    test_context_socket_close (publisher);
    test_context_socket_close (subscriber);
}

void test_tcp ()
{
    test ("tcp://127.0.0.1:*");
}

void test_conflate_preserves_topic_and_payload ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    const int enabled = 1;
    const int timeout = 250;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      pub, ZLINK_OPT_CONFLATE, &enabled, sizeof (enabled)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      sub, ZLINK_OPT_CONFLATE, &enabled, sizeof (enabled)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      sub, ZLINK_OPT_RCVTIMEO, &timeout, sizeof (timeout)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, "inproc://conflate-record"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, "inproc://conflate-record"));
    send_published_string_expect_success (pub, "topic", "old");
    send_published_string_expect_success (pub, "other", "independent");
    send_published_string_expect_success (pub, "topic", "latest");
    recv_subscribed_string_expect_success (sub, "other", "independent");
    recv_subscribed_string_expect_success (sub, "topic", "latest");
    test_context_socket_close (sub);
    test_context_socket_close (pub);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_tcp);
    RUN_TEST (test_conflate_preserves_topic_and_payload);
    return UNITY_END ();
}
