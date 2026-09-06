#ifndef ZLINK_TEST_ZMP_PEER_FIXTURE_HPP_INCLUDED
#define ZLINK_TEST_ZMP_PEER_FIXTURE_HPP_INCLUDED

/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_zmp_wire.hpp"


#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <errno.h>
#include <map>
#include <mutex>
#include <string>
#include <string.h>
#include <vector>

#ifndef ZLINK_HAVE_WINDOWS
#include <sys/time.h>
#endif


namespace
{
std::mutex raw_pair_alias_sync;
std::map<uint64_t, std::string> raw_pair_aliases;

bool should_run_zmp_metadata_test (const char *name_)
{
    const char *const selected = getenv ("ZLINK_TEST_CASE");
    if (!selected || !*selected)
        return true;

    const size_t name_size = strlen (name_);
    const char *candidate = selected;
    while (*candidate) {
        const char *const delimiter = strchr (candidate, ',');
        const size_t candidate_size = delimiter
                                        ? static_cast<size_t> (delimiter - candidate)
                                        : strlen (candidate);
        if (candidate_size == name_size
            && memcmp (candidate, name_, name_size) == 0)
            return true;
        if (!delimiter)
            break;
        candidate = delimiter + 1;
    }
    return false;
}

bool raw_pair_routing_id (uint64_t legacy_pair_id_, std::string *out_)
{
    std::lock_guard<std::mutex> lock (raw_pair_alias_sync);
    const std::map<uint64_t, std::string>::const_iterator it =
      raw_pair_aliases.find (legacy_pair_id_);
    if (it == raw_pair_aliases.end ())
        return false;
    *out_ = it->second;
    return true;
}

zlink_routing_id_t make_text_routing_id (const char *value_)
{
    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    const size_t size = strlen (value_);
    TEST_ASSERT_TRUE (size <= sizeof (rid.data));
    rid.size = static_cast<uint8_t> (size);
    if (size != 0)
        memcpy (rid.data, value_, size);
    return rid;
}

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

bool fd_readable (fd_t fd_, int timeout_ms_)
{
    fd_set read_set;
    FD_ZERO (&read_set);
    FD_SET (fd_, &read_set);
    struct timeval timeout;
    timeout.tv_sec = timeout_ms_ / 1000;
    timeout.tv_usec = (timeout_ms_ % 1000) * 1000;
#if defined ZLINK_HAVE_WINDOWS
    const int rc = select (0, &read_set, NULL, NULL, &timeout);
#else
    const int rc = select (static_cast<int> (fd_) + 1, &read_set, NULL, NULL,
                           &timeout);
#endif
    return rc > 0 && FD_ISSET (fd_, &read_set);
}

fd_t accept_eventually (fd_t listener_, int timeout_ms_)
{
    if (!fd_readable (listener_, timeout_ms_))
        return retired_fd;
    return accept (listener_, NULL, NULL);
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
            if (err == WSAECONNRESET)
                return recv_closed;
            return recv_error;
        }
#else
        const ssize_t rc = recv (fd_, buf_ + offset, size_ - offset, MSG_NOSIGNAL);
        if (rc == 0)
            return recv_closed;
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return recv_timeout;
            if (errno == ECONNRESET)
                return recv_closed;
            return recv_error;
        }
#endif
        offset += static_cast<size_t> (rc);
    }
    return recv_ok;
}

bool send_zmp_frame (fd_t fd_, unsigned char flags_, const unsigned char *body_, size_t body_len_)
{
    unsigned char header[test_zmp_wire::zmp_header_size];
    header[0] = test_zmp_wire::zmp_magic;
    header[1] = test_zmp_wire::zmp_version;
    header[2] = flags_;
    header[3] = 0;
    test_zmp_wire::put_uint32 (header + 4, static_cast<uint32_t> (body_len_));

    if (!send_all (fd_, header, sizeof (header)))
        return false;
    if (body_len_ > 0)
        return send_all (fd_, body_, body_len_);
    return true;
}

bool send_zmp_request_reply_frame (fd_t fd_,
                                   unsigned char kind_,
                                   uint64_t sequence_,
                                   const unsigned char *body_,
                                   size_t body_len_)
{
    unsigned char header[test_zmp_wire::zmp_request_reply_header_size];
    header[0] = test_zmp_wire::zmp_magic;
    header[1] = test_zmp_wire::zmp_version;
    header[2] = 0;
    header[3] = kind_;
    test_zmp_wire::put_uint32 (header + 4, static_cast<uint32_t> (body_len_));
    test_zmp_wire::put_uint64 (header + test_zmp_wire::zmp_header_size, sequence_);
    if (!send_all (fd_, header, sizeof (header)))
        return false;
    return body_len_ == 0 || send_all (fd_, body_, body_len_);
}

using test_zmp_wire::make_zmp_wire_frame;
using test_zmp_wire::append_wire_frame;

bool send_zmp_control (fd_t fd_, const unsigned char *body_, size_t body_len_)
{
    return send_zmp_frame (fd_, test_zmp_wire::zmp_flag_control, body_, body_len_);
}

bool send_paired_hello_only (fd_t fd_, int socket_type_,
                             const char *routing_id_)
{
    const size_t routing_id_size = strlen (routing_id_);
    if (routing_id_size > 255)
        return false;
    unsigned char hello[3 + 255];
    hello[0] = test_zmp_wire::zmp_control_hello;
    hello[1] = static_cast<unsigned char> (socket_type_);
    hello[2] = static_cast<unsigned char> (routing_id_size);
    if (routing_id_size != 0)
        memcpy (hello + 3, routing_id_, routing_id_size);
    return send_zmp_control (fd_, hello, 3 + routing_id_size);
}

bool send_paired_ready_only (fd_t fd_, int socket_type_,
                             const char *routing_id_, unsigned char lane_count_,
                             unsigned char lane_)
{
    std::vector<unsigned char> ready;
    ready.push_back (test_zmp_wire::zmp_control_ready);
    const char *const socket_type_name =
      socket_type_ == test_zmp_wire::socket_router ? "ROUTER" : "DEALER";
    test_zmp_wire::zmp_metadata::append_property (
      ready, "Socket-Type", socket_type_name, strlen (socket_type_name));
    test_zmp_wire::zmp_metadata::append_property (
      ready, "Routing-Id", routing_id_, strlen (routing_id_));
    test_zmp_wire::zmp_metadata::append_property (
      ready, "Zlink-Lane-Count", &lane_count_, sizeof (lane_count_));
    test_zmp_wire::zmp_metadata::append_property (
      ready, "Zlink-Lane", &lane_, sizeof (lane_));
    return send_zmp_control (fd_, &ready[0], ready.size ());
}

bool send_basic_handshake (fd_t fd_, int socket_type_)
{
    unsigned char hello[3];
    hello[0] = test_zmp_wire::zmp_control_hello;
    hello[1] = static_cast<unsigned char> (socket_type_);
    hello[2] = 0;
    if (!send_zmp_control (fd_, hello, sizeof (hello)))
        return false;

    std::vector<unsigned char> ready;
    ready.push_back (test_zmp_wire::zmp_control_ready);
    const char *socket_type_name =
      socket_type_ == test_zmp_wire::socket_pair
        ? "PAIR"
        : socket_type_ == test_zmp_wire::socket_router ? "ROUTER" : "DEALER";
    test_zmp_wire::zmp_metadata::append_property (
      ready, "Socket-Type", socket_type_name, strlen (socket_type_name));
    return send_zmp_control (fd_, &ready[0], ready.size ());
}

zlink_auto_hwm_budget_snapshot_t read_hwm_snapshot ()
{
    zlink_auto_hwm_budget_snapshot_t snapshot;
    memset (&snapshot, 0, sizeof (snapshot));
    snapshot.abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    snapshot.struct_size = sizeof (snapshot);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_ctx_get_auto_hwm_budget_snapshot (get_test_context (),
                                              &snapshot));
    return snapshot;
}

bool wait_for_current_accounted_bytes (uint64_t expected_)
{
    for (size_t attempt = 0; attempt != 400; ++attempt) {
        if (read_hwm_snapshot ().current_accounted_bytes == expected_)
            return true;
        msleep (5);
    }
    return false;
}

bool wait_for_active_directional_queue_count (uint64_t expected_)
{
    for (size_t attempt = 0; attempt != 400; ++attempt) {
        if (read_hwm_snapshot ().active_directional_queue_count == expected_)
            return true;
        msleep (5);
    }
    return false;
}

bool send_paired_handshake_with_lane_count_property (
  fd_t fd_, const char *routing_id_, uint64_t pair_id_,
  uint64_t generation_, unsigned char lane_, int socket_type_,
  const char *ready_routing_id_, const void *lane_count_data_,
  size_t lane_count_size_, bool include_lane_count_)
{
    const int core_socket_type =
      socket_type_ == ZLINK_SOCKET_DEALER
        ? test_zmp_wire::socket_dealer
        : socket_type_ == ZLINK_SOCKET_ROUTER
            ? test_zmp_wire::socket_router
            : socket_type_;
    unsigned char hello[3 + 255];
    const size_t routing_id_size = strlen (routing_id_);
    if (routing_id_size > 255)
        return false;
    {
        std::lock_guard<std::mutex> lock (raw_pair_alias_sync);
        raw_pair_aliases[pair_id_] = std::string (routing_id_, routing_id_size);
    }
    (void) generation_;
    hello[0] = test_zmp_wire::zmp_control_hello;
    hello[1] = static_cast<unsigned char> (core_socket_type);
    hello[2] = static_cast<unsigned char> (routing_id_size);
    memcpy (hello + 3, routing_id_, routing_id_size);
    if (!send_zmp_control (fd_, hello, 3 + routing_id_size))
        return false;

    std::vector<unsigned char> ready;
    ready.push_back (test_zmp_wire::zmp_control_ready);
    const char *socket_type_name =
      core_socket_type == test_zmp_wire::socket_router ? "ROUTER" : "DEALER";
    const char *const ready_routing_id =
      ready_routing_id_ ? ready_routing_id_ : routing_id_;
    const size_t ready_routing_id_size = strlen (ready_routing_id);
    test_zmp_wire::zmp_metadata::append_property (
      ready, "Socket-Type", socket_type_name, strlen (socket_type_name));
    test_zmp_wire::zmp_metadata::append_property (
      ready, "Routing-Id", ready_routing_id, ready_routing_id_size);

    test_zmp_wire::zmp_metadata::append_property (
      ready, "Zlink-Lane", &lane_, sizeof (lane_));
    if (include_lane_count_)
        test_zmp_wire::zmp_metadata::append_property (
          ready, "Zlink-Lane-Count", lane_count_data_, lane_count_size_);
    return send_zmp_control (fd_, &ready[0], ready.size ());
}

bool send_paired_dealer_handshake (fd_t fd_,
                                   const char *routing_id_,
                                   uint64_t pair_id_,
                                   uint64_t generation_,
                                   unsigned char lane_,
                                   int socket_type_ = ZLINK_SOCKET_DEALER,
                                   const char *ready_routing_id_ = NULL,
                                   unsigned char lane_count_ = 0)
{
    const unsigned char lane_count =
      lane_count_ != 0 ? lane_count_ : lane_ == 0 ? 1 : 2;
    return send_paired_handshake_with_lane_count_property (
      fd_, routing_id_, pair_id_, generation_, lane_, socket_type_,
      ready_routing_id_, &lane_count, sizeof (lane_count), true);
}

bool read_zmp_frame (fd_t fd_,
                     unsigned char &flags_,
                     std::vector<unsigned char> &body_,
                     bool &closed_,
                     unsigned char *kind_out_ = NULL,
                     uint64_t *sequence_out_ = NULL)
{
    unsigned char header[test_zmp_wire::zmp_header_size];
    const recv_status_t header_rc = recv_all (fd_, header, sizeof (header));
    if (header_rc == recv_closed) {
        closed_ = true;
        return false;
    }
    if (header_rc != recv_ok)
        return false;
    if (header[0] != test_zmp_wire::zmp_magic
        || header[1] != test_zmp_wire::zmp_version)
        return false;

    const unsigned char kind = header[3];
    uint64_t sequence = 0;
    if (test_zmp_wire::zmp_is_request_reply_kind (kind)) {
        unsigned char sequence_bytes[test_zmp_wire::zmp_request_sequence_size];
        const recv_status_t sequence_rc =
          recv_all (fd_, sequence_bytes, sizeof (sequence_bytes));
        if (sequence_rc == recv_closed) {
            closed_ = true;
            return false;
        }
        if (sequence_rc != recv_ok)
            return false;
        sequence = test_zmp_wire::get_uint64 (sequence_bytes);
    }

    const uint32_t body_len = test_zmp_wire::get_uint32 (header + 4);
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
    if (kind_out_)
        *kind_out_ = kind;
    if (sequence_out_)
        *sequence_out_ = sequence;
    return true;
}

fd_t accept_core_hello (fd_t listener_, int timeout_ms_)
{
    const fd_t connection = accept_eventually (listener_, timeout_ms_);
    if (connection == retired_fd)
        return retired_fd;
    set_recv_timeout (connection, timeout_ms_);
    unsigned char flags = 0;
    std::vector<unsigned char> body;
    bool closed = false;
    if (!read_zmp_frame (connection, flags, body, closed)
        || closed || (flags & test_zmp_wire::zmp_flag_control) == 0
        || body.empty () || body[0] != test_zmp_wire::zmp_control_hello) {
        close (connection);
        return retired_fd;
    }
    return connection;
}

bool read_paired_ready_lane (fd_t fd_, unsigned char *count_out_,
                             unsigned char *lane_out_)
{
    for (size_t attempt = 0; attempt != 8; ++attempt) {
        unsigned char flags = 0;
        std::vector<unsigned char> body;
        bool closed = false;
        if (!read_zmp_frame (fd_, flags, body, closed) || closed)
            return false;
        if ((flags & test_zmp_wire::zmp_flag_control) == 0 || body.empty ()
            || body[0] != test_zmp_wire::zmp_control_ready)
            continue;
        test_zmp_wire::zmp_metadata::properties_t properties;
        if (test_zmp_wire::zmp_metadata::parse (
              body.size () == 1 ? NULL : &body[1], body.size () - 1,
              properties)
            != 0)
            return false;
        const test_zmp_wire::zmp_metadata::properties_t::const_iterator count =
          properties.find ("Zlink-Lane-Count");
        const test_zmp_wire::zmp_metadata::properties_t::const_iterator lane =
          properties.find ("Zlink-Lane");
        if (count == properties.end () || lane == properties.end ()
            || count->second.size () != 1 || lane->second.size () != 1)
            return false;
        *count_out_ = static_cast<unsigned char> (count->second[0]);
        *lane_out_ = static_cast<unsigned char> (lane->second[0]);
        return true;
    }
    return false;
}

bool wait_for_raw_ready (fd_t fd_)
{
    for (size_t attempt = 0; attempt != 16; ++attempt) {
        unsigned char flags = 0;
        std::vector<unsigned char> body;
        bool closed = false;
        if (!read_zmp_frame (fd_, flags, body, closed) || closed)
            return false;
        if ((flags & test_zmp_wire::zmp_flag_control) != 0 && !body.empty ()
            && body[0] == test_zmp_wire::zmp_control_ready)
            return true;
    }
    return false;
}




bool wait_for_transport_pair_admission (void *socket_,
                                        uint64_t pair_id_,
                                        uint64_t generation_)
{
    std::string peer_routing_id;
    if (!raw_pair_routing_id (pair_id_, &peer_routing_id))
        return false;
    (void) generation_;
    zlink_socket_monitor_open_options_t options = {};
    options.events = ZLINK_EVENT_CONNECTION_READY;
    void *monitor = zlink_socket_monitor_open (socket_, &options);
    if (!monitor)
        return false;
    bool ready = false;
    const int poll_step_ms = 5;
    const int poll_attempts = 5000 / poll_step_ms;
    for (int attempt = 0; attempt <= poll_attempts; ++attempt) {
        zlink_monitor_status_t snapshot = {};
        if (zlink_monitor_status (monitor, &snapshot) != ZLINK_CONFIG_OK)
            break;
        if ((snapshot.state_flags & ZLINK_MONITOR_STATE_READY) != 0) {
            ready = true;
            break;
        }
        if (attempt != poll_attempts)
            msleep (poll_step_ms);
    }
    const zlink_close_result_t close_rc = zlink_monitor_close (&monitor);
    return ready && close_rc == ZLINK_CLOSE_OK;
}

bool wait_for_raw_close (fd_t fd_, bool timeout_is_set_ = false)
{
    if (!timeout_is_set_)
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

void shutdown_raw_send (fd_t fd_)
{
#if defined ZLINK_HAVE_WINDOWS
    (void) shutdown (fd_, SD_SEND);
#else
    (void) shutdown (fd_, SHUT_WR);
#endif
}

bool read_next_application_frame (fd_t fd_,
                                  unsigned char *flags_out_,
                                  unsigned char *kind_out_,
                                  uint64_t *sequence_out_,
                                  std::vector<unsigned char> *body_out_)
{
    for (size_t attempt = 0; attempt != 16; ++attempt) {
        unsigned char flags = 0;
        unsigned char kind = test_zmp_wire::zmp_kind_data;
        uint64_t sequence = 0;
        std::vector<unsigned char> body;
        bool closed = false;
        if (!read_zmp_frame (
              fd_, flags, body, closed, &kind, &sequence))
            return false;
        if (closed)
            return false;
        if (flags & (test_zmp_wire::zmp_flag_control | test_zmp_wire::zmp_flag_identity))
            continue;
        *flags_out_ = flags;
        *kind_out_ = kind;
        *sequence_out_ = sequence;
        body_out_->swap (body);
        return true;
    }
    return false;
}

uint64_t assert_raw_two_part_application_record (
  fd_t fd_,
  unsigned char first_kind_,
  uint64_t expected_sequence_,
  const unsigned char *first_payload_,
  size_t first_payload_size_,
  const unsigned char *second_payload_,
  size_t second_payload_size_)
{
    unsigned char first_flags = 0;
    unsigned char first_kind = 0xff;
    uint64_t first_sequence = UINT64_MAX;
    std::vector<unsigned char> first_body;
    TEST_ASSERT_TRUE (read_next_application_frame (
      fd_, &first_flags, &first_kind, &first_sequence, &first_body));
    TEST_ASSERT_EQUAL_HEX8 (test_zmp_wire::zmp_flag_more, first_flags);
    TEST_ASSERT_EQUAL_HEX8 (first_kind_, first_kind);
    if (first_kind_ == test_zmp_wire::zmp_kind_request && expected_sequence_ == 0) {
        TEST_ASSERT_TRUE (first_sequence != 0);
    } else {
        TEST_ASSERT_EQUAL_UINT64 (expected_sequence_, first_sequence);
    }
    TEST_ASSERT_EQUAL_UINT64 (first_payload_size_, first_body.size ());
    TEST_ASSERT_EQUAL_MEMORY (first_payload_, &first_body[0],
                              first_body.size ());

    unsigned char second_flags = 0;
    unsigned char second_kind = 0xff;
    uint64_t second_sequence = UINT64_MAX;
    std::vector<unsigned char> second_body;
    TEST_ASSERT_TRUE (read_next_application_frame (
      fd_, &second_flags, &second_kind, &second_sequence, &second_body));
    TEST_ASSERT_EQUAL_HEX8 (0, second_flags);
    TEST_ASSERT_EQUAL_HEX8 (test_zmp_wire::zmp_kind_data, second_kind);
    TEST_ASSERT_EQUAL_UINT64 (0, second_sequence);
    TEST_ASSERT_EQUAL_UINT64 (second_payload_size_, second_body.size ());
    TEST_ASSERT_EQUAL_MEMORY (second_payload_, &second_body[0],
                              second_body.size ());

    //  Exactly two application parts must produce exactly two wire frames.
    set_recv_timeout (fd_, 100);
    unsigned char extra_flags = 0;
    unsigned char extra_kind = 0xff;
    uint64_t extra_sequence = UINT64_MAX;
    std::vector<unsigned char> extra_body;
    TEST_ASSERT_FALSE (read_next_application_frame (
      fd_, &extra_flags, &extra_kind, &extra_sequence, &extra_body));
    set_recv_timeout (fd_, 2000);
    return first_sequence;
}

void init_two_part_application_record (
  zlink_msg_t parts_[2],
  const unsigned char *first_payload_,
  size_t first_payload_size_,
  const unsigned char *second_payload_,
  size_t second_payload_size_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&parts_[0], first_payload_size_));
    memcpy (zlink_msg_data (&parts_[0]), first_payload_, first_payload_size_);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&parts_[1], second_payload_size_));
    memcpy (zlink_msg_data (&parts_[1]), second_payload_, second_payload_size_);
}

void assert_pair_has_no_raw_payload (void *server_)
{
    unsigned char payload[32];
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      -1, zlink_recv (server_, payload, sizeof (payload), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
}

void assert_raw_stream_rejected (const std::vector<unsigned char> &stream_)
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    fd_t raw = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, raw);
    TEST_ASSERT_TRUE (send_basic_handshake (raw, test_zmp_wire::socket_pair));
    TEST_ASSERT_TRUE (send_all (raw, &stream_[0], stream_.size ()));
    shutdown_raw_send (raw);
    TEST_ASSERT_TRUE (wait_for_raw_close (raw));
    assert_pair_has_no_raw_payload (server);
    close (raw);
    test_context_socket_close (server);
}

uint64_t read_raw_request_sequence (fd_t fd_)
{
    for (size_t attempt = 0; attempt < 16; ++attempt) {
        unsigned char flags = 0;
        unsigned char kind = test_zmp_wire::zmp_kind_data;
        uint64_t sequence = 0;
        std::vector<unsigned char> body;
        bool closed = false;
        TEST_ASSERT_TRUE (
          read_zmp_frame (fd_, flags, body, closed, &kind, &sequence));
        TEST_ASSERT_FALSE (closed);
        if ((flags & (test_zmp_wire::zmp_flag_control | test_zmp_wire::zmp_flag_identity))
            != 0)
            continue;
        if (kind != test_zmp_wire::zmp_kind_request)
            continue;
        TEST_ASSERT_TRUE (sequence != 0);
        return sequence;
    }
    TEST_FAIL_MESSAGE ("request metadata was not received");
    return 0;
}

bool send_raw_reply (fd_t fd_, uint64_t request_seq_)
{
    return send_zmp_request_reply_frame (
      fd_, test_zmp_wire::zmp_kind_reply, request_seq_, NULL, 0);
}

void init_empty_completion (zlink_completion_t *completion_)
{
    memset (completion_, 0, sizeof (*completion_));
    completion_->struct_size = sizeof (*completion_);
}

zlink_completion_t receive_request_completion_eventually (void *socket_)
{
    zlink_completion_t completion;
    init_empty_completion (&completion);
    for (int i = 0; i < 1000; ++i) {
        errno = 0;
        const zlink_recv_result_t result = zlink_completion_recv (
          socket_, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST,
                                   completion.kind);
            return completion;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
        msleep (5);
    }
    TEST_FAIL_MESSAGE ("request completion was not received");
    return completion;
}

void assert_no_completion (void *socket_)
{
    zlink_completion_t completion;
    init_empty_completion (&completion);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (socket_, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    zlink_completion_close (&completion);
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
    set_recv_timeout (application, 100);
    set_recv_timeout (completion, 100);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      application, application_routing_id_, application_pair_id_,
      application_generation_, application_lane_, ZLINK_SOCKET_ROUTER,
      NULL, 2));
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      completion, completion_routing_id_, completion_pair_id_,
      completion_generation_, completion_lane_, ZLINK_SOCKET_ROUTER,
      NULL, 2));

    // ROUTER-ROUTER count 2 lanes with different adopted Routing-Ids, or two
    // instances of the same lane, must never form a dispatchable pair.
    // Pair-id/generation are no longer wire metadata, so there is no physical
    // identifier to compare.
    msleep (100);

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


void run_raw_error_reply_case (uint64_t pair_id_,
                               const unsigned char *errno_part_,
                               size_t errno_part_size_,
                               bool include_payload_,
                               zlink_request_result_t expected_result_,
                               size_t expected_payload_count_)
{
    void *server = test_context_socket (ZLINK_SOCKET_DEALER);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    fd_t application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, application);
    set_recv_timeout (application, 2000);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      application, "raw-error-reply", pair_id_, 1, 0,
      test_zmp_wire::socket_router));
    TEST_ASSERT_TRUE (wait_for_raw_ready (application));
    TEST_ASSERT_TRUE (
      wait_for_transport_pair_admission (server, pair_id_, 1));

    zlink_msg_t request;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&request, 1));
    *static_cast<unsigned char *> (zlink_msg_data (&request)) = 'q';
    zlink_completion_id_t completion_id = 0;
    int user_context_value = 17;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (server, NULL, &request, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_FINAL, 5000, &user_context_value,
                          &completion_id));
    TEST_ASSERT_TRUE (completion_id != 0);
    const uint64_t request_sequence =
      read_raw_request_sequence (application);

    std::vector<unsigned char> completion_record = make_zmp_wire_frame (
      include_payload_ ? test_zmp_wire::zmp_flag_more : 0,
      test_zmp_wire::zmp_kind_error_reply, request_sequence, errno_part_,
      errno_part_size_);
    if (include_payload_) {
        static const unsigned char detail_a[] = {'d', 'e', 't', 'a', 'i', 'l'};
        static const unsigned char detail_b[] = {'c', 'o', 'n', 't', 'e', 'x', 't'};
        append_wire_frame (&completion_record, make_zmp_wire_frame (
          test_zmp_wire::zmp_flag_more, test_zmp_wire::zmp_kind_data, 0, detail_a,
          sizeof (detail_a)));
        append_wire_frame (&completion_record, make_zmp_wire_frame (
          0, test_zmp_wire::zmp_kind_data, 0, detail_b, sizeof (detail_b)));
    }
    TEST_ASSERT_TRUE (send_all (
      application, &completion_record[0], completion_record.size ()));

    zlink_completion_t request_completion =
      receive_request_completion_eventually (server);
    TEST_ASSERT_EQUAL_UINT64 (completion_id,
                              request_completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (&user_context_value,
                           request_completion.user_context);
    TEST_ASSERT_EQUAL_INT (expected_result_,
                           request_completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (expected_payload_count_,
                              request_completion.reply_part_count);
    if (expected_payload_count_ == 0)
        TEST_ASSERT_NULL (request_completion.reply_parts);
    if (expected_payload_count_ == 2) {
        TEST_ASSERT_EQUAL_UINT64 (
          6, zlink_msg_size (&request_completion.reply_parts[0]));
        TEST_ASSERT_EQUAL_MEMORY (
          "detail", zlink_msg_data (&request_completion.reply_parts[0]), 6);
        TEST_ASSERT_EQUAL_UINT64 (
          7, zlink_msg_size (&request_completion.reply_parts[1]));
        TEST_ASSERT_EQUAL_MEMORY (
          "context", zlink_msg_data (&request_completion.reply_parts[1]), 7);
    }
    zlink_completion_close (&request_completion);
    zlink_completion_close (&request_completion);
    msleep (SETTLE_TIME);
    assert_no_completion (server);

    close (application);
    test_context_socket_close_zero_linger (server);
}

#endif
