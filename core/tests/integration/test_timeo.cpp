/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

SETUP_TEARDOWN_TESTCONTEXT

void test_timeo ()
{
    void *frontend = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (frontend, "inproc://timeout_test"));

    //  Receive on disconnected socket returns immediately
    char buffer[32];
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (frontend, buffer, 32, ZLINK_DONTWAIT));


    //  Check whether receive timeout is honored
    const int timeout = 250;
    const int jitter = 50;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (frontend, ZLINK_OPT_RCVTIMEO, &timeout, sizeof (int)));

    void *stopwatch = zlink_stopwatch_start ();
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (frontend, buffer, 32, 0));
    unsigned int elapsed = zlink_stopwatch_stop (stopwatch) / 1000;
    TEST_ASSERT_GREATER_THAN_INT (timeout - jitter, elapsed);
    if (elapsed >= timeout + jitter) {
        // we cannot assert this on a non-RT system
        fprintf (stderr,
                 "zlink_recv took quite long, with a timeout of %i ms, it took "
                 "actually %i ms\n",
                 timeout, elapsed);
    }

    //  Check that normal message flow works as expected
    void *backend = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (backend, "inproc://timeout_test"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (backend, ZLINK_OPT_SNDTIMEO, &timeout, sizeof (int)));

    send_string_expect_success (backend, "Hello", 0);
    recv_string_expect_success (frontend, "Hello", 0);

    //  Clean-up
    test_context_socket_close (backend);
    test_context_socket_close (frontend);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_timeo);
    return UNITY_END ();
}
