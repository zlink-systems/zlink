/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil_unity.hpp"
#include "contract_socket_pair_fixture.hpp"
#include "api/socket/socket_message_api_internal.hpp"

SETUP_TEARDOWN_TESTCONTEXT

void test_public_inproc_router_send_envelope_blocking ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    const char routing_id[] = "D2";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, routing_id, sizeof (routing_id) - 1));
    contract_socket_pair_t pair (router, dealer);

    zlink_msg_t outbound;
    const char payload[] = "ping";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&outbound, sizeof (payload) - 1));
    memcpy (zlink_msg_data (&outbound), payload, sizeof (payload) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (dealer, &outbound, 1, 0));

    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    uint64_t request_seq = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv (
      router, &source_rid, &request_seq, &received, &part_count, 0));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (routing_id) - 1, source_rid->size);
    TEST_ASSERT_EQUAL_MEMORY (routing_id, source_rid->data, sizeof (routing_id) - 1);
    zlink_multipart_close (received, part_count);

    zlink_msg_t reply_parts[2];
    const char reply_payload[] = "pong";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&reply_parts[0], source_rid->size));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&reply_parts[1], sizeof (reply_payload) - 1));
    memcpy (zlink_msg_data (&reply_parts[0]), source_rid->data, source_rid->size);
    memcpy (zlink_msg_data (&reply_parts[1]), reply_payload, sizeof (reply_payload) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_socket_send_internal (router, reply_parts, 2, ZLINK_SEND_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&reply_parts[0]));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&reply_parts[1]));

    zlink_msg_t *reply_recv = NULL;
    size_t reply_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (dealer, NULL, &reply_recv, &reply_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, reply_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (reply_payload) - 1, zlink_msg_size (&reply_recv[0]));
    TEST_ASSERT_EQUAL_MEMORY (reply_payload, zlink_msg_data (&reply_recv[0]),
                              sizeof (reply_payload) - 1);
    zlink_multipart_close (reply_recv, reply_count);
}
int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_public_inproc_router_send_envelope_blocking);
    return UNITY_END ();
}
