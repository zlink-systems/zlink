/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cstdlib>
#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
void init_part (zlink_msg_t *part_, const char *text_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, strlen (text_)));
    memcpy (zlink_msg_data (part_), text_, strlen (text_));
}

}

void test_wrong_send_helper_aborts_open_sequence_after_bad_recv_attempt ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://helper-more-bad-send"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, "inproc://helper-more-bad-send"));
    msleep (SETTLE_TIME);

    zlink_msg_t first;
    init_part (&first, "part-1");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &first, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE, NULL, NULL));

    zlink_msg_t *recv_parts = NULL;
    size_t recv_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA,
                           zlink_recv (dealer, NULL, &recv_parts, &recv_count, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    zlink_msg_t wrong_family;
    init_part (&wrong_family, "wrong");
    zlink_completion_id_t completion_id = UINT64_MAX;
    const zlink_submit_result_t wrong_rc =
      zlink_request_part (dealer, NULL, &wrong_family,
                          static_cast<zlink_send_flags_t> (0),
                          ZLINK_PART_FINAL, 1000, NULL, &completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT, wrong_rc);
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&wrong_family));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&wrong_family));

    zlink_msg_t final_part;
    init_part (&final_part, "part-2");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &final_part, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL, NULL, NULL));

    recv_routed_string_expect_success (router, "part-2", "D1");

    zlink_msg_t next_msg;
    init_part (&next_msg, "after-reset");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &next_msg, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL, NULL, NULL));
    recv_routed_string_expect_success (router, "after-reset", "D1");

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_wrong_send_helper_aborts_open_sequence_after_bad_recv_attempt);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
