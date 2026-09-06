/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <unity.h>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

void setUp ()
{
    setup_test_context ();
}

void tearDown ()
{
    teardown_test_context ();
}

namespace
{
void *create_sync_socket (int type_)
{
    void *socket = zlink_socket (get_test_context (), static_cast<zlink_socket_type_t> (type_));
    TEST_ASSERT_NOT_NULL (socket);
    return socket;
}

void close_sync_socket (void *socket_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_));
}

void recv_router_payload (void *router_, const char *expected_rid_,
                          const char *expected_payload_)
{
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = UINT64_MAX;
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router_, &source_rid, &request_seq, &part,
                              &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (strlen (expected_rid_), source_rid->size);
    TEST_ASSERT_EQUAL_MEMORY (expected_rid_, source_rid->data, source_rid->size);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_UINT64 (strlen (expected_payload_), zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_MEMORY (expected_payload_, zlink_msg_data (&part),
                              zlink_msg_size (&part));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
}


}

void test_router_multiple_dealers_tcp ()
{
    void *router = create_sync_socket (ZLINK_SOCKET_ROUTER);
    void *dealer1 = create_sync_socket (ZLINK_SOCKET_DEALER);
    void *dealer2 = create_sync_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer1, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer2, "D2", 2));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "tcp://127.0.0.1:*"));

    char endpoint[MAX_SOCKET_STRING];
    size_t len = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (router, ZLINK_OPT_LAST_ENDPOINT, endpoint, &len));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer1, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer2, endpoint));

    msleep (SETTLE_TIME);

    // Both dealers send messages
    send_string_expect_success (dealer1, "from_dealer1", 0);
    recv_router_payload (router, "D1", "from_dealer1");
    send_string_expect_success (dealer2, "from_dealer2", 0);
    recv_router_payload (router, "D2", "from_dealer2");

    // Router can reply to specific dealer
    send_routed_string_expect_success (router, "D1", "reply_to_d1");

    send_routed_string_expect_success (router, "D2", "reply_to_d2");

    // Dealers receive their specific replies
    recv_string_expect_success (dealer1, "reply_to_d1", 0);
    recv_string_expect_success (dealer2, "reply_to_d2", 0);

    close_sync_socket (dealer2);
    close_sync_socket (dealer1);
    close_sync_socket (router);
}

void test_router_multiple_dealers_ipc ()
{
#if defined(ZLINK_HAVE_IPC)
    void *router = create_sync_socket (ZLINK_SOCKET_ROUTER);
    void *dealer1 = create_sync_socket (ZLINK_SOCKET_DEALER);
    void *dealer2 = create_sync_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer1, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer2, "D2", 2));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "ipc://*"));

    char endpoint[MAX_SOCKET_STRING];
    size_t len = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (router, ZLINK_OPT_LAST_ENDPOINT, endpoint, &len));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer1, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer2, endpoint));

    msleep (SETTLE_TIME);

    // Both dealers send messages
    send_string_expect_success (dealer1, "from_dealer1", 0);
    recv_router_payload (router, "D1", "from_dealer1");
    send_string_expect_success (dealer2, "from_dealer2", 0);
    recv_router_payload (router, "D2", "from_dealer2");

    // Router replies to specific dealers
    send_routed_string_expect_success (router, "D1", "reply_to_d1");

    send_routed_string_expect_success (router, "D2", "reply_to_d2");

    recv_string_expect_success (dealer1, "reply_to_d1", 0);
    recv_string_expect_success (dealer2, "reply_to_d2", 0);

    close_sync_socket (dealer2);
    close_sync_socket (dealer1);
    close_sync_socket (router);
#else
    TEST_IGNORE_MESSAGE ("IPC not supported on this platform");
#endif
}

void test_router_multiple_dealers_inproc ()
{
    void *router = create_sync_socket (ZLINK_SOCKET_ROUTER);
    void *dealer1 = create_sync_socket (ZLINK_SOCKET_DEALER);
    void *dealer2 = create_sync_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer1, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer2, "D2", 2));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://test_router_multi_dealers"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer1, "inproc://test_router_multi_dealers"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer2, "inproc://test_router_multi_dealers"));

    // Both dealers send messages
    send_string_expect_success (dealer1, "from_dealer1", 0);
    recv_router_payload (router, "D1", "from_dealer1");
    send_string_expect_success (dealer2, "from_dealer2", 0);
    recv_router_payload (router, "D2", "from_dealer2");

    // Router replies to specific dealers
    send_routed_string_expect_success (router, "D1", "reply_to_d1");

    send_routed_string_expect_success (router, "D2", "reply_to_d2");

    recv_string_expect_success (dealer1, "reply_to_d1", 0);
    recv_string_expect_success (dealer2, "reply_to_d2", 0);

    close_sync_socket (dealer2);
    close_sync_socket (dealer1);
    close_sync_socket (router);
}

void test_weighted_dealer_preserves_peer_weight_after_backpressure ()
{
    void *dealer = create_sync_socket (ZLINK_SOCKET_DEALER);
    void *router1 = create_sync_socket (ZLINK_SOCKET_ROUTER);
    void *router2 = create_sync_socket (ZLINK_SOCKET_ROUTER);

    const uint64_t hwm = 64u + sizeof (zlink_msg_t);
    const int timeout = 0;
    const int weight1 = 25;
    const int weight2 = 100;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router1, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router2, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_SNDTIMEO, &timeout, sizeof (timeout)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (
        router1, ZLINK_ROUTER_OPT_WEIGHT, &weight1, sizeof (weight1)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (
        router2, ZLINK_ROUTER_OPT_WEIGHT, &weight2, sizeof (weight2)));

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router1, "inproc://weighted-backpressure-1"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router2, "inproc://weighted-backpressure-2"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://weighted-backpressure-1"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://weighted-backpressure-2"));
    msleep (SETTLE_TIME);

    bool backpressured = false;
    for (int i = 0; i < 1000; ++i) {
        if (zlink_send (dealer, "fill", 4, ZLINK_DONTWAIT) == -1) {
            TEST_ASSERT_TRUE (errno == EAGAIN || errno == ECONNREFUSED);
            backpressured = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE (
      backpressured, "weighted dealer did not reach the backpressure path");

    char buffer[16];
    while (test_recv_router (router1, buffer, sizeof (buffer), ZLINK_DONTWAIT) >= 0) {
    }
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    while (test_recv_router (router2, buffer, sizeof (buffer), ZLINK_DONTWAIT) >= 0) {
    }
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    msleep (SETTLE_TIME);
    while (test_recv_router (router1, buffer, sizeof (buffer), ZLINK_DONTWAIT) >= 0) {
    }
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    while (test_recv_router (router2, buffer, sizeof (buffer), ZLINK_DONTWAIT) >= 0) {
    }
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    int router1_received = 0;
    int router2_received = 0;
    for (int i = 0; i < 40; ++i) {
        int send_rc = -1;
        for (int attempt = 0; attempt < 1000 && send_rc == -1; ++attempt) {
            send_rc = zlink_send (dealer, "next", 4, ZLINK_DONTWAIT);
            if (send_rc == -1)
                msleep (1);
        }
        TEST_ASSERT_EQUAL_INT (4, send_rc);

        bool received = false;
        for (int attempt = 0; attempt < 1000 && !received; ++attempt) {
            void *routers[] = {router1, router2};
            int *counts[] = {&router1_received, &router2_received};
            for (size_t router_index = 0; router_index < 2; ++router_index) {
                const zlink_routing_id_t *source_rid = NULL;
                uint64_t request_seq = UINT64_MAX;
                zlink_msg_t part;
                TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
                zlink_part_flag_t has_more = ZLINK_PART_MORE;
                const zlink_recv_result_t rc = zlink_router_recv_part (
                  routers[router_index], &source_rid, &request_seq, &part,
                  &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
                if (rc == ZLINK_RECV_NO_DATA) {
                    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
                    continue;
                }
                TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
                TEST_ASSERT_NOT_NULL (source_rid);
                TEST_ASSERT_GREATER_THAN_INT (0, source_rid->size);
                TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
                TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
                TEST_ASSERT_EQUAL_UINT64 (4, zlink_msg_size (&part));
                TEST_ASSERT_EQUAL_MEMORY ("next", zlink_msg_data (&part), 4);
                TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
                *counts[router_index] += 1;
                received = true;
                break;
            }
            if (!received)
                msleep (1);
        }
        TEST_ASSERT_TRUE (received);
    }

    TEST_ASSERT_GREATER_THAN_INT (
      0, router1_received);
    TEST_ASSERT_GREATER_THAN_INT (
      0, router2_received);

    close_sync_socket (router2);
    close_sync_socket (router1);
    close_sync_socket (dealer);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_router_multiple_dealers_tcp);
    RUN_TEST (test_router_multiple_dealers_ipc);
    RUN_TEST (test_router_multiple_dealers_inproc);
    RUN_TEST (test_weighted_dealer_preserves_peer_weight_after_backpressure);
    return UNITY_END ();
}
