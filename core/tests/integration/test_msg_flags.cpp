/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

SETUP_TEARDOWN_TESTCONTEXT

void test_more ()
{
    //  Create the infrastructure
    void *sb = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (sb, "inproc://a"));

    void *sc = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sc, "inproc://a"));

    //  Send 2-part message.
    send_string_expect_success (sc, "A", ZLINK_SNDMORE);
    send_string_expect_success (sc, "B", 0);

    //  Routing id comes first.
    zlink_msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
    TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&msg, sb, 0));
    TEST_ASSERT_TRUE (test_msg_has_more (&msg));

    //  Then the first part of the message body.
    TEST_ASSERT_EQUAL_INT (1, TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&msg, sb, 0)));
    TEST_ASSERT_TRUE (test_msg_has_more (&msg));

    //  And finally, the second part of the message body.
    TEST_ASSERT_EQUAL_INT (1, TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&msg, sb, 0)));
    TEST_ASSERT_FALSE (test_msg_has_more (&msg));

    //  Deallocate the infrastructure.
    test_context_socket_close (sc);
    test_context_socket_close (sb);
}

void test_pair_socket_preserves_multipart_more_flag ()
{
    void *sb = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (sb, "inproc://msg-flags-pair"));

    void *sc = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sc, "inproc://msg-flags-pair"));

    TEST_ASSERT_EQUAL_INT (3, TEST_ASSERT_SUCCESS_ERRNO (zlink_send (sb, "foo", 3, ZLINK_SNDMORE)));
    TEST_ASSERT_EQUAL_INT (6, TEST_ASSERT_SUCCESS_ERRNO (zlink_send (sb, "foobar", 6, 0)));

    recv_string_expect_success (sc, "foo", 0);
    recv_string_expect_success (sc, "foobar", 0);

    test_context_socket_close (sc);
    test_context_socket_close (sb);
}

void test_shared_refcounted ()
{
    // Test shared storage query (case 1, refcounted messages)
    zlink_msg_t msg_a;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&msg_a, 1024)); // large enough to be a type_lmsg

    // Single-owner reference-counted storage reports refcount 1.
    TEST_ASSERT_EQUAL_INT (1, zlink_msg_refcnt (&msg_a, NULL));

    zlink_msg_t msg_b;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg_b));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_copy (&msg_b, &msg_a));

    // Both handles now share the same storage.
    TEST_ASSERT_EQUAL_INT (2, zlink_msg_refcnt (&msg_a, NULL));
    TEST_ASSERT_EQUAL_INT (2, zlink_msg_refcnt (&msg_b, NULL));

    // cleanup
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg_a));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg_b));
}

void test_shared_const ()
{
    zlink_msg_t msg_a;
    // Test shared storage query (case 2, constant data messages)
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_data (&msg_a, (void *) "TEST", 5, 0, 0));

    // Constant messages are not internally refcounted; they report 1.
    TEST_ASSERT_EQUAL_INT (1, zlink_msg_refcnt (&msg_a, NULL));

    // cleanup
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg_a));
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_more);
    RUN_TEST (test_pair_socket_preserves_multipart_more_flag);
    RUN_TEST (test_shared_refcounted);
    RUN_TEST (test_shared_const);
    return UNITY_END ();
}
