/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

SETUP_TEARDOWN_TESTCONTEXT

void test ()
{
    //  Create a publisher
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, "inproc://soname"));

    //  set pub socket options
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_pub_option (pub, ZLINK_PUB_OPT_WELCOME_MSG, "W", 1));
    int enabled = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_pub_option (pub, ZLINK_PUB_OPT_VERBOSE, &enabled, sizeof (enabled)));

    char welcome[2] = {};
    size_t welcome_size = sizeof (welcome);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_pub_option (pub, ZLINK_PUB_OPT_WELCOME_MSG, welcome, &welcome_size));
    TEST_ASSERT_EQUAL_UINT (1, welcome_size);
    TEST_ASSERT_EQUAL_CHAR ('W', welcome[0]);

    int verbose = 0;
    size_t verbose_size = sizeof (verbose);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_pub_option (pub, ZLINK_PUB_OPT_VERBOSE, &verbose, &verbose_size));
    TEST_ASSERT_EQUAL_INT (1, verbose);

    //  Create a subscriber
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);

    // Subscribe to the welcome message
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "W"));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, "inproc://soname"));

    const uint8_t buffer[2] = {1, 'W'};

    // Receive the welcome subscription
    recv_array_expect_success (pub, buffer, 0);

    // Receive the welcome message
    recv_string_expect_success (sub, "W", 0);

    //  Clean up.
    test_context_socket_close (pub);
    test_context_socket_close (sub);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test);
    return UNITY_END ();
}
