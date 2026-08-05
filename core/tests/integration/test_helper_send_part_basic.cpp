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

void assert_recv_parts (void *socket_, const char *part0_, const char *part1_, const char *part2_)
{
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv (socket_, NULL, &parts, &part_count, static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_UINT64 (3, part_count);

    TEST_ASSERT_EQUAL_UINT64 (strlen (part0_), zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (part0_, zlink_msg_data (&parts[0]), strlen (part0_));
    TEST_ASSERT_EQUAL_UINT64 (strlen (part1_), zlink_msg_size (&parts[1]));
    TEST_ASSERT_EQUAL_MEMORY (part1_, zlink_msg_data (&parts[1]), strlen (part1_));
    TEST_ASSERT_EQUAL_UINT64 (strlen (part2_), zlink_msg_size (&parts[2]));
    TEST_ASSERT_EQUAL_MEMORY (part2_, zlink_msg_data (&parts[2]), strlen (part2_));

    zlink_multipart_close (parts, part_count);
}

void assert_recv_single (void *socket_, const char *payload_)
{
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_recv (socket_, NULL, &parts, &part_count, static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (strlen (payload_), zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (payload_, zlink_msg_data (&parts[0]), strlen (payload_));
    zlink_multipart_close (parts, part_count);
}
}

void test_send_part_matches_aggregate_wire_shape ()
{
    TEST_ASSERT_TRUE (setenv ("ZLINK_RECV_TLS_PAYLOAD_CAP", "8", 1) == 0);

    void *receiver = test_context_socket (ZLINK_SOCKET_DEALER);
    void *sender = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, "inproc://helper-send-part-basic"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, "inproc://helper-send-part-basic"));
    msleep (SETTLE_TIME);

    zlink_msg_t aggregate[3];
    init_part (&aggregate[0], "alpha");
    init_part (&aggregate[1], "beta");
    init_part (&aggregate[2], "gamma");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send (sender, aggregate, 3, static_cast<zlink_send_flags_t> (0)));
    assert_recv_parts (receiver, "alpha", "beta", "gamma");

    zlink_msg_t helper0;
    zlink_msg_t helper1;
    zlink_msg_t helper2;
    init_part (&helper0, "alpha");
    init_part (&helper1, "beta");
    init_part (&helper2, "gamma");

    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &helper0, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &helper1, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &helper2, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL));

    assert_recv_parts (receiver, "alpha", "beta", "gamma");
}

void test_send_part_single_final_keeps_next_multipart_valid ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_DEALER);
    void *sender = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (receiver, "inproc://helper-send-part-single-final"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sender, "inproc://helper-send-part-single-final"));
    msleep (SETTLE_TIME);

    zlink_msg_t single;
    init_part (&single, "single");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &single, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&single));
    assert_recv_single (receiver, "single");

    zlink_msg_t helper0;
    zlink_msg_t helper1;
    zlink_msg_t helper2;
    init_part (&helper0, "after");
    init_part (&helper1, "single");
    init_part (&helper2, "final");

    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &helper0, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &helper1, static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &helper2, static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL));
    assert_recv_parts (receiver, "after", "single", "final");
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_send_part_matches_aggregate_wire_shape);
    RUN_TEST (test_send_part_single_final_keeps_next_multipart_valid);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
