/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "testutil_monitoring.hpp"

#include "protocol/wire.hpp"
#include "protocol/zmp_metadata.hpp"
#include "protocol/zmp_protocol.hpp"
#include "api/message/request_result_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "sockets/dealer/dealer.hpp"
#include "sockets/router/router.hpp"

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

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
std::mutex raw_pair_alias_sync;
std::map<uint64_t, std::string> raw_pair_aliases;

struct owner_after_claim_gate_t
{
    owner_after_claim_gate_t () : entered (false) {}

    std::mutex sync;
    std::condition_variable cv;
    bool entered;
};

bool defer_first_owner_after_claim (uint64_t, uint64_t, uint64_t,
                                    void *userdata_)
{
    owner_after_claim_gate_t *const gate =
      static_cast<owner_after_claim_gate_t *> (userdata_);
    if (!gate)
        return false;
    std::lock_guard<std::mutex> lock (gate->sync);
    if (gate->entered)
        return false;
    gate->entered = true;
    gate->cv.notify_all ();
    return true;
}

bool wait_owner_gate_entered (owner_after_claim_gate_t *gate_, int timeout_ms_)
{
    std::unique_lock<std::mutex> lock (gate_->sync);
    return gate_->cv.wait_for (
      lock, std::chrono::milliseconds (timeout_ms_),
      [gate_] { return gate_->entered; });
}

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

bool send_zmp_request_reply_frame (fd_t fd_,
                                   unsigned char kind_,
                                   uint64_t sequence_,
                                   const unsigned char *body_,
                                   size_t body_len_)
{
    unsigned char header[zlink::zmp_request_reply_header_size];
    header[0] = zlink::zmp_magic;
    header[1] = zlink::zmp_version;
    header[2] = 0;
    header[3] = kind_;
    zlink::put_uint32 (header + 4, static_cast<uint32_t> (body_len_));
    zlink::put_uint64 (header + zlink::zmp_header_size, sequence_);
    if (!send_all (fd_, header, sizeof (header)))
        return false;
    return body_len_ == 0 || send_all (fd_, body_, body_len_);
}

std::vector<unsigned char> make_zmp_wire_frame (
  unsigned char flags_, unsigned char kind_, uint64_t sequence_,
  const unsigned char *body_, size_t body_len_)
{
    const bool extended = zlink::zmp_is_request_reply_kind (kind_);
    const size_t header_size =
      extended ? zlink::zmp_request_reply_header_size
               : zlink::zmp_header_size;
    std::vector<unsigned char> frame (header_size + body_len_);
    frame[0] = zlink::zmp_magic;
    frame[1] = zlink::zmp_version;
    frame[2] = flags_;
    frame[3] = kind_;
    zlink::put_uint32 (&frame[4], static_cast<uint32_t> (body_len_));
    if (extended)
        zlink::put_uint64 (&frame[zlink::zmp_header_size], sequence_);
    if (body_len_ > 0)
        memcpy (&frame[header_size], body_, body_len_);
    return frame;
}

void append_wire_frame (std::vector<unsigned char> *stream_,
                        const std::vector<unsigned char> &frame_)
{
    stream_->insert (stream_->end (), frame_.begin (), frame_.end ());
}

bool send_zmp_control (fd_t fd_, const unsigned char *body_, size_t body_len_)
{
    return send_zmp_frame (fd_, zlink::zmp_flag_control, body_, body_len_);
}

bool send_paired_hello_only (fd_t fd_, int socket_type_,
                             const char *routing_id_)
{
    const size_t routing_id_size = strlen (routing_id_);
    if (routing_id_size > 255)
        return false;
    unsigned char hello[3 + 255];
    hello[0] = zlink::zmp_control_hello;
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
    ready.push_back (zlink::zmp_control_ready);
    const char *const socket_type_name =
      socket_type_ == ZLINK_CORE_SOCKET_ROUTER ? "ROUTER" : "DEALER";
    zlink::zmp_metadata::append_property (
      ready, "Socket-Type", socket_type_name, strlen (socket_type_name));
    zlink::zmp_metadata::append_property (
      ready, "Routing-Id", routing_id_, strlen (routing_id_));
    zlink::zmp_metadata::append_property (
      ready, "Zlink-Lane-Count", &lane_count_, sizeof (lane_count_));
    zlink::zmp_metadata::append_property (
      ready, "Zlink-Lane", &lane_, sizeof (lane_));
    return send_zmp_control (fd_, &ready[0], ready.size ());
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
    const char *socket_type_name =
      socket_type_ == ZLINK_CORE_SOCKET_PAIR
        ? "PAIR"
        : socket_type_ == ZLINK_CORE_SOCKET_ROUTER ? "ROUTER" : "DEALER";
    zlink::zmp_metadata::append_property (
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
        ? ZLINK_CORE_SOCKET_DEALER
        : socket_type_ == ZLINK_SOCKET_ROUTER
            ? ZLINK_CORE_SOCKET_ROUTER
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
    hello[0] = zlink::zmp_control_hello;
    hello[1] = static_cast<unsigned char> (core_socket_type);
    hello[2] = static_cast<unsigned char> (routing_id_size);
    memcpy (hello + 3, routing_id_, routing_id_size);
    if (!send_zmp_control (fd_, hello, 3 + routing_id_size))
        return false;

    std::vector<unsigned char> ready;
    ready.push_back (zlink::zmp_control_ready);
    const char *socket_type_name =
      core_socket_type == ZLINK_CORE_SOCKET_ROUTER ? "ROUTER" : "DEALER";
    const char *const ready_routing_id =
      ready_routing_id_ ? ready_routing_id_ : routing_id_;
    const size_t ready_routing_id_size = strlen (ready_routing_id);
    zlink::zmp_metadata::append_property (
      ready, "Socket-Type", socket_type_name, strlen (socket_type_name));
    zlink::zmp_metadata::append_property (
      ready, "Routing-Id", ready_routing_id, ready_routing_id_size);

    zlink::zmp_metadata::append_property (
      ready, "Zlink-Lane", &lane_, sizeof (lane_));
    if (include_lane_count_)
        zlink::zmp_metadata::append_property (
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
    unsigned char header[zlink::zmp_header_size];
    const recv_status_t header_rc = recv_all (fd_, header, sizeof (header));
    if (header_rc == recv_closed) {
        closed_ = true;
        return false;
    }
    if (header_rc != recv_ok)
        return false;
    if (header[0] != zlink::zmp_magic
        || header[1] != zlink::zmp_version)
        return false;

    const unsigned char kind = header[3];
    uint64_t sequence = 0;
    if (zlink::zmp_is_request_reply_kind (kind)) {
        unsigned char sequence_bytes[zlink::zmp_request_sequence_size];
        const recv_status_t sequence_rc =
          recv_all (fd_, sequence_bytes, sizeof (sequence_bytes));
        if (sequence_rc == recv_closed) {
            closed_ = true;
            return false;
        }
        if (sequence_rc != recv_ok)
            return false;
        sequence = zlink::get_uint64 (sequence_bytes);
    }

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
        || closed || (flags & zlink::zmp_flag_control) == 0
        || body.empty () || body[0] != zlink::zmp_control_hello) {
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
        if ((flags & zlink::zmp_flag_control) == 0 || body.empty ()
            || body[0] != zlink::zmp_control_ready)
            continue;
        zlink::zmp_metadata::properties_t properties;
        if (zlink::zmp_metadata::parse (
              body.size () == 1 ? NULL : &body[1], body.size () - 1,
              properties)
            != 0)
            return false;
        const zlink::zmp_metadata::properties_t::const_iterator count =
          properties.find ("Zlink-Lane-Count");
        const zlink::zmp_metadata::properties_t::const_iterator lane =
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
        if ((flags & zlink::zmp_flag_control) != 0 && !body.empty ()
            && body[0] == zlink::zmp_control_ready)
            return true;
    }
    return false;
}

bool wait_for_transport_pair_admission (void *socket_,
                                        uint64_t pair_id_,
                                        uint64_t generation_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket)
        return false;

    std::string peer_routing_id;
    if (!raw_pair_routing_id (pair_id_, &peer_routing_id))
        return false;
    (void) generation_;

    // Receiving both wire READY frames does not imply that the socket thread
    // has applied their pair-admission commands yet. A snapshot drains those
    // commands before the adopted logical RID is inspected.
    const int poll_step_ms = 5;
    const int poll_attempts = 5000 / poll_step_ms;
    for (int attempt = 0; attempt <= poll_attempts; ++attempt) {
        zlink_monitor_status_t snapshot;
        if (handle.socket->monitor_snapshot (&snapshot) != 0)
            return false;
        uint64_t internal_pair_id = 0;
        uint64_t internal_generation = 0;
        bool ready = false;
        if (handle.socket->test_pair_identity_for_peer (
              reinterpret_cast<const unsigned char *> (
                peer_routing_id.data ()),
              peer_routing_id.size (), &internal_pair_id,
              &internal_generation, &ready)
            && ready)
            return true;
        if (attempt != poll_attempts)
            msleep (poll_step_ms);
    }
    return false;
}

bool retire_application_transport (void *socket_, uint64_t pair_id_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket)
        return false;
    std::string peer_routing_id;
    if (!raw_pair_routing_id (pair_id_, &peer_routing_id))
        return false;

    zlink_monitor_status_t snapshot;
    if (handle.socket->monitor_snapshot (&snapshot) != 0)
        return false;
    uint64_t internal_pair_id = 0;
    uint64_t internal_generation = 0;
    bool ready = false;
    if (!handle.socket->test_pair_identity_for_peer (
          reinterpret_cast<const unsigned char *> (peer_routing_id.data ()),
          peer_routing_id.size (), &internal_pair_id, &internal_generation,
          &ready)
        || !ready)
        return false;

    zlink::pipe_t *const application =
      handle.socket->retain_transport_pair_pipe (
        internal_pair_id, internal_generation,
        zlink::transport_lane_application);
    if (!application)
        return false;
    application->terminate (false);
    application->release_lifetime_ref ();
    return true;
}

bool wait_for_peer_weight (void *socket_, uint64_t pair_id_,
                           uint64_t generation_, uint32_t expected_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    if (!handle.socket)
        return false;
    std::string peer_routing_id;
    if (!raw_pair_routing_id (pair_id_, &peer_routing_id))
        return false;
    (void) generation_;
    for (size_t attempt = 0; attempt != 600; ++attempt) {
        zlink_monitor_status_t snapshot;
        if (handle.socket->monitor_snapshot (&snapshot) != 0)
            return false;
        uint64_t internal_pair_id = 0;
        uint64_t internal_generation = 0;
        zlink::pipe_t *application = NULL;
        if (handle.socket->test_pair_identity_for_peer (
              reinterpret_cast<const unsigned char *> (
                peer_routing_id.data ()),
              peer_routing_id.size (), &internal_pair_id,
              &internal_generation))
            application = handle.socket->test_pair_pipe (
              internal_pair_id, internal_generation, false);
        if (application) {
            const uint32_t weight =
              handle.socket->socket_type () == ZLINK_CORE_SOCKET_DEALER
                ? static_cast<zlink::dealer_t *> (handle.socket)
                    ->test_peer_weight (application)
                : static_cast<zlink::router_t *> (handle.socket)
                    ->test_peer_weight (application);
            if (weight == expected_)
                return true;
        }
        msleep (5);
    }
    return false;
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
        unsigned char kind = zlink::zmp_kind_data;
        uint64_t sequence = 0;
        std::vector<unsigned char> body;
        bool closed = false;
        if (!read_zmp_frame (
              fd_, flags, body, closed, &kind, &sequence))
            return false;
        if (closed)
            return false;
        if (flags & (zlink::zmp_flag_control | zlink::zmp_flag_identity))
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
    TEST_ASSERT_EQUAL_HEX8 (zlink::zmp_flag_more, first_flags);
    TEST_ASSERT_EQUAL_HEX8 (first_kind_, first_kind);
    if (first_kind_ == zlink::zmp_kind_request && expected_sequence_ == 0) {
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
    TEST_ASSERT_EQUAL_HEX8 (zlink::zmp_kind_data, second_kind);
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
    TEST_ASSERT_TRUE (send_basic_handshake (raw, ZLINK_CORE_SOCKET_PAIR));
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
        unsigned char kind = zlink::zmp_kind_data;
        uint64_t sequence = 0;
        std::vector<unsigned char> body;
        bool closed = false;
        TEST_ASSERT_TRUE (
          read_zmp_frame (fd_, flags, body, closed, &kind, &sequence));
        TEST_ASSERT_FALSE (closed);
        if ((flags & (zlink::zmp_flag_control | zlink::zmp_flag_identity))
            != 0)
            continue;
        if (kind != zlink::request_reply::request_type)
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
      fd_, zlink::request_reply::reply_type, request_seq_, NULL, 0);
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

void test_raw_wire_ordinary_data_uses_eight_byte_kind_zero_header ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    fd_t raw = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, raw);
    set_recv_timeout (raw, 2000);
    TEST_ASSERT_TRUE (send_basic_handshake (raw, ZLINK_CORE_SOCKET_PAIR));

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
    TEST_ASSERT_EQUAL_HEX8 (zlink::zmp_kind_data, kind);
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
      ZLINK_CORE_SOCKET_DEALER));
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

void test_owner_timeout_before_commit_leaves_no_stale_completion_child ()
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set (get_test_context (), ZLINK_IO_THREADS, 2));

    char endpoint[MAX_SOCKET_STRING];
    const fd_t listener =
      bind_socket_resolve_port ("127.0.0.1", "0", endpoint);

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    const int zero = 0;
    const int handshake_ivl = 1000;
    const int reconnect_ivl = 10;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_HANDSHAKE_IVL, &handshake_ivl,
                        sizeof (handshake_ivl)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_RECONNECT_IVL, &reconnect_ivl,
                        sizeof (reconnect_ivl)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (router, "owner-timeout-local",
                            strlen ("owner-timeout-local")));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (
        router, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
        "owner-timeout-peer", strlen ("owner-timeout-peer")));

    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (
      router, ZLINK_EVENT_CONNECTION_READY, &probe);
    owner_after_claim_gate_t gate;

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (router, endpoint));
    const fd_t old_application = accept_core_hello (listener, 3000);
    TEST_ASSERT_NOT_EQUAL (retired_fd, old_application);
    zlink::test_set_transport_pair_owner_after_claim_hook (
      &defer_first_owner_after_claim, &gate);
    const bool hello_sent = send_paired_hello_only (
      old_application, ZLINK_CORE_SOCKET_ROUTER, "owner-timeout-peer");
    const bool owner_claimed =
      hello_sent && wait_owner_gate_entered (&gate, 3000);

    // The Application handshake expires while the socket owner continuation is
    // deferred after claim but before commit. No Completion child from that
    // generation may be launched when the exact request is resumed.
    const bool timed_out_before_release =
      owner_claimed && wait_for_raw_close (old_application);
    socket_handle_t handle = as_socket_handle (router);
    // Cleanup is deliberately independent of the assertions below. If the
    // gate wait itself times out while the owner request has nevertheless been
    // deferred, the reserved decision seqnum and owner-progress lease must
    // still be released before Unity aborts this test and tears down context.
    bool owner_resumed =
      handle.socket
      && handle.socket->test_resume_deferred_transport_pair_owner_request ();
    zlink::test_set_transport_pair_owner_after_claim_hook (NULL, NULL);
    if (!owner_resumed && handle.socket) {
        const std::chrono::steady_clock::time_point cleanup_deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (3);
        do {
            owner_resumed =
              handle.socket
              ->test_resume_deferred_transport_pair_owner_request ();
            if (owner_resumed)
                break;
            msleep (1);
        } while (std::chrono::steady_clock::now () < cleanup_deadline);
    }
    close (old_application);

    // Keep every assertion after the paused owner is released and the global
    // hook is cleared so a failed precondition cannot strand context teardown.
    TEST_ASSERT_TRUE (hello_sent);
    TEST_ASSERT_TRUE (owner_claimed);
    TEST_ASSERT_TRUE (timed_out_before_release);
    TEST_ASSERT_TRUE (owner_resumed);

    // Reconnect creates Application first. Keep its HELLO unanswered so the
    // fresh owner cannot legitimately create Completion yet; a second accept at
    // this point would therefore be the canceled generation's stale child.
    const fd_t application = accept_core_hello (listener, 3000);
    TEST_ASSERT_NOT_EQUAL (retired_fd, application);
    TEST_ASSERT_FALSE (fd_readable (listener, 300));

    TEST_ASSERT_TRUE (send_paired_hello_only (
      application, ZLINK_CORE_SOCKET_ROUTER, "owner-timeout-peer"));
    const fd_t completion = accept_core_hello (listener, 3000);
    TEST_ASSERT_NOT_EQUAL (retired_fd, completion);
    TEST_ASSERT_TRUE (send_paired_hello_only (
      completion, ZLINK_CORE_SOCKET_ROUTER, "owner-timeout-peer"));

    unsigned char application_count = 0;
    unsigned char application_lane = 0xff;
    unsigned char completion_count = 0;
    unsigned char completion_lane = 0xff;
    TEST_ASSERT_TRUE (read_paired_ready_lane (
      application, &application_count, &application_lane));
    TEST_ASSERT_TRUE (read_paired_ready_lane (
      completion, &completion_count, &completion_lane));
    TEST_ASSERT_EQUAL_UINT8 (2, application_count);
    TEST_ASSERT_EQUAL_UINT8 (2, completion_count);
    TEST_ASSERT_TRUE ((application_lane == 0 && completion_lane == 1)
                      || (application_lane == 1 && completion_lane == 0));
    TEST_ASSERT_TRUE (send_paired_ready_only (
      application, ZLINK_CORE_SOCKET_ROUTER, "owner-timeout-peer", 2,
      application_lane));
    TEST_ASSERT_TRUE (send_paired_ready_only (
      completion, ZLINK_CORE_SOCKET_ROUTER, "owner-timeout-peer", 2,
      completion_lane));

    int ready_index = -1;
    TEST_ASSERT_TRUE (test_monitor_probe_wait_event_after (
      &probe, ZLINK_EVENT_CONNECTION_READY, 0, 3000, &ready_index));
    TEST_ASSERT_FALSE (fd_readable (listener, 300));

    close (completion);
    close (application);
    close (listener);
    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (router);
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
      ZLINK_CORE_SOCKET_ROUTER));
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
    TEST_ASSERT_EQUAL_HEX8 (zlink::zmp_flag_control, flags);
    TEST_ASSERT_EQUAL_HEX8 (zlink::zmp_kind_data, kind);
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

void test_raw_wire_peer_weight_bypasses_application_limit_and_consumes_malformed ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const int64_t max_message_size = 1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      dealer, ZLINK_OPT_MAXMSGSIZE, &max_message_size,
      sizeof (max_message_size)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (dealer, endpoint, sizeof (endpoint));
    fd_t application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, application);
    set_recv_timeout (application, 2000);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      application, "raw-weight-limit-router", 904, 1, 0,
      ZLINK_CORE_SOCKET_ROUTER));
    TEST_ASSERT_TRUE (wait_for_raw_ready (application));
    TEST_ASSERT_TRUE (wait_for_transport_pair_admission (dealer, 904, 1));
    TEST_ASSERT_TRUE (wait_for_peer_weight (dealer, 904, 1, 100));

    // The name identifies this as WEIGHT, but the missing u32 makes it a
    // type-specific malformed command. It is consumed as an internal no-op.
    static const unsigned char malformed[] = {
      'W', 'E', 'I', 'G', 'H', 'T'};
    TEST_ASSERT_TRUE (
      send_zmp_control (application, malformed, sizeof (malformed)));
    static const unsigned char barrier[] = {'x'};
    TEST_ASSERT_TRUE (send_zmp_frame (application, 0, barrier,
                                      sizeof (barrier)));
    unsigned char received[4];
    TEST_ASSERT_EQUAL_INT (
      1, zlink_recv (dealer, received, sizeof (received), 0));
    TEST_ASSERT_EQUAL_UINT8 ('x', received[0]);
    TEST_ASSERT_TRUE (wait_for_peer_weight (dealer, 904, 1, 100));
    assert_pair_has_no_raw_payload (dealer);

    // A valid fixed 10-byte control bypasses MAXMSGSIZE=1, is applied on the
    // exact Application pipe, and remains absent from public receive.
    static const unsigned char valid[] = {
      'W', 'E', 'I', 'G', 'H', 'T', 0, 0, 0, 7};
    TEST_ASSERT_TRUE (send_zmp_control (application, valid, sizeof (valid)));
    TEST_ASSERT_TRUE (send_zmp_frame (application, 0, barrier,
                                      sizeof (barrier)));
    TEST_ASSERT_EQUAL_INT (
      1, zlink_recv (dealer, received, sizeof (received), 0));
    TEST_ASSERT_EQUAL_UINT8 ('x', received[0]);
    TEST_ASSERT_TRUE (wait_for_peer_weight (dealer, 904, 1, 7));
    assert_pair_has_no_raw_payload (dealer);

    close (application);
    test_context_socket_close_zero_linger (dealer);
}

void test_raw_wire_peer_weight_waits_for_exact_pair_readiness ()
{
    // ROUTER-ROUTER retains two physical lanes. A WEIGHT received on the
    // Application connection may be cached, but it is not scheduler-visible
    // until the matching Completion connection validates the same pair.
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (
      router, ZLINK_EVENT_PEER_WEIGHT_CHANGED, &probe);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));

    fd_t application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, application);
    set_recv_timeout (application, 2000);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      application, "raw-weight-gated-router", 905, 1, 0,
      ZLINK_CORE_SOCKET_ROUTER, NULL, 2));
    TEST_ASSERT_TRUE (wait_for_raw_ready (application));

    static const unsigned char valid[] = {
      'W', 'E', 'I', 'G', 'H', 'T', 0, 0, 0, 41};
    TEST_ASSERT_TRUE (send_zmp_control (application, valid, sizeof (valid)));

    socket_handle_t handle = as_socket_handle (router);
    zlink::router_t *const router_socket =
      static_cast<zlink::router_t *> (handle.socket);
    std::string peer_routing_id;
    TEST_ASSERT_TRUE (raw_pair_routing_id (905, &peer_routing_id));
    uint64_t internal_pair_id = 0;
    uint64_t internal_generation = 0;
    zlink::pipe_t *application_pipe = NULL;
    bool cached = false;
    for (size_t attempt = 0; attempt != 600 && !cached; ++attempt) {
        zlink_monitor_status_t snapshot;
        TEST_ASSERT_SUCCESS_ERRNO (
          handle.socket->monitor_snapshot (&snapshot));
        if (handle.socket->test_pair_identity_for_peer (
              reinterpret_cast<const unsigned char *> (
                peer_routing_id.data ()),
              peer_routing_id.size (), &internal_pair_id,
              &internal_generation))
            application_pipe = handle.socket->test_pair_pipe (
              internal_pair_id, internal_generation, false);
        uint32_t cached_weight = 0;
        cached = application_pipe
                 && application_pipe->peer_weight (&cached_weight)
                 && cached_weight == 41;
        if (!cached)
            msleep (5);
    }
    TEST_ASSERT_TRUE (cached);
    TEST_ASSERT_NOT_NULL (application_pipe);
    TEST_ASSERT_FALSE (handle.socket->test_pair_is_ready (
      internal_pair_id, internal_generation));
    TEST_ASSERT_EQUAL_UINT32 (
      0, router_socket->test_peer_weight (application_pipe));
    TEST_ASSERT_EQUAL_INT (0, test_monitor_probe_count (&probe));

    fd_t completion = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, completion);
    set_recv_timeout (completion, 2000);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      completion, "raw-weight-gated-router", 905, 1, 1,
      ZLINK_CORE_SOCKET_ROUTER, NULL, 2));
    TEST_ASSERT_TRUE (wait_for_raw_ready (completion));
    TEST_ASSERT_TRUE (wait_for_transport_pair_admission (router, 905, 1));
    TEST_ASSERT_TRUE (wait_for_peer_weight (router, 905, 1, 41));
    TEST_ASSERT_TRUE (
      test_monitor_probe_wait_no_additional (&probe, 0, 200));
    assert_pair_has_no_raw_payload (router);

    // Drop the internal test handle before exercising the public close path.
    // A live pin correctly makes zlink_close report EBUSY.
    handle = socket_handle_t ();
    close_test_monitor_probe (&monitor, &probe);
    close (completion);
    close (application);
    test_context_socket_close_zero_linger (router);
}

void test_raw_wire_fragmented_base_extension_and_payload_delivers_once ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    fd_t raw = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, raw);
    TEST_ASSERT_TRUE (send_basic_handshake (raw, ZLINK_CORE_SOCKET_PAIR));

    static const unsigned char payload[] = {
      'f', 'r', 'a', 'g', 'm', 'e', 'n', 't', 'e', 'd'};
    const std::vector<unsigned char> frame = make_zmp_wire_frame (
      0, zlink::zmp_kind_request, UINT64_C (0x1020304050607080),
      payload, sizeof (payload));

    //  Force each decoder stage to span transport reads: partial base header,
    //  partial sequence extension, then partial payload.
    static const size_t cuts[] = {3, zlink::zmp_header_size + 3,
                                  zlink::zmp_request_reply_header_size + 4,
                                  sizeof (payload)
                                    + zlink::zmp_request_reply_header_size};
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
      application, "raw-router", 901, 1, 0, ZLINK_CORE_SOCKET_ROUTER));
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
           application, zlink::zmp_kind_data, 0, first_payload,
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
      application, zlink::zmp_kind_request, 0, first_payload,
      sizeof (first_payload), second_payload, sizeof (second_payload));

    std::vector<unsigned char> raw_reply = make_zmp_wire_frame (
      zlink::zmp_flag_more, zlink::zmp_kind_reply, request_sequence,
      first_payload, sizeof (first_payload));
    append_wire_frame (&raw_reply, make_zmp_wire_frame (
      0, zlink::zmp_kind_data, 0, second_payload, sizeof (second_payload)));
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
      application, "raw-dealer", 902, 1, 0, ZLINK_CORE_SOCKET_DEALER));
    TEST_ASSERT_TRUE (wait_for_raw_ready (application));
    TEST_ASSERT_TRUE (
      wait_for_transport_pair_admission (router, 902, 1));

    static const unsigned char request_payload[] = {'a', 's', 'k'};
    const uint64_t request_sequence = UINT64_C (0x0102030405060708);
    TEST_ASSERT_TRUE (send_zmp_request_reply_frame (
      application, zlink::zmp_kind_request, request_sequence,
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
    TEST_ASSERT_EQUAL_HEX8 (zlink::zmp_kind_reply, kind);
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
    invalid_streams.back ().push_back (zlink::zmp_magic);
    invalid_streams.back ().push_back (zlink::zmp_version);
    invalid_streams.back ().push_back (0);

    //  EOF in the request sequence extension.
    invalid_streams.push_back (make_zmp_wire_frame (
      0, zlink::zmp_kind_request, 1, NULL, 0));
    invalid_streams.back ().resize (zlink::zmp_header_size + 3);

    //  EOF in the declared payload.
    static const unsigned char body[] = {'b', 'o', 'd', 'y'};
    invalid_streams.push_back (make_zmp_wire_frame (
      0, zlink::zmp_kind_data, 0, body, sizeof (body)));
    invalid_streams.back ().resize (zlink::zmp_header_size + 2);

    //  EOF after MORE is a partial logical multipart, even though its first
    //  wire frame is complete.
    invalid_streams.push_back (make_zmp_wire_frame (
      zlink::zmp_flag_more, zlink::zmp_kind_data, 0, body,
      sizeof (body)));

    invalid_streams.push_back (make_zmp_wire_frame (
      0, 0x7f, 0, NULL, 0));
    invalid_streams.push_back (make_zmp_wire_frame (
      0, zlink::zmp_kind_request, 0, NULL, 0));
    static const unsigned char request_special_flags[] = {
      zlink::zmp_flag_control, zlink::zmp_flag_identity,
      zlink::zmp_flag_subscribe, zlink::zmp_flag_cancel};
    for (size_t i = 0;
         i != sizeof (request_special_flags)
                / sizeof (request_special_flags[0]);
         ++i) {
        invalid_streams.push_back (make_zmp_wire_frame (
          request_special_flags[i], zlink::zmp_kind_request, 1, NULL, 0));
    }
    static const unsigned char forbidden_flags[] = {
      0x20, 0x40, 0x80,
      zlink::zmp_flag_control | zlink::zmp_flag_identity,
      zlink::zmp_flag_control | zlink::zmp_flag_more,
      zlink::zmp_flag_subscribe | zlink::zmp_flag_cancel,
      zlink::zmp_flag_subscribe | zlink::zmp_flag_more,
      zlink::zmp_flag_cancel | zlink::zmp_flag_identity};
    for (size_t i = 0;
         i != sizeof (forbidden_flags) / sizeof (forbidden_flags[0]);
         ++i) {
        invalid_streams.push_back (make_zmp_wire_frame (
          forbidden_flags[i], zlink::zmp_kind_data, 0, NULL, 0));
    }

    std::vector<unsigned char> mid_multipart_kind = make_zmp_wire_frame (
      zlink::zmp_flag_more, zlink::zmp_kind_data, 0, body, sizeof (body));
    append_wire_frame (&mid_multipart_kind, make_zmp_wire_frame (
      0, zlink::zmp_kind_request, 2, body, sizeof (body)));
    invalid_streams.push_back (mid_multipart_kind);

    static const unsigned char mid_multipart_special_flags[] = {
      zlink::zmp_flag_identity, zlink::zmp_flag_control,
      zlink::zmp_flag_subscribe, zlink::zmp_flag_cancel};
    for (size_t i = 0;
         i != sizeof (mid_multipart_special_flags)
                / sizeof (mid_multipart_special_flags[0]);
         ++i) {
        std::vector<unsigned char> mid_multipart_special =
          make_zmp_wire_frame (zlink::zmp_flag_more,
                               zlink::zmp_kind_data, 0, body,
                               sizeof (body));
        append_wire_frame (&mid_multipart_special, make_zmp_wire_frame (
          mid_multipart_special_flags[i], zlink::zmp_kind_data, 0, NULL,
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
    TEST_ASSERT_TRUE (send_basic_handshake (raw, ZLINK_CORE_SOCKET_PAIR));
    TEST_ASSERT_TRUE (wait_for_raw_ready (raw));

    const unsigned char payload[6] = {'b', 'o', 'u', 'n', 'd', 's'};
    TEST_ASSERT_TRUE (send_zmp_frame (raw, zlink::zmp_flag_more, payload, sizeof (payload)));

    // The first part is individually valid. Prove that READY completed and
    // that only the aggregate 6 + 6 boundary rejects the next part.
    set_recv_timeout (raw, 100);
    unsigned char flags = 0;
    std::vector<unsigned char> body;
    bool closed = false;
    TEST_ASSERT_FALSE (read_zmp_frame (raw, flags, body, closed));
    TEST_ASSERT_FALSE (closed);

    TEST_ASSERT_TRUE (send_zmp_frame (raw, zlink::zmp_flag_more, payload, sizeof (payload)));
    TEST_ASSERT_TRUE (wait_for_raw_close (raw, true));
    assert_pair_has_no_raw_payload (server);

    close (raw);
    test_context_socket_close_zero_linger (server);
}

void test_tcp_hidden_identity_releases_decoder_reservation ()
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    const uint64_t payload_size = 4;
    const uint64_t frame_bytes = payload_size + sizeof (zlink::msg_t);
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
    TEST_ASSERT_TRUE (send_basic_handshake (raw, ZLINK_CORE_SOCKET_PAIR));

    const unsigned char identity[payload_size] = {'r', 'i', 'd', '0'};
    const unsigned char payload[payload_size] = {'d', 'a', 't', 'a'};
    TEST_ASSERT_TRUE (send_zmp_frame (
      raw, zlink::zmp_flag_identity | zlink::zmp_flag_more,
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
    const uint64_t frame_bytes = payload_size + sizeof (zlink::msg_t);
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
      ZLINK_CORE_SOCKET_DEALER));
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      raw_b_application, "origin-b", 102, 1, 0,
      ZLINK_CORE_SOCKET_DEALER));

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

void test_stale_application_connection_cannot_complete_reconnected_request ()
{
    void *server = test_context_socket (ZLINK_SOCKET_DEALER);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    const char peer_name[] = "raw-reconnect-peer";
    fd_t old_application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    set_recv_timeout (old_application, 2000);
    TEST_ASSERT_TRUE (
      send_paired_dealer_handshake (
        old_application, peer_name, 73, 1, 0, ZLINK_SOCKET_ROUTER));
    TEST_ASSERT_TRUE (wait_for_raw_ready (old_application));
    TEST_ASSERT_TRUE (
      wait_for_transport_pair_admission (server, 73, 1));

    zlink_msg_t first_request;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&first_request, 1));
    zlink_completion_id_t first_completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (server, NULL, &first_request,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 250,
                          NULL, &first_completion_id));
    TEST_ASSERT_TRUE (first_completion_id != 0);
    TEST_ASSERT_TRUE (read_raw_request_sequence (old_application) != 0);

    // Retire the exact count-1 Application transport while retaining the raw
    // peer FD as a stale-generation sender. The pending request terminates on
    // detach, and a same-RID connection can then be admitted independently.
    TEST_ASSERT_TRUE (retire_application_transport (server, 73));
    zlink_completion_t first_completion =
      receive_request_completion_eventually (server);
    TEST_ASSERT_EQUAL_UINT64 (first_completion_id,
                              first_completion.completion_id);
    zlink_completion_close (&first_completion);
    set_recv_timeout (old_application, 100);
    TEST_ASSERT_TRUE (wait_for_raw_close (old_application, true));

    fd_t new_application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    set_recv_timeout (new_application, 2000);
    TEST_ASSERT_TRUE (
      send_paired_dealer_handshake (
        new_application, peer_name, 73, 2, 0, ZLINK_SOCKET_ROUTER));
    TEST_ASSERT_TRUE (wait_for_raw_ready (new_application));
    TEST_ASSERT_TRUE (
      wait_for_transport_pair_admission (server, 73, 2));

    zlink_msg_t second_request;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&second_request, 1));
    zlink_completion_id_t second_completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (server, NULL, &second_request,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 5000,
                          NULL, &second_completion_id));
    TEST_ASSERT_TRUE (second_completion_id != 0);
    const uint64_t second_sequence =
      read_raw_request_sequence (new_application);

    // A retired single Application connection may reject the write locally or
    // accept it into a stale TCP buffer. Neither outcome may complete the
    // request registered on the new physical generation.
    (void) send_raw_reply (old_application, second_sequence);
    msleep (SETTLE_TIME * 2);
    assert_no_completion (server);

    TEST_ASSERT_TRUE (send_raw_reply (new_application, second_sequence));
    zlink_completion_t second_completion =
      receive_request_completion_eventually (server);
    TEST_ASSERT_EQUAL_UINT64 (second_completion_id,
                              second_completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK,
                           second_completion.request_result);
    zlink_completion_close (&second_completion);

    close (new_application);
    close (old_application);
    test_context_socket_close_zero_linger (server);
}

void test_completion_lane_rejects_data_and_request_kinds_without_completing_them ()
{
    const unsigned char invalid_kinds[] = {zlink::zmp_kind_data,
                                           zlink::zmp_kind_request};
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
          ZLINK_CORE_SOCKET_ROUTER, NULL, 2));
        TEST_ASSERT_TRUE (send_paired_dealer_handshake (
          completion, "invalid-completion-kind", 950 + kind_index, 1, 1,
          ZLINK_CORE_SOCKET_ROUTER, NULL, 2));
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
        if (invalid_kinds[kind_index] == zlink::zmp_kind_data) {
            TEST_ASSERT_TRUE (send_zmp_frame (
              completion, 0, invalid_payload, sizeof (invalid_payload)));
        } else {
            TEST_ASSERT_TRUE (send_zmp_request_reply_frame (
              completion, zlink::zmp_kind_request, request_sequence,
              invalid_payload, sizeof (invalid_payload)));
        }

        zlink_completion_t request_completion =
          receive_request_completion_eventually (server);
        TEST_ASSERT_EQUAL_UINT64 (completion_id,
                                  request_completion.completion_id);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT,
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

void run_raw_error_reply_case (uint64_t pair_id_,
                               const unsigned char *errno_part_,
                               size_t errno_part_size_,
                               bool include_payload_,
                               zlink_request_result_t expected_result_,
                               size_t expected_payload_count_,
                               bool fail_payload_export_ = false)
{
    void *server = test_context_socket (ZLINK_SOCKET_DEALER);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    fd_t application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, application);
    set_recv_timeout (application, 2000);
    TEST_ASSERT_TRUE (send_paired_dealer_handshake (
      application, "raw-error-reply", pair_id_, 1, 0,
      ZLINK_CORE_SOCKET_ROUTER));
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
      include_payload_ ? zlink::zmp_flag_more : 0,
      zlink::zmp_kind_error_reply, request_sequence, errno_part_,
      errno_part_size_);
    if (include_payload_) {
        static const unsigned char detail_a[] = {'d', 'e', 't', 'a', 'i', 'l'};
        static const unsigned char detail_b[] = {'c', 'o', 'n', 't', 'e', 'x', 't'};
        append_wire_frame (&completion_record, make_zmp_wire_frame (
          zlink::zmp_flag_more, zlink::zmp_kind_data, 0, detail_a,
          sizeof (detail_a)));
        append_wire_frame (&completion_record, make_zmp_wire_frame (
          0, zlink::zmp_kind_data, 0, detail_b, sizeof (detail_b)));
    }
    if (fail_payload_export_)
        zlink::socket_reqrep_internal::test_set_request_reply_allocation_failpoint (
          zlink::socket_reqrep_internal::request_reply_allocation_payload_export);
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
    if (expected_result_ == ZLINK_REQUEST_REJECTED)
        TEST_ASSERT_EQUAL_INT (
          EACCES, zlink::request_result_internal::to_errno (
                    request_completion.request_result));
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

void test_error_reply_completion_decodes_errno_and_hides_invalid_payloads ()
{
    unsigned char valid_errno[4];
    zlink::put_uint32 (valid_errno, EACCES);
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

void test_error_reply_payload_export_allocation_failure_is_payloadless ()
{
    unsigned char valid_errno[4];
    zlink::put_uint32 (valid_errno, EACCES);
    run_raw_error_reply_case (984, valid_errno, sizeof (valid_errno), true,
                              ZLINK_REQUEST_INTERNAL_ERROR, 0, true);
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
    RUN_ZMP_METADATA_TEST (
      test_owner_timeout_before_commit_leaves_no_stale_completion_child);
    RUN_ZMP_METADATA_TEST (test_raw_wire_peer_weight_uses_application_control_only);
    RUN_ZMP_METADATA_TEST (
      test_raw_wire_peer_weight_bypasses_application_limit_and_consumes_malformed);
    RUN_ZMP_METADATA_TEST (test_raw_wire_peer_weight_waits_for_exact_pair_readiness);
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
      test_stale_application_connection_cannot_complete_reconnected_request);
    RUN_ZMP_METADATA_TEST (
      test_completion_lane_rejects_data_and_request_kinds_without_completing_them);
    RUN_ZMP_METADATA_TEST (
      test_error_reply_completion_decodes_errno_and_hides_invalid_payloads);
    RUN_ZMP_METADATA_TEST (
      test_error_reply_payload_export_allocation_failure_is_payloadless);

#undef RUN_ZMP_METADATA_TEST

    return UNITY_END ();
}
