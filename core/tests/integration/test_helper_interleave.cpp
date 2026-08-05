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

void ignore_reply (zlink_request_result_t, zlink_msg_t *, size_t, void *)
{
}
}

void test_send_part_more_rejects_different_send_helper_on_same_handle ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://helper-interleave-dealer"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, "inproc://helper-interleave-dealer"));
    msleep (SETTLE_TIME);

    zlink_msg_t first;
    init_part (&first, "hello");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &first, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE));

    zlink_msg_t wrong_family;
    init_part (&wrong_family, "request");
    const zlink_submit_result_t wrong_rc =
      zlink_dealer_request_part (dealer, &wrong_family, static_cast<zlink_send_flags_t> (0),
                                 ZLINK_PART_FINAL, 1000, &ignore_reply, NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT, wrong_rc);
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());

    zlink_msg_t last;
    init_part (&last, "world");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &last, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL));

    recv_string_expect_success (router, "D1", 0);
    recv_string_expect_success (router, "hello", 1);
    recv_string_expect_success (router, "world", 0);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_send_part_rid_more_rejects_interleaved_target_change ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer1 = test_context_socket (ZLINK_SOCKET_DEALER);
    void *dealer2 = test_context_socket (ZLINK_SOCKET_DEALER);

    const int mandatory = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof (mandatory)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer1, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer2, "D2", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://helper-interleave-rid"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer1, "inproc://helper-interleave-rid"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer2, "inproc://helper-interleave-rid"));
    msleep (SETTLE_TIME);

    send_string_expect_success (dealer1, "prime-1", 0);
    recv_string_expect_success (router, "D1", 0);
    recv_string_expect_success (router, "prime-1", 0);
    send_string_expect_success (dealer2, "prime-2", 0);
    recv_string_expect_success (router, "D2", 0);
    recv_string_expect_success (router, "prime-2", 0);

    zlink_routing_id_t rid1;
    zlink_routing_id_t rid2;
    memset (&rid1, 0, sizeof (rid1));
    memset (&rid2, 0, sizeof (rid2));
    memcpy (rid1.data, "D1", 2);
    memcpy (rid2.data, "D2", 2);
    rid1.size = 2;
    rid2.size = 2;

    zlink_msg_t first;
    init_part (&first, "frame-a");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, zlink_send_part_rid (router, &rid1, &first,
                                            static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE));

    zlink_msg_t wrong_target;
    init_part (&wrong_target, "frame-b");
    const zlink_submit_result_t wrong_rc = zlink_send_part_rid (
      router, &rid2, &wrong_target, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_INVALID_ARGUMENT, wrong_rc);
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());

    zlink_msg_t last;
    init_part (&last, "frame-c");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, zlink_send_part_rid (router, &rid1, &last,
                                            static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL));

    recv_string_expect_success (dealer1, "frame-a", 1);
    recv_string_expect_success (dealer1, "frame-c", 0);

    test_context_socket_close_zero_linger (dealer2);
    test_context_socket_close_zero_linger (dealer1);
    test_context_socket_close_zero_linger (router);
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_send_part_more_rejects_different_send_helper_on_same_handle);
    RUN_TEST (test_send_part_rid_more_rejects_interleaved_target_change);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
