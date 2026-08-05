/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

void test_stream_connect_routing_id_rejected ()
{
    void *socket = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (socket);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    const char *alias = "stream-alias";
    TEST_ASSERT_FAILURE_ERRNO (
      EOPNOTSUPP,
      zlink_set_router_option (socket, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, alias, strlen (alias)));

    const unsigned char fixed_id[4] = {'R', 'I', 'D', '1'};
    TEST_ASSERT_FAILURE_ERRNO (EOPNOTSUPP,
                               zlink_set_router_option (socket, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                                        fixed_id, sizeof (fixed_id)));

    test_context_socket_close_zero_linger (socket);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_stream_connect_routing_id_rejected);
    return UNITY_END ();
}
