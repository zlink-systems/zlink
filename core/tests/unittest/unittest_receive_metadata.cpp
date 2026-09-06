/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "contract_socket_pair_fixture.hpp"
#include "testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include <cstring>

SETUP_TEARDOWN_TESTCONTEXT

void set_recv_timeout_ms (void *socket_, int timeout_ms_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &timeout_ms_, sizeof (timeout_ms_)));
}

void init_part (zlink_msg_t *part_, const char *text_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, strlen (text_)));
    memcpy (zlink_msg_data (part_), text_, strlen (text_));
}

void test_router_direct_single_part_uses_owned_source_rid_storage ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "D1", 2));
    contract_socket_pair_t pair (router, dealer);
    set_recv_timeout_ms (router, 3000);

    socket_handle_t router_handle = as_socket_handle (router);
    TEST_ASSERT_NOT_NULL (router_handle.socket);

    zlink_msg_t outbound;
    init_part (&outbound, "single");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send (dealer, &outbound, 1, static_cast<zlink_send_flags_t> (0)));

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::socket_reqrep_internal::recv_router_message_direct (
        as_socket_handle (router), &source_rid, &request_seq, &parts,
        &part_count, 0));

    TEST_ASSERT_EQUAL_PTR (
      router_handle.socket->last_recv_source_rid_view (), source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_UINT8 (2, source_rid->size);
    TEST_ASSERT_EQUAL_MEMORY ("D1", source_rid->data, 2);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_MEMORY ("single", zlink_msg_data (&parts[0]), 6);
    zlink_multipart_close (parts, part_count);

    router_handle = socket_handle_t ();
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_subscribe_receive_surfaces_reject_request_metadata_after_topic ()
{
    for (int aggregate = 0; aggregate <= 1; ++aggregate) {
        void *pub = test_context_socket (ZLINK_SOCKET_PUB);
        void *sub = test_context_socket (ZLINK_SOCKET_SUB);
        TEST_ASSERT_NOT_NULL (pub);
        TEST_ASSERT_NOT_NULL (sub);

        contract_socket_pair_t pair (pub, sub);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));
        set_recv_timeout_ms (sub, 1000);
        pair.pump ();

        zlink_msg_t payload;
        init_part (&payload, "metadata-after-topic");
        TEST_ASSERT_SUCCESS_ERRNO (
          reinterpret_cast<zlink::msg_t *> (&payload)
            ->set_request_reply_metadata (
              zlink::request_reply::request_type, 77 + aggregate));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_publish (pub, "topic", &payload, 1,
                         ZLINK_SEND_FLAGS_NONE));

        errno = 0;
        int observed_errno = 0;
        if (aggregate) {
            zlink_msg_t *parts = NULL;
            size_t part_count = 0;
            char topic[16];
            size_t topic_len = sizeof (topic);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_INTERNAL_ERROR,
              zlink_subscribe (sub, NULL, &parts, &part_count, topic,
                               &topic_len, ZLINK_RECV_FLAGS_NONE));
            observed_errno = errno;
            TEST_ASSERT_NULL (parts);
            TEST_ASSERT_EQUAL_UINT64 (0, part_count);
        } else {
            char topic[16];
            size_t topic_len = 0;
            zlink_msg_t part;
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
            zlink_part_flag_t has_more = ZLINK_PART_FINAL;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_INTERNAL_ERROR,
              zlink_subscribe_part (
                sub, NULL, topic, sizeof (topic), &topic_len, &part,
                &has_more, ZLINK_RECV_FLAGS_NONE));
            observed_errno = errno;
            TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));
            unsigned char kind = 0xff;
            uint64_t sequence = UINT64_MAX;
            TEST_ASSERT_FALSE (
              reinterpret_cast<zlink::msg_t *> (&part)
                ->get_request_reply_metadata (&kind, &sequence));
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
        }
        TEST_ASSERT_EQUAL_INT (EPROTO, observed_errno);

        test_context_socket_close_zero_linger (sub);
        test_context_socket_close_zero_linger (pub);
    }
}

void test_filtered_subscribe_still_rejects_request_metadata_in_record_tail ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_NOT_NULL (pub);
    TEST_ASSERT_NOT_NULL (sub);
    // Deliver a topic that does not match the SUB-side filter so xhas_in()
    // must drain and structurally validate the complete record.  Keep the SUB
    // itself non-inverted: only the publisher-side distribution is inverted.
    const int invert = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (pub, ZLINK_OPT_INVERT_MATCHING, &invert,
                        sizeof (invert)));
    contract_socket_pair_t pair (pub, sub);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "wanted"));
    set_recv_timeout_ms (sub, 1000);
    pair.pump ();

    zlink_msg_t payload;
    init_part (&payload, "filtered-metadata");
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&payload)
        ->set_request_reply_metadata (
          zlink::request_reply::request_type, 99));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_publish (pub, "other-topic", &payload, 1,
                     ZLINK_SEND_FLAGS_NONE));

    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    char topic[32];
    size_t topic_len = sizeof (topic);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INTERNAL_ERROR,
      zlink_subscribe (sub, NULL, &parts, &part_count, topic, &topic_len,
                       ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (EPROTO, errno);
    TEST_ASSERT_NULL (parts);
    TEST_ASSERT_EQUAL_UINT64 (0, part_count);

    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_router_direct_single_part_uses_owned_source_rid_storage);
    RUN_TEST (test_subscribe_receive_surfaces_reject_request_metadata_after_topic);
    RUN_TEST (test_filtered_subscribe_still_rejects_request_metadata_in_record_tail);
    return UNITY_END ();
}
