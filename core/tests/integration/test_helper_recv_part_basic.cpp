/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
zlink_completion_t wait_for_request_completion (void *socket_, int timeout_ms_)
{
    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_recv_result_t rc = zlink_completion_recv (
          socket_, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_OK)
            return completion;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }

    TEST_FAIL_MESSAGE ("timed out waiting for REQUEST completion");
    return completion;
}

void set_recv_timeout_ms (void *socket_, int timeout_ms_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &timeout_ms_, sizeof (timeout_ms_)));
}

zlink_recv_result_t recv_router_part_with_retry (void *router_,
                                                 const zlink_routing_id_t **source_node_rid_out_,
                                                 uint64_t *request_seq_out_,
                                                 zlink_msg_t *part_out_,
                                                 zlink_part_flag_t *has_more_out_)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_recv_result_t rc = zlink_router_recv_part (
          router_, source_node_rid_out_, request_seq_out_, part_out_,
          has_more_out_, static_cast<zlink_recv_flags_t> (0));
        if (rc == ZLINK_RECV_OK)
            return rc;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }

    TEST_FAIL_MESSAGE ("timed out waiting for zlink_router_recv_part");
    return ZLINK_RECV_INTERNAL_ERROR;
}

zlink_recv_result_t recv_subscribe_part_with_retry (void *sub_,
                                                    char *topic_id_buf_,
                                                    size_t topic_id_capacity_,
                                                    size_t *topic_id_len_out_,
                                                    zlink_msg_t *part_out_,
                                                    zlink_part_flag_t *has_more_out_)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_recv_result_t rc =
          zlink_subscribe_part (sub_, NULL, topic_id_buf_, topic_id_capacity_, topic_id_len_out_,
                                part_out_, has_more_out_, static_cast<zlink_recv_flags_t> (0));
        if (rc == ZLINK_RECV_OK || zlink_errno () == EMSGSIZE)
            return rc;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }

    TEST_FAIL_MESSAGE ("timed out waiting for zlink_subscribe_part");
    return ZLINK_RECV_INTERNAL_ERROR;
}

void init_part (zlink_msg_t *part_, const char *text_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, strlen (text_)));
    memcpy (zlink_msg_data (part_), text_, strlen (text_));
}

zlink_completion_id_t send_dealer_request_single (void *dealer_,
                                                   const char *part0_)
{
    zlink_msg_t part;
    init_part (&part, part0_);
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer_, NULL, &part, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_FINAL, 3000, NULL, &completion_id));
    TEST_ASSERT_TRUE (completion_id != 0);
    return completion_id;
}

void reply_from_router (void *router_,
                        const zlink_routing_id_t *peer_rid_,
                        uint64_t request_seq_,
                        const char *payload_)
{
    zlink_msg_t part;
    init_part (&part, payload_);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router_, peer_rid_, request_seq_, &part,
                        ZLINK_PART_FINAL));
}

void publish_and_recv_subscribe_part_eventually (void *pub_,
                                                 void *sub_,
                                                 const char *topic_,
                                                 const char *payload_,
                                                 char *topic_id_buf_,
                                                 size_t topic_id_capacity_,
                                                 size_t *topic_id_len_out_,
                                                 zlink_msg_t *part_out_,
                                                 zlink_part_flag_t *has_more_out_,
                                                 zlink_recv_result_t expected_rc_,
                                                 int expected_errno_)
{
    zlink_msg_t part;
    init_part (&part, payload_);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_publish (pub_, topic_, &part, 1,
                     static_cast<zlink_send_flags_t> (0)));

    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_recv_result_t rc = zlink_subscribe_part (
          sub_, NULL, topic_id_buf_, topic_id_capacity_, topic_id_len_out_, part_out_,
          has_more_out_, static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
        if (rc == expected_rc_ && zlink_errno () == expected_errno_)
            return;
        if (rc == ZLINK_RECV_OK && expected_rc_ == ZLINK_RECV_OK)
            return;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }

    TEST_FAIL_MESSAGE ("timed out waiting for publish/subscribe helper exchange");
}

}

void test_recv_part_reads_each_part_and_tracks_has_more ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *receiver = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (receiver, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://helper-recv-part-basic"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (receiver, "inproc://helper-recv-part-basic"));
    set_recv_timeout_ms (receiver, 50);
    msleep (SETTLE_TIME);

    zlink_msg_t ping;
    init_part (&ping, "prime");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send (receiver, &ping, 1, static_cast<zlink_send_flags_t> (0)));

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *primed = NULL;
    size_t primed_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv (router, &source_rid,
                                                  &request_seq, &primed, &primed_count,
                                                  static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    zlink_multipart_close (primed, primed_count);

    zlink_msg_t parts[2];
    init_part (&parts[0], "one");
    init_part (&parts[1], "two");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send_rid (router, source_rid, parts, 2, static_cast<zlink_send_flags_t> (0)));
    msleep (SETTLE_TIME);

    const char *expected[2] = {"one", "two"};
    for (int i = 0; i < 2; ++i) {
        zlink_msg_t part;
        zlink_msg_init (&part);
        const zlink_routing_id_t *part_source_rid =
          reinterpret_cast<const zlink_routing_id_t *> (0x1);
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_recv_part (receiver, &part_source_rid, &part, &has_more,
                                                    static_cast<zlink_recv_flags_t> (0)));
        TEST_ASSERT_NULL (part_source_rid);
        TEST_ASSERT_EQUAL_UINT64 (strlen (expected[i]), zlink_msg_size (&part));
        TEST_ASSERT_EQUAL_MEMORY (expected[i], zlink_msg_data (&part), strlen (expected[i]));
        TEST_ASSERT_EQUAL_INT (i == 1 ? 0 : 1, has_more);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    }

    test_context_socket_close_zero_linger (receiver);
    test_context_socket_close_zero_linger (router);
}


void test_router_recv_part_metadata_view_invalidates_on_next_recv_like_call ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer1 = test_context_socket (ZLINK_SOCKET_DEALER);
    void *dealer2 = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer1, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer2, "D2", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://helper-router-recv-part-basic"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer1, "inproc://helper-router-recv-part-basic"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer2, "inproc://helper-router-recv-part-basic"));
    set_recv_timeout_ms (router, 50);
    msleep (SETTLE_TIME);

    const zlink_routing_id_t *primed_source_rid = NULL;
    uint64_t primed_request_seq = 0;
    zlink_msg_t *primed_parts = NULL;
    size_t primed_part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA, zlink_router_recv (router, &primed_source_rid,
                                             &primed_request_seq, &primed_parts, &primed_part_count,
                                             static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    const zlink_completion_id_t completion_id1 =
      send_dealer_request_single (dealer1, "first");

    const zlink_routing_id_t *first_source_rid = NULL;
    uint64_t first_request_seq = 0;
    zlink_msg_t part;
    zlink_msg_init (&part);
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    TEST_ASSERT_SUCCESS_ERRNO (recv_router_part_with_retry (
      router, &first_source_rid, &first_request_seq, &part, &has_more));
    TEST_ASSERT_NOT_NULL (first_source_rid);
    TEST_ASSERT_TRUE (first_request_seq != 0);
    TEST_ASSERT_EQUAL_INT (0, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("D1", first_source_rid->data, 2);
    TEST_ASSERT_EQUAL_MEMORY ("first", zlink_msg_data (&part), 5);
    zlink_routing_id_t first_source_copy = *first_source_rid;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));

    // A recv entry on another socket must not invalidate router's borrowed
    // view, even when that other entry returns NO_DATA.
    void *other_router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (other_router);
    const zlink_routing_id_t *other_source_rid =
      reinterpret_cast<const zlink_routing_id_t *> (0x1);
    zlink_reply_token_t other_token = UINT64_MAX;
    zlink_msg_t other_part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&other_part));
    zlink_part_flag_t other_more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_router_recv_part (other_router, &other_source_rid, &other_token,
                              &other_part, &other_more,
                              ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_MEMORY ("D1", first_source_rid->data, 2);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&other_part));
    test_context_socket_close_zero_linger (other_router);

    reply_from_router (router, first_source_rid, first_request_seq, "reply-1");
    zlink_completion_t completion1 = wait_for_request_completion (dealer1, 3000);
    TEST_ASSERT_EQUAL_UINT64 (completion_id1, completion1.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion1.request_result);
    zlink_completion_close (&completion1);

    const zlink_completion_id_t completion_id2 =
      send_dealer_request_single (dealer2, "second");

    const zlink_routing_id_t *second_source_rid = NULL;
    const zlink_routing_id_t *second_spot_rid = NULL;
    uint64_t second_request_seq = 0;
    zlink_msg_init (&part);
    has_more = ZLINK_PART_FINAL;
    TEST_ASSERT_SUCCESS_ERRNO (recv_router_part_with_retry (
      router, &second_source_rid, &second_request_seq, &part, &has_more));
    TEST_ASSERT_NOT_NULL (second_source_rid);
    TEST_ASSERT_EQUAL_MEMORY ("D2", second_source_rid->data, 2);
    TEST_ASSERT_EQUAL_INT (0, has_more);

    TEST_ASSERT_EQUAL_MEMORY (second_source_rid->data, first_source_rid->data,
                              second_source_rid->size);
    TEST_ASSERT_FALSE (
      first_source_copy.size == first_source_rid->size
      && memcmp (first_source_copy.data, first_source_rid->data, first_source_copy.size) == 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    reply_from_router (router, second_source_rid, second_request_seq, "reply-2");
    zlink_completion_t completion2 = wait_for_request_completion (dealer2, 3000);
    TEST_ASSERT_EQUAL_UINT64 (completion_id2, completion2.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion2.request_result);
    zlink_completion_close (&completion2);

    test_context_socket_close_zero_linger (dealer2);
    test_context_socket_close_zero_linger (dealer1);
    test_context_socket_close_zero_linger (router);
}

void test_subscribe_part_reports_needed_topic_size_without_consuming_payload ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, "inproc://helper-subscribe-part-basic"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, "inproc://helper-subscribe-part-basic"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));
    set_recv_timeout_ms (sub, 50);
    msleep (SETTLE_TIME * 6);

    char small_topic[2] = {'x', 'y'};
    size_t topic_len = 0;
    zlink_msg_t payload;
    zlink_msg_init (&payload);
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    publish_and_recv_subscribe_part_eventually (pub, sub, "topic-1", "payload-1", small_topic,
                                                sizeof (small_topic), &topic_len, &payload,
                                                &has_more, ZLINK_RECV_BUFFER_TOO_SMALL, ENOBUFS);
    TEST_ASSERT_EQUAL_INT (ENOBUFS, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (7, topic_len);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, has_more);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&payload));
    TEST_ASSERT_EQUAL_MEMORY ("xy", small_topic, sizeof (small_topic));

    char full_topic[7];
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_subscribe_part_with_retry (sub, full_topic, sizeof (full_topic),
                                      &topic_len, &payload, &has_more));
    TEST_ASSERT_EQUAL_MEMORY ("topic-1", full_topic, sizeof (full_topic));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("payload-1", zlink_msg_data (&payload), 9);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&payload));

    char sentinel[4] = {'k', 'e', 'e', 'p'};
    topic_len = 0;
    zlink_msg_init (&payload);
    has_more = ZLINK_PART_MORE;
    publish_and_recv_subscribe_part_eventually (
      pub, sub, "topic-2", "payload-2", sentinel, 0, &topic_len, &payload,
      &has_more, ZLINK_RECV_BUFFER_TOO_SMALL, ENOBUFS);
    TEST_ASSERT_EQUAL_INT (ENOBUFS, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (7, topic_len);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("keep", sentinel, sizeof (sentinel));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&payload));

    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_subscribe_part_with_retry (sub, full_topic, sizeof (full_topic),
                                      &topic_len, &payload, &has_more));
    TEST_ASSERT_EQUAL_MEMORY ("topic-2", full_topic, sizeof (full_topic));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("payload-2", zlink_msg_data (&payload), 9);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&payload));

    zlink_msg_t positive_null_part;
    init_part (&positive_null_part, "payload-3");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_publish (pub, "topic-3", &positive_null_part, 1,
                     static_cast<zlink_send_flags_t> (0)));
    topic_len = 91;
    zlink_msg_init (&payload);
    has_more = ZLINK_PART_MORE;
    const zlink_routing_id_t *source_rid =
      reinterpret_cast<const zlink_routing_id_t *> (0x1);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_HANDLE,
      zlink_subscribe_part (
        sub, &source_rid, NULL, 1, &topic_len, &payload, &has_more,
        static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (EFAULT, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (91, topic_len);
    TEST_ASSERT_EQUAL_PTR (
      reinterpret_cast<const zlink_routing_id_t *> (0x1), source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&payload));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, has_more);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      recv_subscribe_part_with_retry (sub, full_topic, sizeof (full_topic),
                                      &topic_len, &payload, &has_more));
    TEST_ASSERT_EQUAL_MEMORY ("topic-3", full_topic, sizeof (full_topic));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("payload-3", zlink_msg_data (&payload), 9);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&payload));

    topic_len = 47;
    zlink_msg_init (&payload);
    has_more = ZLINK_PART_MORE;
    publish_and_recv_subscribe_part_eventually (
      pub, sub, "", "empty-topic", NULL, 0, &topic_len, &payload,
      &has_more, ZLINK_RECV_OK, 0);
    TEST_ASSERT_EQUAL_UINT64 (0, topic_len);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_MEMORY ("empty-topic", zlink_msg_data (&payload), 11);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&payload));

    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}



int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_recv_part_reads_each_part_and_tracks_has_more);
    RUN_TEST (test_router_recv_part_metadata_view_invalidates_on_next_recv_like_call);
    RUN_TEST (
      test_subscribe_part_reports_needed_topic_size_without_consuming_payload);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
