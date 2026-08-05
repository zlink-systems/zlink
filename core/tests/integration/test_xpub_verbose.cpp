/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

SETUP_TEARDOWN_TESTCONTEXT

const uint8_t unsubscribe_a_msg[] = {0, 'A'};
const uint8_t subscribe_a_msg[] = {1, 'A'};
const uint8_t subscribe_b_msg[] = {1, 'B'};

const char test_endpoint[] = "inproc://soname";
const char topic_a[] = "A";
const char topic_b[] = "B";

void test_xpub_verbose_one_sub ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, test_endpoint));

    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, test_endpoint));

    //  Subscribe for A
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, topic_a));

    // Receive subscriptions from subscriber
    recv_array_expect_success (pub, subscribe_a_msg, 0);

    // Subscribe socket for B instead
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, topic_b));

    // Receive subscriptions from subscriber
    recv_array_expect_success (pub, subscribe_b_msg, 0);

    //  Subscribe again for A again
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, topic_a));

    //  This time it is duplicated, so it will be filtered out
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (pub, NULL, 0, ZLINK_DONTWAIT));

    int verbose = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_pub_option (pub, ZLINK_PUB_OPT_VERBOSE, &verbose, sizeof (int)));

    // Subscribe socket for A again
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, topic_a));

    // This time with VERBOSE the duplicated sub will be received
    recv_array_expect_success (pub, subscribe_a_msg, 0);

    // Sending A message and B Message
    send_string_expect_success (pub, topic_a, 0);
    send_string_expect_success (pub, topic_b, 0);

    recv_string_expect_success (sub, topic_a, 0);
    recv_string_expect_success (sub, topic_b, 0);

    //  Clean up.
    test_context_socket_close (pub);
    test_context_socket_close (sub);
}

void create_xpub_with_2_subs (void **pub_, void **sub0_, void **sub1_)
{
    *pub_ = test_context_socket (ZLINK_SOCKET_XPUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (*pub_, test_endpoint));

    *sub0_ = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (*sub0_, test_endpoint));

    *sub1_ = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (*sub1_, test_endpoint));
}

void create_duplicate_subscription (void *pub_, void *sub0_, void *sub1_)
{
    //  Subscribe for A
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub0_, topic_a));

    // Receive subscriptions from subscriber
    recv_array_expect_success (pub_, subscribe_a_msg, 0);

    //  Subscribe again for A on the other socket
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub1_, topic_a));

    //  This time it is duplicated, so it will be filtered out by XPUB
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (pub_, NULL, 0, ZLINK_DONTWAIT));
}

void test_xpub_verbose_two_subs ()
{
    void *pub, *sub0, *sub1;
    create_xpub_with_2_subs (&pub, &sub0, &sub1);
    create_duplicate_subscription (pub, sub0, sub1);

    // Subscribe socket for B instead
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub0, topic_b));

    // Receive subscriptions from subscriber
    recv_array_expect_success (pub, subscribe_b_msg, 0);

    int verbose = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_pub_option (pub, ZLINK_PUB_OPT_VERBOSE, &verbose, sizeof (int)));

    // Subscribe socket for A again
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub1, topic_a));

    // This time with VERBOSE the duplicated sub will be received
    recv_array_expect_success (pub, subscribe_a_msg, 0);

    // Sending A message and B Message
    send_string_expect_success (pub, topic_a, 0);

    send_string_expect_success (pub, topic_b, 0);

    recv_string_expect_success (sub0, topic_a, 0);
    recv_string_expect_success (sub1, topic_a, 0);
    recv_string_expect_success (sub0, topic_b, 0);

    //  Clean up.
    test_context_socket_close (pub);
    test_context_socket_close (sub0);
    test_context_socket_close (sub1);
}

void test_xpub_verboser_one_sub ()
{
    //  Create a publisher
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, test_endpoint));

    //  Create a subscriber
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, test_endpoint));

    //  Unsubscribe for A, does not exist yet
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (sub, topic_a));

    //  Does not exist, so it will be filtered out by XSUB
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (pub, NULL, 0, ZLINK_DONTWAIT));

    //  Subscribe for A
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, topic_a));

    // Receive subscriptions from subscriber
    recv_array_expect_success (pub, subscribe_a_msg, 0);

    //  Subscribe again for A again, XSUB will increase refcount
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, topic_a));

    //  This time it is duplicated, so it will be filtered out by XPUB
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (pub, NULL, 0, ZLINK_DONTWAIT));

    //  Unsubscribe for A, this time it exists in XPUB
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (sub, topic_a));

    //  XSUB refcounts and will not actually send unsub to PUB until the number
    //  of unsubs match the earlier subs
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (sub, topic_a));

    // Receive unsubscriptions from subscriber
    recv_array_expect_success (pub, unsubscribe_a_msg, 0);

    //  XSUB only sends the last and final unsub, so XPUB will only receive 1
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (pub, NULL, 0, ZLINK_DONTWAIT));

    //  Unsubscribe for A, does not exist anymore
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (sub, topic_a));

    //  Does not exist, so it will be filtered out by XSUB
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (pub, NULL, 0, ZLINK_DONTWAIT));

    int verbose = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_pub_option (pub, ZLINK_PUB_OPT_VERBOSER, &verbose, sizeof (int)));

    // Subscribe socket for A again
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, topic_a));

    // Receive subscriptions from subscriber, did not exist anymore
    recv_array_expect_success (pub, subscribe_a_msg, 0);

    // Sending A message to make sure everything still works
    send_string_expect_success (pub, topic_a, 0);

    recv_string_expect_success (sub, topic_a, 0);

    //  Unsubscribe for A, this time it exists
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (sub, topic_a));

    // Receive unsubscriptions from subscriber
    recv_array_expect_success (pub, unsubscribe_a_msg, 0);

    //  Unsubscribe for A again, it does not exist anymore so XSUB will filter
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (sub, topic_a));

    //  XSUB only sends unsub if it matched it in its trie, IOW: it will only
    //  send it if it existed in the first place even with XPUB_VERBBOSER
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (pub, NULL, 0, ZLINK_DONTWAIT));

    //  Clean up.
    test_context_socket_close (pub);
    test_context_socket_close (sub);
}

void test_xpub_verboser_two_subs ()
{
    void *pub, *sub0, *sub1;
    create_xpub_with_2_subs (&pub, &sub0, &sub1);
    create_duplicate_subscription (pub, sub0, sub1);

    //  Unsubscribe for A, this time it exists in XPUB
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (sub0, topic_a));

    //  sub1 is still subscribed, so no notification
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (pub, NULL, 0, ZLINK_DONTWAIT));

    //  Unsubscribe the second socket to trigger the notification
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (sub1, topic_a));

    // Receive unsubscriptions since all sockets are gone
    recv_array_expect_success (pub, unsubscribe_a_msg, 0);

    //  Make really sure there is only one notification
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (pub, NULL, 0, ZLINK_DONTWAIT));

    int verbose = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_pub_option (pub, ZLINK_PUB_OPT_VERBOSER, &verbose, sizeof (int)));

    // Subscribe socket for A again
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub0, topic_a));

    // Subscribe socket for A again
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub1, topic_a));

    // Receive subscriptions from subscriber, did not exist anymore
    recv_array_expect_success (pub, subscribe_a_msg, 0);

    //  VERBOSER is set, so subs from both sockets are received
    recv_array_expect_success (pub, subscribe_a_msg, 0);

    // Sending A message to make sure everything still works
    send_string_expect_success (pub, topic_a, 0);

    recv_string_expect_success (sub0, topic_a, 0);
    recv_string_expect_success (sub1, topic_a, 0);

    //  Unsubscribe for A
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (sub1, topic_a));

    // Receive unsubscriptions from first subscriber due to VERBOSER
    recv_array_expect_success (pub, unsubscribe_a_msg, 0);

    //  Unsubscribe for A again from the other socket
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (sub0, topic_a));

    // Receive unsubscriptions from first subscriber due to VERBOSER
    recv_array_expect_success (pub, unsubscribe_a_msg, 0);

    //  Unsubscribe again to make sure it gets filtered now
    TEST_ASSERT_SUCCESS_ERRNO (zlink_unset_subscription (sub1, topic_a));

    //  Unmatched, so XSUB filters even with VERBOSER
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (pub, NULL, 0, ZLINK_DONTWAIT));

    //  Clean up.
    test_context_socket_close (pub);
    test_context_socket_close (sub0);
    test_context_socket_close (sub1);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_xpub_verbose_one_sub);
    RUN_TEST (test_xpub_verbose_two_subs);
    RUN_TEST (test_xpub_verboser_one_sub);
    RUN_TEST (test_xpub_verboser_two_subs);

    return UNITY_END ();
}
