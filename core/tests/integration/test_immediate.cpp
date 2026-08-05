/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

SETUP_TEARDOWN_TESTCONTEXT

void test_immediate_3 ()
{
    // This time we want to validate that the same blocking behaviour
    // occurs with an existing connection that is broken. We will send
    // messages to a connected pipe, disconnect and verify the messages
    // block. Then we reconnect and verify messages flow again.
    void *backend = test_context_socket (ZLINK_SOCKET_DEALER);
    void *frontend = test_context_socket (ZLINK_SOCKET_DEALER);

    int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (backend, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (frontend, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    //  Frontend connects to backend using IMMEDIATE
    int on = 1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (frontend, ZLINK_OPT_IMMEDIATE, &on, sizeof (on)));

    size_t len = MAX_SOCKET_STRING;
    char my_endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (backend, my_endpoint, len);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (frontend, my_endpoint));

    //  Ping backend to frontend so we know when the connection is up
    send_string_expect_success (backend, "Hello", 0);
    recv_string_expect_success (frontend, "Hello", 0);

    // Send message from frontend to backend
    send_string_expect_success (frontend, "Hello", ZLINK_DONTWAIT);

    test_context_socket_close (backend);

    //  Give time to process disconnect
    msleep (SETTLE_TIME * 10);

    // Send a message, should fail
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_send (frontend, "Hello", 5, ZLINK_DONTWAIT));

    //  Recreate backend socket
    backend = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (backend, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (backend, my_endpoint));

    //  Ping backend to frontend so we know when the connection is up
    send_string_expect_success (backend, "Hello", 0);
    recv_string_expect_success (frontend, "Hello", 0);

    // After the reconnect, should succeed
    send_string_expect_success (frontend, "Hello", ZLINK_DONTWAIT);

    test_context_socket_close (backend);
    test_context_socket_close (frontend);
}

int main (void)
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_immediate_3);
    return UNITY_END ();
}
