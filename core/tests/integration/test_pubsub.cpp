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

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_tcp);
    return UNITY_END ();
}
