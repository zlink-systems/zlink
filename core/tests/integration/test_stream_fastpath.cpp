/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <errno.h>

SETUP_TEARDOWN_TESTCONTEXT

void test_stream_fastpath_tcp_basic ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const zlink_stream_recv_mode_t recv_mode = ZLINK_STREAM_RECV_MODE_RAW;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &recv_mode,
                               sizeof (recv_mode)));

    zlink_msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_recv_part (server, NULL, &msg, &has_more, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&msg));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, has_more);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));

    test_context_socket_close_zero_linger (server);
}

int main (void)
{
    UNITY_BEGIN ();

    setup_test_environment ();

    RUN_TEST (test_stream_fastpath_tcp_basic);

    return UNITY_END ();
}
