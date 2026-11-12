/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

SETUP_TEARDOWN_TESTCONTEXT

static void do_bind_and_verify (void *s_, char *endpoint_, size_t endpoint_size_)
{
    test_bind (s_, "tcp://127.0.0.1:*", endpoint_, endpoint_size_);
    TEST_ASSERT_TRUE (strncmp (endpoint_, "tcp://127.0.0.1:", 16) == 0);
}

void test_last_endpoint ()
{
    void *sb = test_context_socket (ZLINK_SOCKET_ROUTER);
    int val = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (sb, ZLINK_OPT_LINGER, &val, sizeof (val)));

    char endpoint_1[MAX_SOCKET_STRING];
    char endpoint_2[MAX_SOCKET_STRING];
    do_bind_and_verify (sb, endpoint_1, sizeof (endpoint_1));
    do_bind_and_verify (sb, endpoint_2, sizeof (endpoint_2));
    TEST_ASSERT_TRUE (strcmp (endpoint_1, endpoint_2) != 0);

    test_context_socket_close (sb);
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_last_endpoint);
    return UNITY_END ();
}
