/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include "protocol/wire.hpp"
#include "protocol/zmp_metadata.hpp"
#include "protocol/zmp_protocol.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"

#include <atomic>
#include <errno.h>
#include <string.h>
#include <vector>

#ifndef ZLINK_HAVE_WINDOWS
#include <sys/time.h>
#endif

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
void set_recv_timeout (fd_t fd_, int timeout_ms_)
{
#if defined ZLINK_HAVE_WINDOWS
    DWORD timeout = static_cast<DWORD> (timeout_ms_);
    TEST_ASSERT_SUCCESS_RAW_ERRNO (setsockopt (
      fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *> (&timeout), sizeof (timeout)));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms_ / 1000;
    tv.tv_usec = (timeout_ms_ % 1000) * 1000;
    TEST_ASSERT_SUCCESS_RAW_ERRNO (setsockopt (fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv)));
#endif
}

enum recv_status_t
{
    recv_ok = 0,
    recv_closed,
    recv_timeout,
    recv_error
};

bool send_all (fd_t fd_, const unsigned char *buf_, size_t size_)
{
    size_t offset = 0;
    while (offset < size_) {
#if defined ZLINK_HAVE_WINDOWS
        const int rc = send (fd_, reinterpret_cast<const char *> (buf_ + offset),
                             static_cast<int> (size_ - offset), 0);
        if (rc <= 0)
            return false;
#else
        const ssize_t rc = send (fd_, buf_ + offset, size_ - offset, MSG_NOSIGNAL);
        if (rc <= 0)
            return false;
#endif
        offset += static_cast<size_t> (rc);
    }
    return true;
}

recv_status_t recv_all (fd_t fd_, unsigned char *buf_, size_t size_)
{
    size_t offset = 0;
    while (offset < size_) {
#if defined ZLINK_HAVE_WINDOWS
        const int rc = recv (fd_, reinterpret_cast<char *> (buf_ + offset),
                             static_cast<int> (size_ - offset), 0);
        if (rc == 0)
            return recv_closed;
        if (rc < 0) {
            const int err = WSAGetLastError ();
            if (err == WSAETIMEDOUT)
                return recv_timeout;
            return recv_error;
        }
#else
        const ssize_t rc = recv (fd_, buf_ + offset, size_ - offset, MSG_NOSIGNAL);
        if (rc == 0)
            return recv_closed;
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return recv_timeout;
            return recv_error;
        }
#endif
        offset += static_cast<size_t> (rc);
    }
    return recv_ok;
}

bool send_zmp_frame (fd_t fd_, unsigned char flags_, const unsigned char *body_, size_t body_len_)
{
    unsigned char header[zlink::zmp_header_size];
    header[0] = zlink::zmp_magic;
    header[1] = zlink::zmp_version;
    header[2] = flags_;
    header[3] = 0;
    zlink::put_uint32 (header + 4, static_cast<uint32_t> (body_len_));

    if (!send_all (fd_, header, sizeof (header)))
        return false;
    if (body_len_ > 0)
        return send_all (fd_, body_, body_len_);
    return true;
}

bool send_zmp_control (fd_t fd_, const unsigned char *body_, size_t body_len_)
{
    return send_zmp_frame (fd_, zlink::zmp_flag_control, body_, body_len_);
}

bool send_basic_handshake (fd_t fd_, int socket_type_)
{
    unsigned char hello[3];
    hello[0] = zlink::zmp_control_hello;
    hello[1] = static_cast<unsigned char> (socket_type_);
    hello[2] = 0;
    if (!send_zmp_control (fd_, hello, sizeof (hello)))
        return false;

    std::vector<unsigned char> ready;
    ready.push_back (zlink::zmp_control_ready);
    const char *socket_type_name = socket_type_ == ZLINK_SOCKET_PAIR ? "PAIR" : "DEALER";
    zlink::zmp_metadata::append_property (
      ready, "Socket-Type", socket_type_name, strlen (socket_type_name));
    return send_zmp_control (fd_, &ready[0], ready.size ());
}

bool send_paired_dealer_handshake (fd_t fd_,
                                   const char *routing_id_,
                                   uint64_t pair_id_,
                                   uint64_t generation_,
                                   unsigned char lane_,
                                   int socket_type_ = ZLINK_SOCKET_DEALER)
{
    unsigned char hello[3 + 255];
    const size_t routing_id_size = strlen (routing_id_);
    if (routing_id_size > 255)
        return false;
    hello[0] = zlink::zmp_control_hello;
    hello[1] = static_cast<unsigned char> (socket_type_);
    hello[2] = static_cast<unsigned char> (routing_id_size);
    memcpy (hello + 3, routing_id_, routing_id_size);
    if (!send_zmp_control (fd_, hello, 3 + routing_id_size))
        return false;

    std::vector<unsigned char> ready;
    ready.push_back (zlink::zmp_control_ready);
    const char *socket_type_name =
      socket_type_ == ZLINK_SOCKET_ROUTER ? "ROUTER" : "DEALER";
    zlink::zmp_metadata::append_property (
      ready, "Socket-Type", socket_type_name, strlen (socket_type_name));
    zlink::zmp_metadata::append_property (
      ready, "Routing-Id", routing_id_, routing_id_size);

    unsigned char pair_id[8];
    unsigned char generation[8];
    zlink::put_uint64 (pair_id, pair_id_);
    zlink::put_uint64 (generation, generation_);
    zlink::zmp_metadata::append_property (
      ready, "Zlink-Pair-Id", pair_id, sizeof (pair_id));
    zlink::zmp_metadata::append_property (
      ready, "Zlink-Pair-Generation", generation, sizeof (generation));
    zlink::zmp_metadata::append_property (
      ready, "Zlink-Lane", &lane_, sizeof (lane_));
    return send_zmp_control (fd_, &ready[0], ready.size ());
}

bool read_zmp_frame (fd_t fd_,
                     unsigned char &flags_,
                     std::vector<unsigned char> &body_,
                     bool &closed_)
{
    unsigned char header[zlink::zmp_header_size];
    const recv_status_t header_rc = recv_all (fd_, header, sizeof (header));
    if (header_rc == recv_closed) {
        closed_ = true;
        return false;
    }
    if (header_rc != recv_ok)
        return false;

    const uint32_t body_len = zlink::get_uint32 (header + 4);
    if (body_len > 1024)
        return false;

    body_.assign (body_len, 0);
    if (body_len > 0) {
        const recv_status_t body_rc = recv_all (fd_, &body_[0], body_len);
        if (body_rc == recv_closed) {
            closed_ = true;
            return false;
        }
        if (body_rc != recv_ok)
            return false;
    }

    flags_ = header[2];
    return true;
}

bool wait_for_raw_close (fd_t fd_)
{
    set_recv_timeout (fd_, 100);
    for (size_t attempt = 0; attempt < 30; ++attempt) {
        unsigned char flags = 0;
        std::vector<unsigned char> body;
        bool closed = false;
        (void) read_zmp_frame (fd_, flags, body, closed);
        if (closed)
            return true;
    }
    return false;
}

uint64_t read_raw_request_sequence (fd_t fd_)
{
    size_t data_frame_index = 0;
    for (size_t attempt = 0; attempt < 16; ++attempt) {
        unsigned char flags = 0;
        std::vector<unsigned char> body;
        bool closed = false;
        TEST_ASSERT_TRUE (read_zmp_frame (fd_, flags, body, closed));
        TEST_ASSERT_FALSE (closed);
        if ((flags & zlink::zmp_flag_control) != 0)
            continue;
        if (data_frame_index++ == 3) {
            TEST_ASSERT_EQUAL_UINT64 (8, body.size ());
            return zlink::request_reply::decode_u64_be (&body[0]);
        }
    }
    TEST_FAIL_MESSAGE ("request sequence frame was not received");
    return 0;
}

bool send_raw_reply (fd_t fd_, uint64_t request_seq_)
{
    const unsigned char protocol = zlink::request_reply::protocol_id;
    const unsigned char version = zlink::request_reply::version;
    const unsigned char type = zlink::request_reply::reply_type;
    unsigned char sequence[8];
    zlink::request_reply::encode_u64_be (request_seq_, sequence);
    return send_zmp_frame (fd_, zlink::zmp_flag_more, &protocol, 1)
           && send_zmp_frame (fd_, zlink::zmp_flag_more, &version, 1)
           && send_zmp_frame (fd_, zlink::zmp_flag_more, &type, 1)
           && send_zmp_frame (fd_, 0, sequence, sizeof (sequence));
}

struct reply_probe_t
{
    reply_probe_t () : calls (0), result (ZLINK_REQUEST_INTERNAL_ERROR) {}
    std::atomic<int> calls;
    zlink_request_result_t result;
};

void capture_reply (zlink_request_result_t result_,
                    zlink_msg_t *parts_,
                    size_t part_count_,
                    void *userdata_)
{
    reply_probe_t *probe = static_cast<reply_probe_t *> (userdata_);
    probe->result = result_;
    for (size_t i = 0; i < part_count_; ++i)
        zlink_msg_close (&parts_[i]);
    probe->calls.fetch_add (1, std::memory_order_release);
}

bool wait_for_calls (reply_probe_t *probe_, int expected_)
{
    for (int i = 0; i < 200; ++i) {
        if (probe_->calls.load (std::memory_order_acquire) >= expected_)
            return true;
        msleep (5);
    }
    return false;
}

void assert_paired_handshake_not_dispatchable (
  const char *application_routing_id_,
  uint64_t application_pair_id_,
  uint64_t application_generation_,
  unsigned char application_lane_,
  const char *completion_routing_id_,
  uint64_t completion_pair_id_,
  uint64_t completion_generation_,
  unsigned char completion_lane_)
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    fd_t application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    fd_t completion = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, application);
    TEST_ASSERT_NOT_EQUAL (retired_fd, completion);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      application, application_routing_id_, application_pair_id_,
      application_generation_, application_lane_));
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      completion, completion_routing_id_, completion_pair_id_,
      completion_generation_, completion_lane_));

    // Pair admission must reject mismatched handshake metadata before either
    // connection needs application traffic to populate mutable routing state.
    TEST_ASSERT_TRUE (wait_for_raw_close (application));
    TEST_ASSERT_TRUE (wait_for_raw_close (completion));

    zlink_msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_router_recv_part (
        server, &source_rid, &request_seq, &msg, &has_more,
        static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT)));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));

    close (completion);
    close (application);
    test_context_socket_close_zero_linger (server);
}
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
    TEST_ASSERT_TRUE (send_basic_handshake (raw, ZLINK_SOCKET_PAIR));

    const unsigned char payload[6] = {'b', 'o', 'u', 'n', 'd', 's'};
    TEST_ASSERT_TRUE (send_zmp_frame (raw, zlink::zmp_flag_more, payload, sizeof (payload)));
    TEST_ASSERT_TRUE (send_zmp_frame (raw, zlink::zmp_flag_more, payload, sizeof (payload)));
    TEST_ASSERT_TRUE (wait_for_raw_close (raw));

    close (raw);
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
    body[0] = zlink::zmp_control_hello;
    body[1] = ZLINK_SOCKET_PAIR;
    body[2] = 0;

    unsigned char header[zlink::zmp_header_size];
    header[0] = 0x00;
    header[1] = zlink::zmp_version;
    header[2] = zlink::zmp_flag_control;
    header[3] = 0;
    zlink::put_uint32 (header + 4, sizeof (body));
    TEST_ASSERT_TRUE (send_all (raw, header, sizeof (header)));
    TEST_ASSERT_TRUE (send_all (raw, body, sizeof (body)));

    bool closed = false;
    bool saw_error = false;
    for (int i = 0; i < 4 && !saw_error && !closed; ++i) {
        unsigned char flags = 0;
        std::vector<unsigned char> body;
        if (!read_zmp_frame (raw, flags, body, closed))
            continue;
        if ((flags & zlink::zmp_flag_control) && !body.empty ()
            && body[0] == zlink::zmp_control_error) {
            TEST_ASSERT_TRUE (body.size () >= 3);
            TEST_ASSERT_EQUAL_UINT8 (zlink::zmp_error_invalid_magic, body[1]);
            saw_error = true;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE (saw_error, "expected ERROR frame");

    close (raw);
    test_context_socket_close (server);
}

void test_paired_ready_generation_mismatch_is_not_attached ()
{
    assert_paired_handshake_not_dispatchable (
      "raw-paired-peer", 41, 7, 0, "raw-paired-peer", 41, 8, 1);
}

void test_paired_ready_pair_id_mismatch_is_not_attached ()
{
    assert_paired_handshake_not_dispatchable (
      "raw-paired-peer", 41, 7, 0, "raw-paired-peer", 42, 7, 1);
}

void test_paired_ready_peer_identity_mismatch_is_not_attached ()
{
    assert_paired_handshake_not_dispatchable (
      "raw-paired-peer-a", 41, 7, 0, "raw-paired-peer-b", 41, 7, 1);
}

void test_paired_ready_duplicate_lane_is_not_attached ()
{
    assert_paired_handshake_not_dispatchable (
      "raw-paired-peer", 41, 7, 0, "raw-paired-peer", 41, 7, 0);
}

void test_stale_completion_lane_cannot_complete_reconnected_request ()
{
    void *server = test_context_socket (ZLINK_SOCKET_DEALER);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    const char peer_name[] = "raw-reconnect-peer";
    fd_t old_application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    fd_t old_completion = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    set_recv_timeout (old_application, 2000);
    TEST_ASSERT_TRUE (
      send_paired_dealer_handshake (
        old_application, peer_name, 73, 1, 0, ZLINK_SOCKET_ROUTER));
    TEST_ASSERT_TRUE (
      send_paired_dealer_handshake (
        old_completion, peer_name, 73, 1, 1, ZLINK_SOCKET_ROUTER));
    msleep (SETTLE_TIME * 20);

    zlink_msg_t first_request;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&first_request, 1));
    reply_probe_t first_probe;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (
        server, &first_request, ZLINK_SEND_FLAGS_NONE,
        ZLINK_PART_FINAL, 5000, &capture_reply, &first_probe));
    TEST_ASSERT_TRUE (read_raw_request_sequence (old_application) != 0);

    close (old_application);
    TEST_ASSERT_TRUE (wait_for_calls (&first_probe, 1));

    fd_t new_application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    fd_t new_completion = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    set_recv_timeout (new_application, 2000);
    TEST_ASSERT_TRUE (
      send_paired_dealer_handshake (
        new_application, peer_name, 73, 2, 0, ZLINK_SOCKET_ROUTER));
    TEST_ASSERT_TRUE (
      send_paired_dealer_handshake (
        new_completion, peer_name, 73, 2, 1, ZLINK_SOCKET_ROUTER));
    msleep (SETTLE_TIME * 20);

    zlink_msg_t second_request;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&second_request, 1));
    reply_probe_t second_probe;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request_part (
        server, &second_request, ZLINK_SEND_FLAGS_NONE,
        ZLINK_PART_FINAL, 5000, &capture_reply, &second_probe));
    const uint64_t second_sequence =
      read_raw_request_sequence (new_application);

    // The old generation may still accept bytes in the local TCP send buffer,
    // but Core must not apply them to the new generation's pending request.
    (void) send_raw_reply (old_completion, second_sequence);
    msleep (SETTLE_TIME * 2);
    TEST_ASSERT_EQUAL_INT (
      0, second_probe.calls.load (std::memory_order_acquire));

    TEST_ASSERT_TRUE (send_raw_reply (new_completion, second_sequence));
    TEST_ASSERT_TRUE (wait_for_calls (&second_probe, 1));
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, second_probe.result);

    close (new_completion);
    close (new_application);
    close (old_completion);
    test_context_socket_close_zero_linger (server);
}

int main (void)
{
    UNITY_BEGIN ();

    setup_test_environment ();

    RUN_TEST (test_zmp_error_invalid_hello);
    RUN_TEST (test_network_incomplete_multipart_stops_at_receiver_max_message_size);
    RUN_TEST (test_paired_ready_generation_mismatch_is_not_attached);
    RUN_TEST (test_paired_ready_pair_id_mismatch_is_not_attached);
    RUN_TEST (test_paired_ready_peer_identity_mismatch_is_not_attached);
    RUN_TEST (test_paired_ready_duplicate_lane_is_not_attached);
    RUN_TEST (test_stale_completion_lane_cannot_complete_reconnected_request);

    return UNITY_END ();
}
