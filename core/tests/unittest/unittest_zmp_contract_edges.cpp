/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"
#include "../testutil_unity.hpp"

#include "api/message/submit_result_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "protocol/zmp_encoder.hpp"

#include <limits>

void setUp ()
{
}

void tearDown ()
{
}

void test_zmp_encoder_rejects_payload_larger_than_u32 ()
{
    if (std::numeric_limits<size_t>::max ()
        <= std::numeric_limits<uint32_t>::max ())
        TEST_IGNORE_MESSAGE ("size_t cannot represent an oversized ZMP payload");

    unsigned char borrowed = 0;
    zlink::msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (msg.init_data (
      &borrowed,
      static_cast<size_t> (std::numeric_limits<uint32_t>::max ()) + 1u,
      NULL, NULL));

    zlink::zmp_encoder_t encoder (64);
    errno = 0;
    encoder.load_msg (&msg);
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);

    unsigned char *encoded = NULL;
    TEST_ASSERT_EQUAL_UINT64 (0, encoder.encode (&encoded, 0));
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);
    TEST_ASSERT_SUCCESS_ERRNO (msg.close ());
}

void test_zmp_socket_send_rejects_oversized_borrowed_payload ()
{
    if (std::numeric_limits<size_t>::max ()
        <= std::numeric_limits<uint32_t>::max ())
        TEST_IGNORE_MESSAGE ("size_t cannot represent an oversized ZMP payload");

    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);

    unsigned char borrowed = 0;
    zlink_msg_t msg;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_msg_init_data (
        &msg, &borrowed,
        static_cast<size_t> (std::numeric_limits<uint32_t>::max ()) + 1u,
        NULL, NULL));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, test_send_single_msg (&msg, dealer, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_error_reply_with_zero_errno_becomes_protocol_error ()
{
    zlink_msg_t parts[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[0], 4));
    memset (zlink_msg_data (&parts[0]), 0, 4);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[1], 7));
    memcpy (zlink_msg_data (&parts[1]), "payload", 7);

    int callback_errno = 0;
    zlink_msg_t *callback_parts = parts;
    size_t callback_part_count = 2;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::request_reply::decode_reply_completion (
        zlink::request_reply::error_reply_type, parts, 2, &callback_errno,
        &callback_parts, &callback_part_count));
    TEST_ASSERT_EQUAL_INT (EPROTO, callback_errno);
    TEST_ASSERT_NULL (callback_parts);
    TEST_ASSERT_EQUAL_UINT64 (0, callback_part_count);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&parts[0]));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&parts[1]));
}

void test_missing_completion_pipe_is_not_connected ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (dealer);
    socket_handle_t handle = as_socket_handle (dealer);
    TEST_ASSERT_NOT_NULL (handle.socket);

    zlink_msg_t frame;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&frame, 5));
    memcpy (zlink_msg_data (&frame), "reply", 5);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink::socket_reqrep_internal::send_completion_frames (
            handle.socket, NULL, NULL, &frame, 1));
    TEST_ASSERT_EQUAL_INT (ENOTCONN, errno);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink::submit_result_internal::from_errno (errno));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&frame));

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_zmp_encoder_rejects_payload_larger_than_u32);
    RUN_TEST (test_zmp_socket_send_rejects_oversized_borrowed_payload);
    RUN_TEST (test_error_reply_with_zero_errno_becomes_protocol_error);
    RUN_TEST (test_missing_completion_pipe_is_not_connected);
    return UNITY_END ();
}
