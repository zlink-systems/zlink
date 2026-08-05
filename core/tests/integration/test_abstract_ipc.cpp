/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

static const char test_endpoint[] = "ipc://@tmp-tester";
static const char test_endpoint_empty[] = "ipc://@";

void test_roundtrip ()
{
    void *sb = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (sb, test_endpoint));

    char endpoint[MAX_SOCKET_STRING];
    size_t size = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (sb, ZLINK_OPT_LAST_ENDPOINT, endpoint, &size));
    TEST_ASSERT_EQUAL_INT (0, strncmp (endpoint, test_endpoint, size));

    void *sc = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sc, test_endpoint));

    bounce (sb, sc);

    test_context_socket_close (sc);
    test_context_socket_close (sb);
}

void test_empty_abstract_name ()
{
    void *sb = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_FAILURE_ERRNO (EINVAL, zlink_bind (sb, test_endpoint_empty));

    test_context_socket_close (sb);
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_roundtrip);
    RUN_TEST (test_empty_abstract_name);
    return UNITY_END ();
}
