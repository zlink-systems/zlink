/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_TEST_REQUEST_REPLY_FIXTURE_HPP_INCLUDED
#define ZLINK_TEST_REQUEST_REPLY_FIXTURE_HPP_INCLUDED

#include "testutil.hpp"
#include "testutil_unity.hpp"

namespace
{
void exercise_request_reply (void *router_,
                             void *dealer_,
                             const char *endpoint_,
                             const char *request_payload_,
                             const char *reply_payload_,
                             void (*inspect_part_) (zlink_msg_t *) = NULL,
                             void (*progress_) (void *) = NULL,
                             void *progress_data_ = NULL)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer_, "zmp-ws-dealer", 13));
    if (endpoint_) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer_, endpoint_));
        msleep (SETTLE_TIME * 5);
    }

    zlink_msg_t request[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&request[0], strlen (request_payload_)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&request[1], 12));
    memcpy (zlink_msg_data (&request[0]), request_payload_,
            strlen (request_payload_));
    memcpy (zlink_msg_data (&request[1]), "request-tail", 12);
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request (dealer_, NULL, request, 2, ZLINK_SEND_FLAGS_NONE, 5000,
                     NULL, &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    if (progress_)
        progress_ (progress_data_);

    zlink_routing_id_t reply_rid;
    memset (&reply_rid, 0, sizeof (reply_rid));
    zlink_reply_token_t reply_token = 0;
    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t received[2];
    size_t received_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router_, &source_rid, &reply_token, received, 2,
                         &received_count, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_NOT_EQUAL (0, reply_token);
    TEST_ASSERT_EQUAL_UINT64 (2, received_count);
    reply_rid = *source_rid;
    TEST_ASSERT_EQUAL_STRING_LEN (
      request_payload_,
      static_cast<const char *> (zlink_msg_data (&received[0])),
      strlen (request_payload_));
    TEST_ASSERT_EQUAL_STRING_LEN (
      "request-tail",
      static_cast<const char *> (zlink_msg_data (&received[1])), 12);
    if (inspect_part_) {
        inspect_part_ (&received[0]);
        inspect_part_ (&received[1]);
    }
    zlink_multipart_close (received, received_count);

    zlink_msg_t reply[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&reply[0], strlen (reply_payload_)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&reply[1], 10));
    memcpy (zlink_msg_data (&reply[0]), reply_payload_, strlen (reply_payload_));
    memcpy (zlink_msg_data (&reply[1]), "reply-tail", 10);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply (router_, &reply_rid, reply_token, reply, 2));

    if (progress_)
        progress_ (progress_data_);

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    zlink_recv_result_t completion_rc = ZLINK_RECV_NO_DATA;
    for (int attempt = 0; attempt < (progress_ ? 1 : 5000)
                          && completion_rc == ZLINK_RECV_NO_DATA;
         ++attempt) {
        completion_rc = zlink_completion_recv (
          dealer_, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (completion_rc == ZLINK_RECV_NO_DATA && !progress_)
            msleep (1);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, completion_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (2, completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING_LEN (
      reply_payload_, static_cast<const char *> (
                        zlink_msg_data (&completion.reply_parts[0])),
      strlen (reply_payload_));
    if (inspect_part_)
        inspect_part_ (&completion.reply_parts[0]);
    zlink_completion_close (&completion);
}

}

#endif
