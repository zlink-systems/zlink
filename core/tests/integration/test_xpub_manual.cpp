/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
void expect_subscription (void *socket_, const char *topic_, int subscribed_,
                           int flags_ = 0)
{
    char topic[32];
    size_t length = sizeof (topic);
    int subscribed = -1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_subscription_event (
      socket_, NULL, &subscribed, topic, &length, flags_));
    TEST_ASSERT_EQUAL_INT (subscribed_, subscribed);
    TEST_ASSERT_EQUAL_UINT64 (strlen (topic_), length);
    TEST_ASSERT_EQUAL_MEMORY (topic_, topic, length);
}

void publish_bytes (void *socket_, const char *topic_, const void *payload_, size_t size_)
{
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, size_));
    memcpy (zlink_msg_data (&part), payload_, size_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_publish (
      socket_, topic_, &part, 1, ZLINK_SEND_FLAGS_NONE));
}

void expect_publication (void *socket_, const char *topic_, const void *payload_,
                         size_t size_, int flags_ = 0)
{
    char topic[32];
    size_t topic_length = 0;
    zlink_msg_t part;
    size_t count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_subscribe (
      socket_, NULL, topic, sizeof (topic), &topic_length, &part, 1, &count,
      static_cast<zlink_recv_flags_t> (flags_)));
    TEST_ASSERT_EQUAL_UINT64 (1, count);
    TEST_ASSERT_EQUAL_UINT64 (strlen (topic_), topic_length);
    TEST_ASSERT_EQUAL_MEMORY (topic_, topic, topic_length);
    TEST_ASSERT_EQUAL_UINT64 (size_, zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_MEMORY (payload_, zlink_msg_data (&part), size_);
    zlink_multipart_close (&part, count);
}

void expect_no_publication (void *socket_)
{
    char topic[32];
    size_t topic_length = 0;
    zlink_msg_t part;
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, zlink_subscribe (
      socket_, NULL, topic, sizeof (topic), &topic_length, &part, 1, &count,
      ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
}
}

void test_basic ()
{
    //  Create a publisher
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);
    int manual = 1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_pub_option (pub, ZLINK_PUB_OPT_MANUAL, &manual, 4));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, "inproc://soname"));

    //  Create a subscriber
    void *sub = test_context_socket (ZLINK_SOCKET_XSUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, "inproc://soname"));

    //  Subscribe for A
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "A"));

    // Receive subscriptions from subscriber
    expect_subscription (pub, "A", 1);

    // Subscribe socket for B instead
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (pub, "B"));

    // Sending A message and B Message
    send_published_string_expect_success (pub, "A", "A");
    send_published_string_expect_success (pub, "B", "B");

    expect_publication (sub, "B", "B", 1, ZLINK_DONTWAIT);

    //  Clean up.
    test_context_socket_close (pub);
    test_context_socket_close (sub);
}

void test_unsubscribe_manual ()
{
    //  Create a publisher
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, "inproc://soname"));

    //  set pub socket options
    int manual = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_pub_option (pub, ZLINK_PUB_OPT_MANUAL, &manual, sizeof (manual)));

    //  Create a subscriber
    void *sub = test_context_socket (ZLINK_SOCKET_XSUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, "inproc://soname"));

    //  Subscribe for A
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "A"));

    //  Subscribe for B
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "B"));


    // Receive subscription "A" from subscriber
    expect_subscription (pub, "A", 1);

    // Subscribe socket for XA instead
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (pub, "XA"));

    // Receive subscription "B" from subscriber
    expect_subscription (pub, "B", 1);

    // Subscribe socket for XB instead
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (pub, "XB"));

    //  Unsubscribe from A
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (sub, "A"));

    // Receive unsubscription "A" from subscriber
    expect_subscription (pub, "A", 0);

    // Unsubscribe socket from XA instead
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (pub, "XA"));

    // Sending messages XA, XB
    send_published_string_expect_success (pub, "XA", "XA");
    send_published_string_expect_success (pub, "XB", "XB");

    // Subscriber should receive XB only
    expect_publication (sub, "XB", "XB", 2, ZLINK_DONTWAIT);

    // Close subscriber
    test_context_socket_close (sub);

    // Receive unsubscription "B"
    expect_subscription (pub, "B", 0);

    // Unsubscribe socket from XB instead
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (pub, "XB"));

    //  Clean up.
    test_context_socket_close (pub);
}

void test_xpub_proxy_unsubscribe_on_disconnect ()
{
    const char topic_buff[] = {"1"};
    const uint8_t payload_buff[] = {"X"};

    char my_endpoint_backend[MAX_SOCKET_STRING];
    char my_endpoint_frontend[MAX_SOCKET_STRING];

    int manual = 1;

    // proxy frontend
    void *xsub_proxy = test_context_socket (ZLINK_SOCKET_XSUB);
    bind_loopback_ipv4 (xsub_proxy, my_endpoint_frontend, sizeof my_endpoint_frontend);

    // proxy backend
    void *xpub_proxy = test_context_socket (ZLINK_SOCKET_XPUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_pub_option (xpub_proxy, ZLINK_PUB_OPT_MANUAL, &manual, 4));
    bind_loopback_ipv4 (xpub_proxy, my_endpoint_backend, sizeof my_endpoint_backend);

    // publisher
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (pub, my_endpoint_frontend));

    // first subscriber subscribes
    void *sub1 = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub1, my_endpoint_backend));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub1, topic_buff));

    // wait
    msleep (SETTLE_TIME);

    // proxy reroutes and confirms subscriptions
    expect_subscription (xpub_proxy, reinterpret_cast<const char *> (topic_buff), 1, ZLINK_DONTWAIT);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (xpub_proxy, topic_buff));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (xsub_proxy, reinterpret_cast<const char *> (topic_buff)));

    // second subscriber subscribes
    void *sub2 = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub2, my_endpoint_backend));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub2, topic_buff));

    // wait
    msleep (SETTLE_TIME);

    // proxy reroutes
    expect_subscription (xpub_proxy, reinterpret_cast<const char *> (topic_buff), 1, ZLINK_DONTWAIT);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (xpub_proxy, topic_buff));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (xsub_proxy, reinterpret_cast<const char *> (topic_buff)));

    // wait
    msleep (SETTLE_TIME);

    // let publisher send a msg
    publish_bytes (pub, reinterpret_cast<const char *> (topic_buff), payload_buff,
                   sizeof (payload_buff));

    // wait
    msleep (SETTLE_TIME);

    // proxy reroutes data messages to subscribers
    expect_publication (xsub_proxy, reinterpret_cast<const char *> (topic_buff), payload_buff,
                        sizeof (payload_buff), ZLINK_DONTWAIT);
    publish_bytes (xpub_proxy, reinterpret_cast<const char *> (topic_buff), payload_buff,
                   sizeof (payload_buff));

    // wait
    msleep (SETTLE_TIME);

    // each subscriber should now get a message
    expect_publication (sub2, reinterpret_cast<const char *> (topic_buff), payload_buff,
                        sizeof (payload_buff), ZLINK_DONTWAIT);

    expect_publication (sub1, reinterpret_cast<const char *> (topic_buff), payload_buff,
                        sizeof (payload_buff), ZLINK_DONTWAIT);

    //  Disconnect both subscribers
    test_context_socket_close (sub1);
    test_context_socket_close (sub2);

    // wait
    msleep (SETTLE_TIME);

    // unsubscribe messages are passed from proxy to publisher
    expect_subscription (xpub_proxy, reinterpret_cast<const char *> (topic_buff), 0);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (xpub_proxy, topic_buff));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (xsub_proxy, reinterpret_cast<const char *> (topic_buff)));

    // should receive another unsubscribe msg
    expect_subscription (xpub_proxy, reinterpret_cast<const char *> (topic_buff), 0);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (xpub_proxy, topic_buff));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (xsub_proxy, reinterpret_cast<const char *> (topic_buff)));

    // wait
    msleep (SETTLE_TIME);

    // let publisher send a msg
    publish_bytes (pub, reinterpret_cast<const char *> (topic_buff), payload_buff,
                   sizeof (payload_buff));

    // wait
    msleep (SETTLE_TIME);

    // nothing should come to the proxy
    expect_no_publication (xsub_proxy);

    test_context_socket_close (pub);
    test_context_socket_close (xpub_proxy);
    test_context_socket_close (xsub_proxy);
}

void test_missing_subscriptions ()
{
    const char *topic1 = "1";
    const char *topic2 = "2";
    const char *payload = "X";

    char my_endpoint_backend[MAX_SOCKET_STRING];
    char my_endpoint_frontend[MAX_SOCKET_STRING];

    int manual = 1;

    // proxy frontend
    void *xsub_proxy = test_context_socket (ZLINK_SOCKET_XSUB);
    bind_loopback_ipv4 (xsub_proxy, my_endpoint_frontend, sizeof my_endpoint_frontend);

    // proxy backend
    void *xpub_proxy = test_context_socket (ZLINK_SOCKET_XPUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_pub_option (xpub_proxy, ZLINK_PUB_OPT_MANUAL, &manual, 4));
    bind_loopback_ipv4 (xpub_proxy, my_endpoint_backend, sizeof my_endpoint_backend);

    // publisher
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (pub, my_endpoint_frontend));

    // Here's the problem: because subscribers subscribe in quick succession,
    // the proxy is unable to confirm the first subscription before receiving
    // the second. This causes the first subscription to get lost.

    // first subscriber
    void *sub1 = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub1, my_endpoint_backend));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub1, topic1));

    // wait
    msleep (SETTLE_TIME);

    // proxy now reroutes and confirms subscriptions
    expect_subscription (xpub_proxy, topic1, 1, ZLINK_DONTWAIT);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (xpub_proxy, topic1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (xsub_proxy, topic1));

    // second subscriber
    void *sub2 = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub2, my_endpoint_backend));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub2, topic2));

    // wait
    msleep (SETTLE_TIME);

    expect_subscription (xpub_proxy, topic2, 1, ZLINK_DONTWAIT);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (xpub_proxy, topic2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (xsub_proxy, topic2));

    // wait
    msleep (SETTLE_TIME);

    // let publisher send 2 msgs, each with its own topic_buff
    send_published_string_expect_success (pub, topic1, payload);
    send_published_string_expect_success (pub, topic2, payload);

    // wait
    msleep (SETTLE_TIME);

    // proxy reroutes data messages to subscribers
    expect_publication (xsub_proxy, topic1, payload, strlen (payload), ZLINK_DONTWAIT);
    send_published_string_expect_success (xpub_proxy, topic1, payload);

    expect_publication (xsub_proxy, topic2, payload, strlen (payload), ZLINK_DONTWAIT);
    send_published_string_expect_success (xpub_proxy, topic2, payload);

    // wait
    msleep (SETTLE_TIME);

    // each subscriber should now get a message
    expect_publication (sub2, topic2, payload, strlen (payload), ZLINK_DONTWAIT);

    expect_publication (sub1, topic1, payload, strlen (payload), ZLINK_DONTWAIT);

    //  Clean up
    test_context_socket_close (sub1);
    test_context_socket_close (sub2);
    test_context_socket_close (pub);
    test_context_socket_close (xpub_proxy);
    test_context_socket_close (xsub_proxy);
}

void test_unsubscribe_cleanup ()
{
    char my_endpoint[MAX_SOCKET_STRING];

    //  Create a publisher
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);
    int manual = 1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_pub_option (pub, ZLINK_PUB_OPT_MANUAL, &manual, 4));
    bind_loopback_ipv4 (pub, my_endpoint, sizeof my_endpoint);

    //  Create a subscriber
    void *sub = test_context_socket (ZLINK_SOCKET_XSUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, my_endpoint));

    //  Subscribe for A
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "A"));


    // Receive subscriptions from subscriber
    expect_subscription (pub, "A", 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (pub, "XA"));

    // send 2 messages
    send_published_string_expect_success (pub, "XA", "XA");
    send_published_string_expect_success (pub, "XB", "XB");

    // receive the single message
    expect_publication (sub, "XA", "XA", 2, 0);

    // should be nothing left in the queue
    expect_no_publication (sub);

    // close the socket
    test_context_socket_close (sub);

    // closing the socket will result in an unsubscribe event
    expect_subscription (pub, "A", 0);

    // this doesn't really do anything
    // there is no last_pipe set it will just fail silently
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (pub, "XA"));

    // reconnect
    sub = test_context_socket (ZLINK_SOCKET_XSUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, my_endpoint));

    // send a subscription for B
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "B"));

    // receive the subscription, overwrite it to XB
    expect_subscription (pub, "B", 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (pub, "XB"));

    // send 2 messages
    send_published_string_expect_success (pub, "XA", "XA");
    send_published_string_expect_success (pub, "XB", "XB");

    // receive the single message
    expect_publication (sub, "XB", "XB", 2, 0);

    // should be nothing left in the queue
    expect_no_publication (sub);

    //  Clean up.
    test_context_socket_close (pub);
    test_context_socket_close (sub);
}



int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_basic);
    RUN_TEST (test_unsubscribe_manual);
    RUN_TEST (test_xpub_proxy_unsubscribe_on_disconnect);
    RUN_TEST (test_missing_subscriptions);
    RUN_TEST (test_unsubscribe_cleanup);

    return UNITY_END ();
}
