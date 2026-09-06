/* SPDX-License-Identifier: MPL-2.0 */

#include "zmp_peer_fixture.hpp"

SETUP_TEARDOWN_TESTCONTEXT

void test_raw_wire_ordinary_data_uses_eight_byte_kind_zero_header ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    fd_t raw = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, raw);
    set_recv_timeout (raw, 2000);
    TEST_ASSERT_TRUE (send_basic_handshake (raw, test_zmp_wire::socket_pair));

    static const unsigned char payload[] = {'d', 'a', 't', 'a'};
    TEST_ASSERT_EQUAL_INT (
      static_cast<int> (sizeof (payload)),
      zlink_send (server, payload, sizeof (payload), 0));

    unsigned char flags = 0;
    unsigned char kind = 0xff;
    uint64_t sequence = UINT64_MAX;
    std::vector<unsigned char> body;
    TEST_ASSERT_TRUE (read_next_application_frame (
      raw, &flags, &kind, &sequence, &body));
    TEST_ASSERT_EQUAL_HEX8 (0, flags);
    TEST_ASSERT_EQUAL_HEX8 (test_zmp_wire::zmp_kind_data, kind);
    TEST_ASSERT_EQUAL_UINT64 (0, sequence);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (payload), body.size ());
    TEST_ASSERT_EQUAL_MEMORY (payload, &body[0], body.size ());

    close (raw);
    test_context_socket_close_zero_linger (server);
}

void test_dealer_dealer_ready_uses_single_application_connection ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (dealer, endpoint, sizeof (endpoint));
    fd_t application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, application);
    set_recv_timeout (application, 2000);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      application, "raw-dealer-peer", 899, 1, 0,
      test_zmp_wire::socket_dealer));
    TEST_ASSERT_TRUE (wait_for_raw_ready (application));
    TEST_ASSERT_TRUE (wait_for_transport_pair_admission (dealer, 899, 1));

    static const unsigned char payload[] = {'d', 'd', '1'};
    TEST_ASSERT_TRUE (
      send_zmp_frame (application, 0, payload, sizeof (payload)));
    unsigned char received[sizeof (payload)];
    TEST_ASSERT_EQUAL_INT (
      static_cast<int> (sizeof (received)),
      zlink_recv (dealer, received, sizeof (received), 0));
    TEST_ASSERT_EQUAL_MEMORY (payload, received, sizeof (payload));

    close (application);
    test_context_socket_close_zero_linger (dealer);
}


void test_raw_wire_peer_weight_uses_application_control_only ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const int weight = 0x1234;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_dealer_option (
        dealer, ZLINK_DEALER_OPT_WEIGHT, &weight, sizeof (weight)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (dealer, endpoint, sizeof (endpoint));
    fd_t application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, application);
    set_recv_timeout (application, 2000);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      application, "raw-weight-router", 900, 1, 0,
      test_zmp_wire::socket_router));
    TEST_ASSERT_TRUE (wait_for_raw_ready (application));
    TEST_ASSERT_TRUE (
      wait_for_transport_pair_admission (dealer, 900, 1));

    unsigned char flags = 0;
    unsigned char kind = 0xff;
    uint64_t sequence = UINT64_MAX;
    std::vector<unsigned char> body;
    bool closed = false;
    TEST_ASSERT_TRUE (read_zmp_frame (
      application, flags, body, closed, &kind, &sequence));
    TEST_ASSERT_FALSE (closed);
    TEST_ASSERT_EQUAL_HEX8 (test_zmp_wire::zmp_flag_control, flags);
    TEST_ASSERT_EQUAL_HEX8 (test_zmp_wire::zmp_kind_data, kind);
    TEST_ASSERT_EQUAL_UINT64 (0, sequence);
    static const unsigned char expected[] = {
      'W', 'E', 'I', 'G', 'H', 'T', 0x00, 0x00, 0x12, 0x34};
    TEST_ASSERT_EQUAL_UINT64 (sizeof (expected), body.size ());
    TEST_ASSERT_EQUAL_MEMORY (expected, &body[0], sizeof (expected));

    // DEALER-ROUTER owns one physical Application lane. The READY metadata
    // above advertises Count=1, and WEIGHT shares that connection.
    close (application);
    test_context_socket_close_zero_linger (dealer);
}



void test_raw_wire_fragmented_base_extension_and_payload_delivers_once ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    fd_t raw = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, raw);
    TEST_ASSERT_TRUE (send_basic_handshake (raw, test_zmp_wire::socket_pair));

    static const unsigned char payload[] = {
      'f', 'r', 'a', 'g', 'm', 'e', 'n', 't', 'e', 'd'};
    const std::vector<unsigned char> frame = make_zmp_wire_frame (
      0, test_zmp_wire::zmp_kind_request, UINT64_C (0x1020304050607080),
      payload, sizeof (payload));

    //  Force each decoder stage to span transport reads: partial base header,
    //  partial sequence extension, then partial payload.
    static const size_t cuts[] = {3, test_zmp_wire::zmp_header_size + 3,
                                  test_zmp_wire::zmp_request_reply_header_size + 4,
                                  sizeof (payload)
                                    + test_zmp_wire::zmp_request_reply_header_size};
    size_t offset = 0;
    for (size_t i = 0; i != sizeof (cuts) / sizeof (cuts[0]); ++i) {
        TEST_ASSERT_TRUE (send_all (raw, &frame[offset], cuts[i] - offset));
        offset = cuts[i];
        if (i + 1 != sizeof (cuts) / sizeof (cuts[0]))
            msleep (10);
    }

    unsigned char received[32];
    int recv_size = -1;
    for (size_t attempt = 0; attempt != 400 && recv_size == -1; ++attempt) {
        errno = 0;
        recv_size = zlink_recv (
          server, received, sizeof (received), ZLINK_DONTWAIT);
        if (recv_size == -1) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
            msleep (5);
        }
    }
    TEST_ASSERT_EQUAL_INT (static_cast<int> (sizeof (payload)), recv_size);
    TEST_ASSERT_EQUAL_MEMORY (payload, received, sizeof (payload));
    assert_pair_has_no_raw_payload (server);

    close (raw);
    test_context_socket_close_zero_linger (server);
}

void test_raw_wire_sendsend_and_reqrep_match_multipart_frame_count ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (dealer, endpoint, sizeof (endpoint));
    fd_t application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, application);
    set_recv_timeout (application, 2000);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      application, "raw-router", 901, 1, 0, test_zmp_wire::socket_router));
    TEST_ASSERT_TRUE (wait_for_raw_ready (application));
    TEST_ASSERT_TRUE (
      wait_for_transport_pair_admission (dealer, 901, 1));

    static const unsigned char first_payload[] = {'f', 'i', 'r', 's', 't'};
    static const unsigned char second_payload[] = {'s', 'e', 'c', 'o', 'n', 'd'};

    zlink_msg_t ordinary[2];
    init_two_part_application_record (
      ordinary, first_payload, sizeof (first_payload), second_payload,
      sizeof (second_payload));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &ordinary[0], ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_MORE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &ordinary[1], ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64 (
      0, assert_raw_two_part_application_record (
           application, test_zmp_wire::zmp_kind_data, 0, first_payload,
           sizeof (first_payload), second_payload, sizeof (second_payload)));

    zlink_msg_t request[2];
    init_two_part_application_record (
      request, first_payload, sizeof (first_payload), second_payload,
      sizeof (second_payload));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &request[0], ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_MORE, 0, NULL, NULL));
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (dealer, NULL, &request[1], ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_FINAL, 5000, NULL, &completion_id));
    TEST_ASSERT_TRUE (completion_id != 0);

    const uint64_t request_sequence = assert_raw_two_part_application_record (
      application, test_zmp_wire::zmp_kind_request, 0, first_payload,
      sizeof (first_payload), second_payload, sizeof (second_payload));

    std::vector<unsigned char> raw_reply = make_zmp_wire_frame (
      test_zmp_wire::zmp_flag_more, test_zmp_wire::zmp_kind_reply, request_sequence,
      first_payload, sizeof (first_payload));
    append_wire_frame (&raw_reply, make_zmp_wire_frame (
      0, test_zmp_wire::zmp_kind_data, 0, second_payload, sizeof (second_payload)));
    TEST_ASSERT_TRUE (
      send_all (application, &raw_reply[0], raw_reply.size ()));
    zlink_completion_t request_completion =
      receive_request_completion_eventually (dealer);
    TEST_ASSERT_EQUAL_UINT64 (completion_id,
                              request_completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK,
                           request_completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (2, request_completion.reply_part_count);
    TEST_ASSERT_EQUAL_UINT64 (
      sizeof (first_payload),
      zlink_msg_size (&request_completion.reply_parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (
      first_payload, zlink_msg_data (&request_completion.reply_parts[0]),
      sizeof (first_payload));
    TEST_ASSERT_EQUAL_UINT64 (
      sizeof (second_payload),
      zlink_msg_size (&request_completion.reply_parts[1]));
    TEST_ASSERT_EQUAL_MEMORY (
      second_payload, zlink_msg_data (&request_completion.reply_parts[1]),
      sizeof (second_payload));
    zlink_completion_close (&request_completion);

    close (application);
    test_context_socket_close_zero_linger (dealer);
}

void test_raw_wire_public_router_reply_keeps_kind_and_sequence ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));
    fd_t application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, application);
    set_recv_timeout (application, 2000);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      application, "raw-dealer", 902, 1, 0, test_zmp_wire::socket_dealer));
    TEST_ASSERT_TRUE (wait_for_raw_ready (application));
    TEST_ASSERT_TRUE (
      wait_for_transport_pair_admission (router, 902, 1));

    static const unsigned char request_payload[] = {'a', 's', 'k'};
    const uint64_t request_sequence = UINT64_C (0x0102030405060708);
    TEST_ASSERT_TRUE (send_zmp_request_reply_frame (
      application, test_zmp_wire::zmp_kind_request, request_sequence,
      request_payload, sizeof (request_payload)));

    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t reply_token = 0;
    zlink_msg_t request;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&request));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router, &source_rid, &reply_token, &request,
                              &has_more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    // The public value is a socket-owned opaque reply token. The original
    // wire sequence is restored only when the reply is encoded below.
    TEST_ASSERT_TRUE (reply_token != 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (request_payload),
                              zlink_msg_size (&request));
    TEST_ASSERT_EQUAL_MEMORY (request_payload, zlink_msg_data (&request),
                              sizeof (request_payload));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&request));

    static const unsigned char reply_payload[] = {'r', 'e', 'p', 'l', 'y'};
    zlink_msg_t reply;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&reply, sizeof (reply_payload)));
    memcpy (zlink_msg_data (&reply), reply_payload, sizeof (reply_payload));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, source_rid, reply_token, &reply,
                        ZLINK_PART_FINAL));

    unsigned char flags = 0;
    unsigned char kind = 0;
    uint64_t sequence = 0;
    std::vector<unsigned char> body;
    TEST_ASSERT_TRUE (read_next_application_frame (
      application, &flags, &kind, &sequence, &body));
    TEST_ASSERT_EQUAL_HEX8 (0, flags);
    TEST_ASSERT_EQUAL_HEX8 (test_zmp_wire::zmp_kind_reply, kind);
    TEST_ASSERT_EQUAL_UINT64 (request_sequence, sequence);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (reply_payload), body.size ());
    TEST_ASSERT_EQUAL_MEMORY (reply_payload, &body[0], body.size ());

    close (application);
    test_context_socket_close_zero_linger (router);
}

void test_raw_wire_rejects_incomplete_and_malformed_frames_without_payload ()
{
    std::vector<std::vector<unsigned char> > invalid_streams;

    //  EOF in the base header.
    invalid_streams.push_back (std::vector<unsigned char> ());
    invalid_streams.back ().push_back (test_zmp_wire::zmp_magic);
    invalid_streams.back ().push_back (test_zmp_wire::zmp_version);
    invalid_streams.back ().push_back (0);

    //  EOF in the request sequence extension.
    invalid_streams.push_back (make_zmp_wire_frame (
      0, test_zmp_wire::zmp_kind_request, 1, NULL, 0));
    invalid_streams.back ().resize (test_zmp_wire::zmp_header_size + 3);

    //  EOF in the declared payload.
    static const unsigned char body[] = {'b', 'o', 'd', 'y'};
    invalid_streams.push_back (make_zmp_wire_frame (
      0, test_zmp_wire::zmp_kind_data, 0, body, sizeof (body)));
    invalid_streams.back ().resize (test_zmp_wire::zmp_header_size + 2);

    //  EOF after MORE is a partial logical multipart, even though its first
    //  wire frame is complete.
    invalid_streams.push_back (make_zmp_wire_frame (
      test_zmp_wire::zmp_flag_more, test_zmp_wire::zmp_kind_data, 0, body,
      sizeof (body)));

    invalid_streams.push_back (make_zmp_wire_frame (
      0, 0x7f, 0, NULL, 0));
    invalid_streams.push_back (make_zmp_wire_frame (
      0, test_zmp_wire::zmp_kind_request, 0, NULL, 0));
    static const unsigned char request_special_flags[] = {
      test_zmp_wire::zmp_flag_control, test_zmp_wire::zmp_flag_identity,
      test_zmp_wire::zmp_flag_subscribe, test_zmp_wire::zmp_flag_cancel};
    for (size_t i = 0;
         i != sizeof (request_special_flags)
                / sizeof (request_special_flags[0]);
         ++i) {
        invalid_streams.push_back (make_zmp_wire_frame (
          request_special_flags[i], test_zmp_wire::zmp_kind_request, 1, NULL, 0));
    }
    static const unsigned char forbidden_flags[] = {
      0x20, 0x40, 0x80,
      test_zmp_wire::zmp_flag_control | test_zmp_wire::zmp_flag_identity,
      test_zmp_wire::zmp_flag_control | test_zmp_wire::zmp_flag_more,
      test_zmp_wire::zmp_flag_subscribe | test_zmp_wire::zmp_flag_cancel,
      test_zmp_wire::zmp_flag_subscribe | test_zmp_wire::zmp_flag_more,
      test_zmp_wire::zmp_flag_cancel | test_zmp_wire::zmp_flag_identity};
    for (size_t i = 0;
         i != sizeof (forbidden_flags) / sizeof (forbidden_flags[0]);
         ++i) {
        invalid_streams.push_back (make_zmp_wire_frame (
          forbidden_flags[i], test_zmp_wire::zmp_kind_data, 0, NULL, 0));
    }

    std::vector<unsigned char> mid_multipart_kind = make_zmp_wire_frame (
      test_zmp_wire::zmp_flag_more, test_zmp_wire::zmp_kind_data, 0, body, sizeof (body));
    append_wire_frame (&mid_multipart_kind, make_zmp_wire_frame (
      0, test_zmp_wire::zmp_kind_request, 2, body, sizeof (body)));
    invalid_streams.push_back (mid_multipart_kind);

    static const unsigned char mid_multipart_special_flags[] = {
      test_zmp_wire::zmp_flag_identity, test_zmp_wire::zmp_flag_control,
      test_zmp_wire::zmp_flag_subscribe, test_zmp_wire::zmp_flag_cancel};
    for (size_t i = 0;
         i != sizeof (mid_multipart_special_flags)
                / sizeof (mid_multipart_special_flags[0]);
         ++i) {
        std::vector<unsigned char> mid_multipart_special =
          make_zmp_wire_frame (test_zmp_wire::zmp_flag_more,
                               test_zmp_wire::zmp_kind_data, 0, body,
                               sizeof (body));
        append_wire_frame (&mid_multipart_special, make_zmp_wire_frame (
          mid_multipart_special_flags[i], test_zmp_wire::zmp_kind_data, 0, NULL,
          0));
        invalid_streams.push_back (mid_multipart_special);
    }

    for (size_t i = 0; i != invalid_streams.size (); ++i)
        assert_raw_stream_rejected (invalid_streams[i]);
}

void test_network_incomplete_multipart_stops_at_receiver_max_message_size ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    const int64_t max_message_size = 10;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server, ZLINK_OPT_MAXMSGSIZE, &max_message_size, sizeof (max_message_size)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    fd_t raw = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, raw);
    TEST_ASSERT_TRUE (send_basic_handshake (raw, test_zmp_wire::socket_pair));
    TEST_ASSERT_TRUE (wait_for_raw_ready (raw));

    const unsigned char payload[6] = {'b', 'o', 'u', 'n', 'd', 's'};
    TEST_ASSERT_TRUE (send_zmp_frame (raw, test_zmp_wire::zmp_flag_more, payload, sizeof (payload)));

    // The first part is individually valid. Prove that READY completed and
    // that only the aggregate 6 + 6 boundary rejects the next part.
    set_recv_timeout (raw, 100);
    unsigned char flags = 0;
    std::vector<unsigned char> body;
    bool closed = false;
    TEST_ASSERT_FALSE (read_zmp_frame (raw, flags, body, closed));
    TEST_ASSERT_FALSE (closed);

    TEST_ASSERT_TRUE (send_zmp_frame (raw, test_zmp_wire::zmp_flag_more, payload, sizeof (payload)));
    TEST_ASSERT_TRUE (wait_for_raw_close (raw, true));
    assert_pair_has_no_raw_payload (server);

    close (raw);
    test_context_socket_close_zero_linger (server);
}

void test_tcp_hidden_identity_releases_decoder_reservation ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    const uint64_t payload_size = 4;
    const uint64_t frame_bytes = payload_size + sizeof (zlink_msg_t);
    const int recv_timeout = 2000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_RCVHWM, &frame_bytes,
                        sizeof (frame_bytes)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_RCVTIMEO, &recv_timeout,
                        sizeof (recv_timeout)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    fd_t raw = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, raw);
    TEST_ASSERT_TRUE (send_basic_handshake (raw, test_zmp_wire::socket_pair));

    const unsigned char identity[payload_size] = {'r', 'i', 'd', '0'};
    const unsigned char payload[payload_size] = {'d', 'a', 't', 'a'};
    TEST_ASSERT_TRUE (send_zmp_frame (
      raw, test_zmp_wire::zmp_flag_identity | test_zmp_wire::zmp_flag_more,
      identity, sizeof (identity)));
    TEST_ASSERT_TRUE (send_zmp_frame (raw, 0, payload, sizeof (payload)));

    unsigned char received[payload_size];
    TEST_ASSERT_EQUAL_INT (
      static_cast<int> (payload_size),
      zlink_recv (server, received, sizeof (received), 0));
    TEST_ASSERT_EQUAL_MEMORY (payload, received, sizeof (payload));
    TEST_ASSERT_TRUE (wait_for_current_accounted_bytes (0));
    TEST_ASSERT_EQUAL_UINT64 (
      0, read_hwm_snapshot ().deferred_origin_credit_bytes);

    close (raw);
    test_context_socket_close_zero_linger (server);
}

void test_tcp_decoder_hwm_isolated_by_origin_connection ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    const uint64_t payload_size = 16;
    const uint64_t frame_bytes = payload_size + sizeof (zlink_msg_t);
    const int recv_timeout = 2000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_RCVHWM, &frame_bytes,
                        sizeof (frame_bytes)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_RCVTIMEO, &recv_timeout,
                        sizeof (recv_timeout)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    const uint64_t initial_direction_count =
      read_hwm_snapshot ().active_directional_queue_count;
    fd_t raw_a_application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    fd_t raw_b_application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, raw_a_application);
    TEST_ASSERT_NOT_EQUAL (retired_fd, raw_b_application);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      raw_a_application, "origin-a", 101, 1, 0,
      test_zmp_wire::socket_dealer));
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      raw_b_application, "origin-b", 102, 1, 0,
      test_zmp_wire::socket_dealer));

    // Each DEALER-ROUTER connection is one physical Application pipe backed
    // by two directional queues. Wait until both origins are attached before
    // measuring independent decoder admission.
    TEST_ASSERT_TRUE (wait_for_active_directional_queue_count (
      initial_direction_count + 4));

    // ROUTER pipe attachment carries an internal routing-id envelope through
    // the application queue. Pump it before testing decoder-frame admission.
    zlink_msg_t pending;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&pending));
    const zlink_routing_id_t *pending_source_rid = NULL;
    uint64_t pending_request_seq = 0;
    zlink_part_flag_t pending_has_more = ZLINK_PART_FINAL;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_router_recv_part (
        server, &pending_source_rid, &pending_request_seq, &pending,
        &pending_has_more,
        static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&pending));
    TEST_ASSERT_TRUE (wait_for_current_accounted_bytes (0));

    unsigned char a1[payload_size];
    unsigned char a2[payload_size];
    unsigned char b1[payload_size];
    memset (a1, '1', sizeof (a1));
    memset (a2, '2', sizeof (a2));
    memset (b1, 'b', sizeof (b1));
    TEST_ASSERT_TRUE (
      send_zmp_frame (raw_a_application, 0, a1, sizeof (a1)));
    TEST_ASSERT_TRUE (
      send_zmp_frame (raw_a_application, 0, a2, sizeof (a2)));
    TEST_ASSERT_TRUE (
      send_zmp_frame (raw_b_application, 0, b1, sizeof (b1)));

    // A's second header is stopped at its own full physical direction while
    // B independently reaches Core. No socket-wide or context-wide gate may
    // turn the expected two admitted frames into one.
    TEST_ASSERT_TRUE (wait_for_current_accounted_bytes (frame_bytes * 2));

    bool saw_a1 = false;
    bool saw_a2 = false;
    bool saw_b1 = false;
    for (size_t i = 0; i != 3; ++i) {
        zlink_msg_t msg;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
        const zlink_routing_id_t *source_rid = NULL;
        uint64_t request_seq = 0;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_router_recv_part (
            server, &source_rid, &request_seq, &msg, &has_more,
            static_cast<zlink_recv_flags_t> (0)));
        TEST_ASSERT_EQUAL_UINT64 (payload_size, zlink_msg_size (&msg));
        const unsigned char marker =
          *static_cast<unsigned char *> (zlink_msg_data (&msg));
        saw_a1 = saw_a1 || marker == '1';
        saw_a2 = saw_a2 || marker == '2';
        saw_b1 = saw_b1 || marker == 'b';
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));
    }
    TEST_ASSERT_TRUE (saw_a1);
    TEST_ASSERT_TRUE (saw_a2);
    TEST_ASSERT_TRUE (saw_b1);

    close (raw_b_application);
    close (raw_a_application);
    test_context_socket_close_zero_linger (server);
}

void test_zmp_error_invalid_hello ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    fd_t raw = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, raw);
    set_recv_timeout (raw, 2000);

    unsigned char body[3];
    body[0] = test_zmp_wire::zmp_control_hello;
    body[1] = test_zmp_wire::socket_pair;
    body[2] = 0;

    unsigned char header[test_zmp_wire::zmp_header_size];
    header[0] = 0x00;
    header[1] = test_zmp_wire::zmp_version;
    header[2] = test_zmp_wire::zmp_flag_control;
    header[3] = 0;
    test_zmp_wire::put_uint32 (header + 4, sizeof (body));
    TEST_ASSERT_TRUE (send_all (raw, header, sizeof (header)));
    TEST_ASSERT_TRUE (send_all (raw, body, sizeof (body)));

    bool closed = false;
    bool saw_error = false;
    for (int i = 0; i < 4 && !saw_error && !closed; ++i) {
        unsigned char flags = 0;
        std::vector<unsigned char> body;
        if (!read_zmp_frame (raw, flags, body, closed))
            continue;
        if ((flags & test_zmp_wire::zmp_flag_control) && !body.empty ()
            && body[0] == test_zmp_wire::zmp_control_error) {
            TEST_ASSERT_TRUE (body.size () >= 3);
            TEST_ASSERT_EQUAL_UINT8 (test_zmp_wire::zmp_error_invalid_magic, body[1]);
            saw_error = true;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE (saw_error, "expected ERROR frame");

    close (raw);
    test_context_socket_close (server);
}

void test_paired_ready_peer_identity_mismatch_is_not_attached ()
{
    assert_paired_handshake_not_dispatchable (
      "raw-paired-peer-a", 41, 7, 0, "raw-paired-peer-b", 41, 7, 1);
}

void test_paired_ready_hello_identity_mismatch_is_closed ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    fd_t raw = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, raw);
    set_recv_timeout (raw, 2000);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      raw, "hello-rid", 43, 1, 0, ZLINK_SOCKET_DEALER, "ready-rid"));
    TEST_ASSERT_TRUE (wait_for_raw_close (raw, true));

    close (raw);
    test_context_socket_close_zero_linger (server);
}

void test_paired_ready_requires_valid_lane_count ()
{
    struct invalid_lane_count_case_t
    {
        bool include;
        unsigned char lane;
        unsigned char bytes[2];
        size_t size;
    };
    const invalid_lane_count_case_t cases[] = {
      {false, 0, {0, 0}, 0}, // missing
      {true, 0, {0, 0}, 0},  // empty
      {true, 0, {0, 1}, 2},  // wrong property width
      {true, 0, {0, 0}, 1},  // zero count
      {true, 0, {3, 0}, 1},  // unsupported count
      {true, 0, {2, 0}, 1},  // D-R symmetric result is count 1
      {true, 1, {1, 0}, 1}   // lane must be strictly less than count
    };

    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    for (size_t i = 0; i != sizeof (cases) / sizeof (cases[0]); ++i) {
        char routing_id[32];
        snprintf (routing_id, sizeof (routing_id), "bad-lane-count-%zu", i);
        fd_t raw = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
        TEST_ASSERT_NOT_EQUAL (retired_fd, raw);
        set_recv_timeout (raw, 100);
        TEST_ASSERT_TRUE (send_paired_handshake_with_lane_count_property (
          raw, routing_id, 1300 + i, 1, cases[i].lane,
          ZLINK_SOCKET_DEALER, NULL, cases[i].bytes, cases[i].size,
          cases[i].include));
        TEST_ASSERT_TRUE (wait_for_raw_close (raw, true));
        close (raw);
    }

    test_context_socket_close_zero_linger (server);
}

void test_paired_ready_duplicate_lane_is_not_attached ()
{
    assert_paired_handshake_not_dispatchable (
      "raw-paired-peer", 41, 7, 0, "raw-paired-peer", 41, 7, 0);
}

void assert_incomplete_pair_lane_hits_fence (unsigned char lone_lane_,
                                             uint64_t alias_base_)
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    const int handshake_ivl = 50;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server, ZLINK_OPT_HANDSHAKE_IVL, &handshake_ivl,
      sizeof (handshake_ivl)));
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const char peer_rid[] = "raw-pair-fence-peer";
    fd_t lone = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, lone);
    set_recv_timeout (lone, 2000);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      lone, peer_rid, alias_base_, 1, lone_lane_, ZLINK_SOCKET_ROUTER,
      NULL, 2));
    TEST_ASSERT_TRUE (wait_for_raw_ready (lone));
    TEST_ASSERT_TRUE (wait_for_raw_close (lone, true));
    close (lone);

    // The expired lane must release its accepted identity. A fresh pair with
    // the same logical RID then receives a new generation-local identity and
    // cannot be killed by the stale fence callback.
    fd_t application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    fd_t completion = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, application);
    TEST_ASSERT_NOT_EQUAL (retired_fd, completion);
    set_recv_timeout (application, 2000);
    set_recv_timeout (completion, 2000);
    const uint64_t fresh_alias = alias_base_ + 1;
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      application, peer_rid, fresh_alias, 2, 0, ZLINK_SOCKET_ROUTER,
      NULL, 2));
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      completion, peer_rid, fresh_alias, 2, 1, ZLINK_SOCKET_ROUTER,
      NULL, 2));
    TEST_ASSERT_TRUE (wait_for_raw_ready (application));
    TEST_ASSERT_TRUE (wait_for_raw_ready (completion));
    TEST_ASSERT_TRUE (
      wait_for_transport_pair_admission (server, fresh_alias, 2));

    // Let the rearmed lane timers expire and prove admitted pair membership
    // turns those callbacks into no-ops.
    msleep (handshake_ivl * 2);
    TEST_ASSERT_TRUE (
      wait_for_transport_pair_admission (server, fresh_alias, 2));

    close (completion);
    close (application);
    test_context_socket_close_zero_linger (server);
}

void test_paired_incomplete_lane_fence_timeout_and_fresh_pair ()
{
    assert_incomplete_pair_lane_hits_fence (0, 1200);
    assert_incomplete_pair_lane_hits_fence (1, 1210);
}


void test_completion_lane_rejects_data_and_request_kinds_without_completing_them ()
{
    const unsigned char invalid_kinds[] = {test_zmp_wire::zmp_kind_data,
                                           test_zmp_wire::zmp_kind_request};
    for (size_t kind_index = 0;
         kind_index < sizeof (invalid_kinds) / sizeof (invalid_kinds[0]);
         ++kind_index) {
        // Only ROUTER-ROUTER owns a physical Completion lane in the new
        // topology. Keep its kind restriction covered independently of the
        // DEALER-ROUTER single-connection head-kind demultiplexer.
        void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
        char endpoint[MAX_SOCKET_STRING];
        bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
        fd_t application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
        fd_t completion = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
        TEST_ASSERT_NOT_EQUAL (retired_fd, application);
        TEST_ASSERT_NOT_EQUAL (retired_fd, completion);
        set_recv_timeout (application, 2000);
        set_recv_timeout (completion, 2000);
        TEST_ASSERT_TRUE (send_paired_dealer_handshake (
          application, "invalid-completion-kind", 950 + kind_index, 1, 0,
          test_zmp_wire::socket_router, NULL, 2));
        TEST_ASSERT_TRUE (send_paired_dealer_handshake (
          completion, "invalid-completion-kind", 950 + kind_index, 1, 1,
          test_zmp_wire::socket_router, NULL, 2));
        TEST_ASSERT_TRUE (wait_for_raw_ready (application));
        TEST_ASSERT_TRUE (wait_for_raw_ready (completion));
        TEST_ASSERT_TRUE (wait_for_transport_pair_admission (
          server, 950 + kind_index, 1));

        zlink_msg_t request;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&request, 1));
        *static_cast<unsigned char *> (zlink_msg_data (&request)) = 'q';
        zlink_completion_id_t completion_id = 0;
        const zlink_routing_id_t target_rid =
          make_text_routing_id ("invalid-completion-kind");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_request_part (server, &target_rid, &request,
                              ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 250,
                              NULL, &completion_id));
        TEST_ASSERT_TRUE (completion_id != 0);
        const uint64_t request_sequence =
          read_raw_request_sequence (application);

        static const unsigned char invalid_payload[] = {'b', 'a', 'd'};
        if (invalid_kinds[kind_index] == test_zmp_wire::zmp_kind_data) {
            TEST_ASSERT_TRUE (send_zmp_frame (
              completion, 0, invalid_payload, sizeof (invalid_payload)));
        } else {
            TEST_ASSERT_TRUE (send_zmp_request_reply_frame (
              completion, test_zmp_wire::zmp_kind_request, request_sequence,
              invalid_payload, sizeof (invalid_payload)));
        }

        zlink_completion_t request_completion =
          receive_request_completion_eventually (server);
        TEST_ASSERT_EQUAL_UINT64 (completion_id,
                                  request_completion.completion_id);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_NOT_CONNECTED,
                               request_completion.request_result);
        zlink_completion_close (&request_completion);
        msleep (SETTLE_TIME);
        assert_no_completion (server);
        TEST_ASSERT_TRUE (wait_for_raw_close (completion));

        close (completion);
        close (application);
        test_context_socket_close_zero_linger (server);
    }
}


void test_error_reply_completion_decodes_errno_and_hides_invalid_payloads ()
{
    unsigned char valid_errno[4];
    test_zmp_wire::put_uint32 (valid_errno, EACCES);
    run_raw_error_reply_case (980, valid_errno, sizeof (valid_errno), true,
                              ZLINK_REQUEST_REJECTED, 2);

    // An empty errno frame, a wrong-sized errno frame, and a four-byte zero
    // errno are semantic protocol errors. Even with valid trailing frames,
    // none of that record is exposed as completion payload.
    run_raw_error_reply_case (981, NULL, 0, true,
                              ZLINK_REQUEST_PROTOCOL_ERROR, 0);
    static const unsigned char short_errno[] = {0, 0, 1};
    run_raw_error_reply_case (982, short_errno, sizeof (short_errno), true,
                              ZLINK_REQUEST_PROTOCOL_ERROR, 0);
    static const unsigned char zero_errno[] = {0, 0, 0, 0};
    run_raw_error_reply_case (983, zero_errno, sizeof (zero_errno), true,
                              ZLINK_REQUEST_PROTOCOL_ERROR, 0);
}


int main (void)
{
    UNITY_BEGIN ();

    setup_test_environment ();

#define RUN_ZMP_METADATA_TEST(test_name_)                                      \
    do {                                                                       \
        if (should_run_zmp_metadata_test (#test_name_))                        \
            RUN_TEST (test_name_);                                             \
    } while (false)

    RUN_ZMP_METADATA_TEST (
      test_raw_wire_ordinary_data_uses_eight_byte_kind_zero_header);
    RUN_ZMP_METADATA_TEST (
      test_dealer_dealer_ready_uses_single_application_connection);
    RUN_ZMP_METADATA_TEST (test_raw_wire_peer_weight_uses_application_control_only);
    RUN_ZMP_METADATA_TEST (
      test_raw_wire_fragmented_base_extension_and_payload_delivers_once);
    RUN_ZMP_METADATA_TEST (
      test_raw_wire_sendsend_and_reqrep_match_multipart_frame_count);
    RUN_ZMP_METADATA_TEST (test_raw_wire_public_router_reply_keeps_kind_and_sequence);
    RUN_ZMP_METADATA_TEST (
      test_raw_wire_rejects_incomplete_and_malformed_frames_without_payload);
    RUN_ZMP_METADATA_TEST (test_zmp_error_invalid_hello);
    RUN_ZMP_METADATA_TEST (
      test_network_incomplete_multipart_stops_at_receiver_max_message_size);
    RUN_ZMP_METADATA_TEST (test_tcp_hidden_identity_releases_decoder_reservation);
    RUN_ZMP_METADATA_TEST (test_tcp_decoder_hwm_isolated_by_origin_connection);
    RUN_ZMP_METADATA_TEST (test_paired_ready_hello_identity_mismatch_is_closed);
    RUN_ZMP_METADATA_TEST (test_paired_ready_requires_valid_lane_count);
    RUN_ZMP_METADATA_TEST (
      test_paired_ready_peer_identity_mismatch_is_not_attached);
    RUN_ZMP_METADATA_TEST (test_paired_ready_duplicate_lane_is_not_attached);
    RUN_ZMP_METADATA_TEST (
      test_paired_incomplete_lane_fence_timeout_and_fresh_pair);
    RUN_ZMP_METADATA_TEST (
      test_completion_lane_rejects_data_and_request_kinds_without_completing_them);
    RUN_ZMP_METADATA_TEST (
      test_error_reply_completion_decodes_errno_and_hides_invalid_payloads);

#undef RUN_ZMP_METADATA_TEST

    return UNITY_END ();
}
