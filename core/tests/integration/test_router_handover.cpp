/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cstring>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
bool should_run_router_handover_test (const char *name_)
{
    const char *selected = getenv ("ZLINK_TEST_CASE");
    return !selected || !*selected || strcmp (selected, name_) == 0;
}

void set_connect_routing_id (void *router_, const char *routing_id_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      router_, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, routing_id_, strlen (routing_id_)));
}

void send_request_to_exercise_completion_path (void *client_,
                                               void *server_,
                                               const char *peer_rid_,
                                               const char *expected_source_rid_ = NULL)
{
    zlink_routing_id_t peer_rid;
    memset (&peer_rid, 0, sizeof peer_rid);
    peer_rid.size = static_cast<uint8_t> (strlen (peer_rid_));
    memcpy (peer_rid.data, peer_rid_, peer_rid.size);

    // Connection establishment and routing-id attachment are asynchronous.
    // Synchronize on the route that the request below actually needs instead
    // of assuming that a fixed settle delay made it visible.
    bool route_ready = false;
    for (int i = 0; i < 100 && !route_ready; ++i) {
        zlink_msg_t ready;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&ready, 5));
        memcpy (zlink_msg_data (&ready), "ready", 5);
        const zlink_submit_result_t submit = zlink_send_part_rid (
          client_, &peer_rid, &ready, ZLINK_SEND_FLAGS_NONE,
          ZLINK_PART_FINAL, NULL, NULL);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&ready));
        if (submit != ZLINK_SUBMIT_OK) {
            msleep (10);
            continue;
        }

        zlink_msg_t received;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
        const zlink_routing_id_t *source = NULL;
        zlink_reply_token_t token = UINT64_MAX;
        zlink_part_flag_t has_more = ZLINK_PART_MORE;
        const zlink_recv_result_t recv_rc = zlink_router_recv_part (
          server_, &source, &token, &received, &has_more,
          ZLINK_RECV_FLAGS_NONE);
        if (recv_rc == ZLINK_RECV_OK) {
            TEST_ASSERT_NOT_NULL (source);
            TEST_ASSERT_EQUAL_UINT64 (0, token);
            TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
            TEST_ASSERT_EQUAL_STRING_LEN (
              "ready", static_cast<const char *> (zlink_msg_data (&received)),
              5);
            route_ready = true;
        }
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));
    }
    TEST_ASSERT_TRUE_MESSAGE (
      route_ready, "router route did not become ready for request");

    zlink_msg_t request;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&request, 4));
    memcpy (zlink_msg_data (&request), "ping", 4);
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (client_, &peer_rid, &request, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_FINAL, 30000, NULL, &completion_id));
    TEST_ASSERT_NOT_EQUAL (0, completion_id);

    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t reply_token = 0;
    zlink_msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (server_, &source_rid, &reply_token, &received,
                              &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_EQUAL (0, reply_token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    if (expected_source_rid_) {
        TEST_ASSERT_NOT_NULL (source_rid);
        TEST_ASSERT_EQUAL_UINT8 (static_cast<uint8_t> (strlen (expected_source_rid_)),
                                 source_rid->size);
        TEST_ASSERT_EQUAL_MEMORY (expected_source_rid_, source_rid->data,
                                  source_rid->size);
    }

    zlink_msg_t reply;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&reply, 4));
    memcpy (zlink_msg_data (&reply), "pong", 4);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (server_, source_rid, reply_token, &reply,
                        ZLINK_PART_FINAL));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    zlink_recv_result_t completion_rc = ZLINK_RECV_NO_DATA;
    for (int i = 0; i < 1000 && completion_rc == ZLINK_RECV_NO_DATA; ++i) {
        completion_rc = zlink_completion_recv (
          client_, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (completion_rc == ZLINK_RECV_NO_DATA)
            msleep (1);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, completion_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (completion_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.reply_part_count);
    TEST_ASSERT_EQUAL_STRING_LEN (
      "pong", static_cast<const char *> (
                zlink_msg_data (&completion.reply_parts[0])), 4);
    zlink_completion_close (&completion);
}
}

void test_callback_dispatch_same_direction_reconnect_handover ()
{
    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    const int zero = 0;
    const int probe_timeout = 100;
    char endpoint_one[MAX_SOCKET_STRING];
    char endpoint_two[MAX_SOCKET_STRING];

    void *server_one = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server_one, "S", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_one, ZLINK_OPT_LINGER, &zero, sizeof zero));
    bind_loopback_ipv4 (server_one, endpoint_one, sizeof endpoint_one);

    void *server_two = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server_two, "S", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_two, ZLINK_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_two, ZLINK_OPT_RCVTIMEO, &probe_timeout,
                        sizeof probe_timeout));
    bind_loopback_ipv4 (server_two, endpoint_two, sizeof endpoint_two);

    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, "C", 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_RCVTIMEO, &probe_timeout,
                        sizeof probe_timeout));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover, sizeof handover));
    set_connect_routing_id (client, "S");
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint_one));

    // This installs callback dispatch and records traffic on the original pipe.
    send_request_to_exercise_completion_path (client, server_one, "S");

    // A freshly established same-direction pipe with the same routing ID must
    // replace the prior pipe even though the prior pipe has traffic history.
    set_connect_routing_id (client, "S");
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint_two));

    //  The second pipe attaches asynchronously, so a probe sent before the
    //  handover completes is still routed to server_one. Keep probing until
    //  server_two receives one; the replacement decision is under test, not
    //  the attach timing. With the pre-fix admission condition every probe
    //  keeps landing on the old pipe and the loop exhausts its 5 second bound.
    char buffer[255];
    bool handed_over = false;
    for (int i = 0; i < 50 && !handed_over; ++i) {
        send_string_expect_success (client, "S", ZLINK_SNDMORE);
        send_string_expect_success (client, "recovered", 0);
        const int rc = zlink_recv (server_two, buffer, sizeof buffer, 0);
        if (rc == -1) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (1, rc);
        TEST_ASSERT_EQUAL_INT ('C', buffer[0]);
        recv_string_expect_success (server_two, "recovered", 0);
        handed_over = true;
    }
    TEST_ASSERT_TRUE_MESSAGE (
      handed_over, "same-direction reconnect was not handed over to the new pipe");

    //  A paired Application pipe may still be selected explicitly by its
    //  transport-pair identity while the owner validates the replacement.
    //  Preserve the displaced pipe as standby instead of terminating it at
    //  routing-id handover time.
    send_string_expect_success (server_one, "C", ZLINK_SNDMORE);
    send_string_expect_success (server_one, "standby", 0);
    TEST_ASSERT_GREATER_THAN_INT (0, zlink_recv (client, buffer, sizeof buffer, 0));
    recv_string_expect_success (client, "standby", 0);

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server_two);
    test_context_socket_close_zero_linger (server_one);
}

void test_callback_dispatch_cross_direction_duplicate_converges ()
{
    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    const int zero = 0;
    const int probe_timeout = 100;
    char client_endpoint[MAX_SOCKET_STRING];
    char replacement_endpoint[MAX_SOCKET_STRING];

    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, "Z", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover, sizeof handover));
    bind_loopback_ipv4 (client, client_endpoint, sizeof client_endpoint);

    void *server_one = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server_one, "A", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_one, ZLINK_OPT_LINGER, &zero, sizeof zero));
    set_connect_routing_id (server_one, "Z");
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (server_one, client_endpoint));

    send_request_to_exercise_completion_path (client, server_one, "A");

    void *server_two = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server_two, "A", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_two, ZLINK_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_two, ZLINK_OPT_RCVTIMEO, &probe_timeout,
                        sizeof probe_timeout));
    bind_loopback_ipv4 (server_two, replacement_endpoint, sizeof replacement_endpoint);

    set_connect_routing_id (client, "A");
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, replacement_endpoint));

    // Z sorts after A, so the A -> Z connection is the single direction both
    // peers select. A duplicate in the opposite direction must not displace it
    // and start an endless reconnect/handover loop.
    msleep (SETTLE_TIME);
    for (int i = 0; i < 10; ++i) {
        send_string_expect_success (client, "A", ZLINK_SNDMORE);
        send_string_expect_success (client, "stable", 0);
        recv_string_expect_success (server_one, "Z", 0);
        recv_string_expect_success (server_one, "stable", 0);
    }

    char buffer[255];
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (server_two, buffer, sizeof buffer, 0));

    //  The losing physical direction remains as an idle standby. When the
    //  selected direction closes, ROUTER promotes the existing standby
    //  without waiting for another TCP connection.
    test_context_socket_close_zero_linger (server_one);
    server_one = NULL;
    bool recovered = false;
    for (int i = 0; i < 50 && !recovered; ++i) {
        const int rc = zlink_send (client, "A", 1, ZLINK_SNDMORE);
        if (rc == -1) {
            TEST_ASSERT_EQUAL_INT (EHOSTUNREACH, errno);
            msleep (10);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (1, rc);
        send_string_expect_success (client, "recovered", 0);
        const int received = zlink_recv (server_two, buffer, sizeof buffer, 0);
        if (received == -1) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (1, received);
        TEST_ASSERT_EQUAL_INT ('Z', buffer[0]);
        recv_string_expect_success (server_two, "recovered", 0);
        recovered = true;
    }
    TEST_ASSERT_TRUE_MESSAGE (
      recovered, "standby connector did not restore the route after selected pipe loss");

    //  The promoted reciprocal standby still carries the peer's stable node
    //  identity. The routed receive surface must not expose its internal
    //  five-byte standby routing ID to request handlers.
    send_request_to_exercise_completion_path (client, server_two, "A", "Z");

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server_two);
}

void test_repeated_cross_direction_reconnect_uses_current_endpoint ()
{
    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    const int zero = 0;
    char client_endpoint[MAX_SOCKET_STRING];

    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, "Z", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_RID_DUPLICATE_POLICY,
                        &handover, sizeof handover));
    bind_loopback_ipv4 (
      client, client_endpoint, sizeof client_endpoint);

    for (int lifecycle = 0; lifecycle < 16; ++lifecycle) {
        char server_endpoint[MAX_SOCKET_STRING];
        void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_routing_id (server, "A", 1));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (server, ZLINK_OPT_LINGER,
                            &zero, sizeof zero));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (server, ZLINK_OPT_RID_DUPLICATE_POLICY,
                            &handover, sizeof handover));
        bind_loopback_ipv4 (
          server, server_endpoint, sizeof server_endpoint);

        //  Both nodes configure a connector. A sorts before Z, so A -> Z is
        //  the selected direction and Z -> A remains the reciprocal standby.
        set_connect_routing_id (server, "Z");
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_connect (server, client_endpoint));
        set_connect_routing_id (client, "A");
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_connect (client, server_endpoint));

        send_request_to_exercise_completion_path (
          client, server, "A");

        //  Discovery removes the retired endpoint before the same RID is
        //  observed at the next endpoint. No physical pipe from this
        //  lifecycle may later take ownership of the stable RID.
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_disconnect (client, server_endpoint));
        test_context_socket_close_zero_linger (server);
        msleep (SETTLE_TIME);
    }

    test_context_socket_close_zero_linger (client);
}

void test_async_handshake_preserves_outgoing_direction ()
{
    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    const int zero = 0;
    const int probe_timeout = 500;
    char client_endpoint[MAX_SOCKET_STRING];
    char server_endpoint[MAX_SOCKET_STRING];

    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, "C", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover, sizeof handover));
    bind_loopback_ipv4 (client, client_endpoint, sizeof client_endpoint);

    void *endpoint_reservation = test_context_socket (ZLINK_SOCKET_ROUTER);
    bind_loopback_ipv4 (endpoint_reservation, server_endpoint, sizeof server_endpoint);
    test_context_socket_close_zero_linger (endpoint_reservation);

    zlink_routing_id_t unavailable_rid;
    memset (&unavailable_rid, 0, sizeof unavailable_rid);
    unavailable_rid.size = 1;
    unavailable_rid.data[0] = 'X';
    zlink_msg_t unavailable_request;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&unavailable_request, 1));
    *static_cast<unsigned char *> (zlink_msg_data (&unavailable_request)) = 0;
    zlink_completion_id_t unavailable_completion_id = 0;
    (void) zlink_request_part (
      client, &unavailable_rid, &unavailable_request,
      ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 1, NULL,
      &unavailable_completion_id);

    // Start the connection before the peer exists so its routing id cannot be
    // available when the locally initiated pipe is first attached.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, server_endpoint));
    msleep (SETTLE_TIME);

    void *server_one = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server_one, "S", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_one, ZLINK_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_one, ZLINK_OPT_RCVTIMEO, &probe_timeout,
                        sizeof probe_timeout));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server_one, server_endpoint));

    // No connect routing id is supplied. The client therefore learns S from
    // the asynchronous handshake and must retain that it initiated this pipe.
    send_request_to_exercise_completion_path (client, server_one, "S");

    void *server_two = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server_two, "S", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_two, ZLINK_OPT_LINGER, &zero, sizeof zero));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_two, ZLINK_OPT_RCVTIMEO, &probe_timeout,
                        sizeof probe_timeout));

    // C sorts before S, so the original C -> S ROUTER direction must remain
    // selected when a new inbound peer claims S. Losing the anonymous pipe's
    // direction makes this look like a same-side reconnect and incorrectly
    // hands S over to the DEALER.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (server_two, client_endpoint));
    send_string_expect_success (server_two, "identify", 0);
    msleep (SETTLE_TIME);

    for (int i = 0; i < 10; ++i) {
        send_string_expect_success (client, "S", ZLINK_SNDMORE);
        send_string_expect_success (client, "stable", 0);
        recv_string_expect_success (server_one, "C", 0);
        recv_string_expect_success (server_one, "stable", 0);
    }

    char buffer[255];
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN, zlink_recv (server_two, buffer, sizeof buffer, 0));

    test_context_socket_close_zero_linger (server_two);
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server_one);
}

int main ()
{
    setup_test_environment ();

#define RUN_SELECTED(test_)                                                        \
    do {                                                                           \
        if (should_run_router_handover_test (#test_))                               \
            RUN_TEST (test_);                                                       \
    } while (false)

    UNITY_BEGIN ();
    RUN_SELECTED (test_callback_dispatch_same_direction_reconnect_handover);
    RUN_SELECTED (test_callback_dispatch_cross_direction_duplicate_converges);
    RUN_SELECTED (test_repeated_cross_direction_reconnect_uses_current_endpoint);
    RUN_SELECTED (test_async_handshake_preserves_outgoing_direction);
#undef RUN_SELECTED
    return UNITY_END ();
}
