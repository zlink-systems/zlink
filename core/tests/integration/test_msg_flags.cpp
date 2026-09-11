/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cstring>

SETUP_TEARDOWN_TESTCONTEXT

void test_more ()
{
    //  Create the infrastructure
    void *sb = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (sb, "inproc://a"));

    void *sc = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sc, "inproc://a"));

    zlink_msg_t outgoing[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&outgoing[0], 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&outgoing[1], 1));
    memcpy (zlink_msg_data (&outgoing[0]), "A", 1);
    memcpy (zlink_msg_data (&outgoing[1]), "B", 1);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send (sc, outgoing, 2, ZLINK_SEND_FLAGS_NONE, NULL, NULL));

    //  The public receive returns the source id beside the first payload part.
    zlink_msg_t msg[2];
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token;
    size_t more = 1;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
      zlink_router_recv (sb, &source, &token, msg, 2, &more,
                         ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_GREATER_THAN_UINT (0, source->size);
    TEST_ASSERT_EQUAL_UINT (2, more);
    TEST_ASSERT_EQUAL_UINT (1, zlink_msg_size (&msg[0]));
    TEST_ASSERT_EQUAL_MEMORY ("A", zlink_msg_data (&msg[0]), 1);
    TEST_ASSERT_EQUAL_UINT (1, zlink_msg_size (&msg[1]));
    TEST_ASSERT_EQUAL_MEMORY ("B", zlink_msg_data (&msg[1]), 1);
    zlink_multipart_close (msg, more);

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

    zlink_msg_t outgoing[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&outgoing[0], 3));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&outgoing[1], 6));
    memcpy (zlink_msg_data (&outgoing[0]), "foo", 3);
    memcpy (zlink_msg_data (&outgoing[1]), "foobar", 6);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send (sb, outgoing, 2, ZLINK_SEND_FLAGS_NONE, NULL, NULL));
    zlink_msg_t incoming[2];
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv (sc, NULL, incoming, 2, &count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (2, count);
    TEST_ASSERT_EQUAL_MEMORY ("foo", zlink_msg_data (&incoming[0]), 3);
    TEST_ASSERT_EQUAL_MEMORY ("foobar", zlink_msg_data (&incoming[1]), 6);
    zlink_multipart_close (incoming, count);

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
