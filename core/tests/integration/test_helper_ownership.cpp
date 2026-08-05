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

void test_send_part_consumes_input_and_requires_reinit_before_reuse ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, "inproc://helper-ownership-send"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, "inproc://helper-ownership-send"));

    zlink_msg_t part;
    init_part (&part, "first");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &part, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, 6));
    memcpy (zlink_msg_data (&part), "second", 6);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &part, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));

    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv (receiver, NULL, &received, &part_count, static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_MEMORY ("first", zlink_msg_data (&received[0]), 5);
    zlink_multipart_close (received, part_count);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv (receiver, NULL, &received, &part_count, static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_MEMORY ("second", zlink_msg_data (&received[0]), 6);
    zlink_multipart_close (received, part_count);
}

void test_recv_part_returns_caller_owned_message_handle ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, "inproc://helper-ownership-recv"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, "inproc://helper-ownership-recv"));

    zlink_msg_t send_a;
    zlink_msg_t send_b;
    init_part (&send_a, "owned-a");
    init_part (&send_b, "owned-b");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send (sender, &send_a, 1, static_cast<zlink_send_flags_t> (0)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send (sender, &send_b, 1, static_cast<zlink_send_flags_t> (0)));

    zlink_msg_t recv_a;
    zlink_msg_t recv_b;
    zlink_msg_init (&recv_a);
    zlink_msg_init (&recv_b);
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv_part (receiver, NULL, &recv_a, &has_more, static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_INT (0, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("owned-a", zlink_msg_data (&recv_a), 7);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&recv_a));

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv_part (receiver, NULL, &recv_b, &has_more, static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_INT (0, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("owned-b", zlink_msg_data (&recv_b), 7);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&recv_b));
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_send_part_consumes_input_and_requires_reinit_before_reuse);
    RUN_TEST (test_recv_part_returns_caller_owned_message_handle);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
