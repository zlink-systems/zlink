/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"

#include "protocol/wire.hpp"
#include "protocol/zmp_metadata.hpp"
#include "protocol/zmp_protocol.hpp"
#include "core/flow_state_frame.hpp"

#include "../../src/runtime/core/pipe.hpp"
#include "../../src/api/socket/part_helper_internal.hpp"
#include "../../src/api/socket/socket_request_reply_internal.hpp"
#include "../../src/runtime/sockets/common/socket_base.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <errno.h>
#include <limits.h>
#include <mutex>
#include <set>
#include <string>
#include <string.h>
#include <thread>
#include <vector>

#ifndef ZLINK_HAVE_WINDOWS
#include <sys/time.h>
#endif

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int contract_wait_ms = 5000;

struct reply_prefix_accounting_gate_t
{
    reply_prefix_accounting_gate_t () : entered (false), released (false) {}

    std::mutex sync;
    std::condition_variable changed;
    bool entered;
    bool released;
};

void hold_reply_after_first_physical_prefix (void *userdata_)
{
    reply_prefix_accounting_gate_t *const gate =
      static_cast<reply_prefix_accounting_gate_t *> (userdata_);
    std::unique_lock<std::mutex> lock (gate->sync);
    gate->entered = true;
    gate->changed.notify_all ();
    gate->changed.wait (lock, [gate] { return gate->released; });
}

bool wait_for_reply_prefix_gate (reply_prefix_accounting_gate_t *gate_)
{
    std::unique_lock<std::mutex> lock (gate_->sync);
    return gate_->changed.wait_for (
      lock, std::chrono::milliseconds (contract_wait_ms),
      [gate_] { return gate_->entered; });
}

void release_reply_prefix_gate (reply_prefix_accounting_gate_t *gate_)
{
    std::lock_guard<std::mutex> lock (gate_->sync);
    gate_->released = true;
    gate_->changed.notify_all ();
}

zlink::socket_base_t *as_socket (void *socket_)
{
    return as_socket_handle (socket_).socket;
}

bool resolve_ready_pair_identity (void *socket_,
                                  const unsigned char *peer_identity_,
                                  size_t peer_identity_size_,
                                  uint64_t *pair_id_out_,
                                  uint64_t *generation_out_)
{
    return zlink_test_wait_until (contract_wait_ms, [=] {
        (void) as_socket (socket_)->process_submit_commands ();
        bool ready = false;
        return as_socket (socket_)->test_pair_identity_for_peer (
                 peer_identity_, peer_identity_size_, pair_id_out_,
                 generation_out_, &ready)
               && ready;
    });
}

bool resolve_ready_pair (void *socket_, const char *peer_routing_id_,
                         uint64_t *pair_id_out_, uint64_t *generation_out_)
{
    return resolve_ready_pair_identity (
      socket_, reinterpret_cast<const unsigned char *> (peer_routing_id_),
      strlen (peer_routing_id_), pair_id_out_, generation_out_);
}

void assert_physical_pair_topology_by_id (void *socket_, uint64_t pair_id,
                                          uint64_t generation,
                                          bool router_router_)
{
    TEST_ASSERT_NOT_EQUAL (0, pair_id);
    TEST_ASSERT_NOT_EQUAL (0, generation);
    zlink::pipe_t *const application =
      as_socket (socket_)->test_pair_pipe (pair_id, generation, false);
    zlink::pipe_t *const completion =
      as_socket (socket_)->test_pair_pipe (pair_id, generation, true);
    TEST_ASSERT_NOT_NULL (application);
    TEST_ASSERT_EQUAL_INT (zlink::transport_lane_application,
                           application->get_transport_lane ());
    TEST_ASSERT_EQUAL_UINT (router_router_ ? 2u : 1u,
                            application->get_transport_lane_count ());
    if (router_router_) {
        TEST_ASSERT_NOT_NULL (completion);
        TEST_ASSERT_EQUAL_INT (zlink::transport_lane_completion,
                               completion->get_transport_lane ());
        TEST_ASSERT_EQUAL_UINT (2u, completion->get_transport_lane_count ());
    } else {
        TEST_ASSERT_NULL (completion);
    }
}

void assert_physical_pair_topology (void *socket_, const char *peer_routing_id_,
                                    bool router_router_)
{
    uint64_t pair_id = 0;
    uint64_t generation = 0;
    TEST_ASSERT_TRUE (resolve_ready_pair (socket_, peer_routing_id_, &pair_id,
                                          &generation));
    assert_physical_pair_topology_by_id (socket_, pair_id, generation,
                                         router_router_);
}

void assert_inproc_physical_pair_topology (void *bound_, void *connected_,
                                           bool router_router_)
{
    void *sockets[] = {bound_, connected_};
    const char *routing_ids[] = {"sl-transport-bound",
                                 "sl-transport-connected"};
    void *owner = NULL;
    uint64_t pair_id = 0;
    uint64_t generation = 0;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (contract_wait_ms);
    while (!owner && std::chrono::steady_clock::now () < deadline) {
        for (size_t socket_index = 0; socket_index != 2 && !owner;
             ++socket_index) {
            (void) as_socket (sockets[socket_index])->process_submit_commands ();
            for (size_t rid_index = 0; rid_index != 2; ++rid_index) {
                bool ready = false;
                if (as_socket (sockets[socket_index])
                      ->test_pair_identity_for_peer (
                        reinterpret_cast<const unsigned char *> (
                          routing_ids[rid_index]),
                        strlen (routing_ids[rid_index]), &pair_id, &generation,
                        &ready)
                    && ready) {
                    owner = sockets[socket_index];
                    break;
                }
            }
        }
        if (!owner)
            msleep (1);
    }
    TEST_ASSERT_NOT_NULL (owner);
    assert_physical_pair_topology_by_id (owner, pair_id, generation,
                                         router_router_);
}

bool should_run_case (const char *name_)
{
    const char *const selected = getenv ("ZLINK_TEST_CASE");
    return !selected || !*selected || strcmp (selected, name_) == 0;
}

void set_raw_recv_timeout (fd_t fd_, int timeout_ms_)
{
#if defined ZLINK_HAVE_WINDOWS
    DWORD timeout = static_cast<DWORD> (timeout_ms_);
    TEST_ASSERT_SUCCESS_RAW_ERRNO (
      setsockopt (fd_, SOL_SOCKET, SO_RCVTIMEO,
                  reinterpret_cast<const char *> (&timeout), sizeof (timeout)));
#else
    struct timeval timeout;
    timeout.tv_sec = timeout_ms_ / 1000;
    timeout.tv_usec = (timeout_ms_ % 1000) * 1000;
    TEST_ASSERT_SUCCESS_RAW_ERRNO (
      setsockopt (fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof (timeout)));
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

bool send_all (fd_t fd_, const unsigned char *data_, size_t size_)
{
    size_t offset = 0;
    while (offset != size_) {
#if defined ZLINK_HAVE_WINDOWS
        const int rc = send (fd_, reinterpret_cast<const char *> (data_ + offset),
                             static_cast<int> (size_ - offset), 0);
#else
        const ssize_t rc =
          send (fd_, data_ + offset, size_ - offset, MSG_NOSIGNAL);
#endif
        if (rc <= 0)
            return false;
        offset += static_cast<size_t> (rc);
    }
    return true;
}

bool recv_all (fd_t fd_, unsigned char *data_, size_t size_)
{
    size_t offset = 0;
    while (offset != size_) {
#if defined ZLINK_HAVE_WINDOWS
        const int rc = recv (fd_, reinterpret_cast<char *> (data_ + offset),
                             static_cast<int> (size_ - offset), 0);
#else
        const ssize_t rc = recv (fd_, data_ + offset, size_ - offset, 0);
#endif
        if (rc <= 0)
            return false;
        offset += static_cast<size_t> (rc);
    }
    return true;
}

struct wire_frame_t
{
    wire_frame_t () : flags (0), kind (0), sequence (0) {}

    unsigned char flags;
    unsigned char kind;
    uint64_t sequence;
    std::vector<unsigned char> body;
};

bool send_wire_frame (fd_t fd_, unsigned char flags_, unsigned char kind_,
                      uint64_t sequence_, const void *body_, size_t size_)
{
    const bool extended = zlink::zmp_is_request_reply_kind (kind_);
    const size_t header_size =
      extended ? zlink::zmp_request_reply_header_size : zlink::zmp_header_size;
    std::vector<unsigned char> frame (header_size + size_);
    frame[0] = zlink::zmp_magic;
    frame[1] = zlink::zmp_version;
    frame[2] = flags_;
    frame[3] = kind_;
    zlink::put_uint32 (&frame[4], static_cast<uint32_t> (size_));
    if (extended)
        zlink::put_uint64 (&frame[zlink::zmp_header_size], sequence_);
    if (size_ != 0)
        memcpy (&frame[header_size], body_, size_);
    return send_all (fd_, &frame[0], frame.size ());
}

bool read_wire_frame (fd_t fd_, wire_frame_t *out_)
{
    unsigned char header[zlink::zmp_header_size];
    if (!recv_all (fd_, header, sizeof (header)))
        return false;
    if (header[0] != zlink::zmp_magic || header[1] != zlink::zmp_version)
        return false;

    out_->flags = header[2];
    out_->kind = header[3];
    out_->sequence = 0;
    if (zlink::zmp_is_request_reply_kind (out_->kind)) {
        unsigned char sequence[zlink::zmp_request_sequence_size];
        if (!recv_all (fd_, sequence, sizeof (sequence)))
            return false;
        out_->sequence = zlink::get_uint64 (sequence);
    }

    const uint32_t size = zlink::get_uint32 (header + 4);
    if (size > 64u * 1024u)
        return false;
    out_->body.assign (size, 0);
    return size == 0 || recv_all (fd_, &out_->body[0], size);
}

bool read_control_eventually (fd_t fd_, unsigned char command_,
                              wire_frame_t *out_, int attempts_ = 16)
{
    for (int attempt = 0; attempt != attempts_; ++attempt) {
        wire_frame_t frame;
        if (!read_wire_frame (fd_, &frame))
            return false;
        if ((frame.flags & zlink::zmp_flag_control) != 0
            && !frame.body.empty () && frame.body[0] == command_) {
            if (out_)
                *out_ = frame;
            return true;
        }
    }
    return false;
}

bool send_hello (fd_t fd_, int socket_type_, const char *routing_id_)
{
    const size_t routing_id_size = routing_id_ ? strlen (routing_id_) : 0;
    if (routing_id_size > 255)
        return false;
    std::vector<unsigned char> body (3 + routing_id_size);
    body[0] = zlink::zmp_control_hello;
    body[1] = static_cast<unsigned char> (socket_type_);
    body[2] = static_cast<unsigned char> (routing_id_size);
    if (routing_id_size != 0)
        memcpy (&body[3], routing_id_, routing_id_size);
    return send_wire_frame (fd_, zlink::zmp_flag_control,
                            zlink::zmp_kind_data, 0, &body[0], body.size ());
}

const char *wire_socket_type_name (int socket_type_)
{
    switch (socket_type_) {
        case ZLINK_CORE_SOCKET_PAIR:
            return "PAIR";
        case ZLINK_CORE_SOCKET_PUB:
            return "PUB";
        case ZLINK_CORE_SOCKET_SUB:
            return "SUB";
        case ZLINK_CORE_SOCKET_DEALER:
            return "DEALER";
        case ZLINK_CORE_SOCKET_ROUTER:
            return "ROUTER";
        case ZLINK_CORE_SOCKET_XPUB:
            return "XPUB";
        case ZLINK_CORE_SOCKET_XSUB:
            return "XSUB";
        case ZLINK_CORE_SOCKET_STREAM:
            return "STREAM";
        default:
            return "";
    }
}

bool send_ready (fd_t fd_, int socket_type_, const char *routing_id_,
                 bool include_lane_count_, const void *lane_count_,
                 size_t lane_count_size_, bool include_lane_,
                 const void *lane_, size_t lane_size_)
{
    std::vector<unsigned char> body;
    body.push_back (zlink::zmp_control_ready);
    const char *const socket_type = wire_socket_type_name (socket_type_);
    zlink::zmp_metadata::append_property (
      body, "Socket-Type", socket_type, strlen (socket_type));
    if (socket_type_ == ZLINK_CORE_SOCKET_DEALER
        || socket_type_ == ZLINK_CORE_SOCKET_ROUTER) {
        const char *const rid = routing_id_ ? routing_id_ : "";
        zlink::zmp_metadata::append_property (body, "Routing-Id", rid,
                                              strlen (rid));
    }
    if (include_lane_count_)
        zlink::zmp_metadata::append_property (
          body, "Zlink-Lane-Count", lane_count_, lane_count_size_);
    if (include_lane_)
        zlink::zmp_metadata::append_property (body, "Zlink-Lane", lane_,
                                              lane_size_);
    return send_wire_frame (fd_, zlink::zmp_flag_control,
                            zlink::zmp_kind_data, 0, &body[0], body.size ());
}

bool send_valid_ready (fd_t fd_, int socket_type_, const char *routing_id_,
                       unsigned char lane_count_, unsigned char lane_)
{
    const bool paired = socket_type_ == ZLINK_CORE_SOCKET_DEALER
                        || socket_type_ == ZLINK_CORE_SOCKET_ROUTER;
    return send_ready (fd_, socket_type_, routing_id_, paired, &lane_count_,
                       paired ? 1 : 0, paired, &lane_, paired ? 1 : 0);
}

bool ready_properties (const wire_frame_t &ready_,
                       zlink::zmp_metadata::properties_t *properties_)
{
    if ((ready_.flags & zlink::zmp_flag_control) == 0
        || ready_.body.empty ()
        || ready_.body[0] != zlink::zmp_control_ready)
        return false;
    return zlink::zmp_metadata::parse (
             ready_.body.size () == 1 ? NULL : &ready_.body[1],
             ready_.body.size () - 1, *properties_)
           == 0;
}

bool property_byte (const zlink::zmp_metadata::properties_t &properties_,
                    const char *name_, unsigned char *value_)
{
    const zlink::zmp_metadata::properties_t::const_iterator it =
      properties_.find (name_);
    if (it == properties_.end () || it->second.size () != 1)
        return false;
    *value_ = static_cast<unsigned char> (it->second[0]);
    return true;
}

fd_t accept_and_exchange_hello (fd_t listener_, int peer_socket_type_,
                                const char *peer_routing_id_)
{
    fd_t connection = accept_eventually (listener_, contract_wait_ms);
    TEST_ASSERT_NOT_EQUAL (retired_fd, connection);
    set_raw_recv_timeout (connection, contract_wait_ms);
    wire_frame_t hello;
    TEST_ASSERT_TRUE (
      read_control_eventually (connection, zlink::zmp_control_hello, &hello));
    TEST_ASSERT_TRUE (
      send_hello (connection, peer_socket_type_, peer_routing_id_));
    return connection;
}

fd_t connect_raw_peer (const char *endpoint_, int peer_socket_type_,
                       const char *peer_routing_id_, unsigned char lane_count_,
                       unsigned char lane_, bool include_count_ = true)
{
    fd_t connection = connect_socket (endpoint_, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, connection);
    set_raw_recv_timeout (connection, contract_wait_ms);
    TEST_ASSERT_TRUE (
      send_hello (connection, peer_socket_type_, peer_routing_id_));
    TEST_ASSERT_TRUE (send_ready (
      connection, peer_socket_type_, peer_routing_id_, include_count_,
      &lane_count_, include_count_ ? 1 : 0, true, &lane_, 1));
    wire_frame_t ready;
    TEST_ASSERT_TRUE (
      read_control_eventually (connection, zlink::zmp_control_ready, &ready));
    return connection;
}

bool wait_for_raw_close (fd_t fd_, int timeout_ms_ = 3000)
{
    set_raw_recv_timeout (fd_, 100);
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        unsigned char byte = 0;
#if defined ZLINK_HAVE_WINDOWS
        const int rc = recv (fd_, reinterpret_cast<char *> (&byte), 1, 0);
        if (rc == 0 || (rc < 0 && WSAGetLastError () == WSAECONNRESET))
            return true;
#else
        const ssize_t rc = recv (fd_, &byte, 1, MSG_DONTWAIT);
        if (rc == 0 || (rc < 0 && errno == ECONNRESET))
            return true;
#endif
        msleep (10);
    }
    return false;
}

void configure_socket (void *socket_)
{
    const int zero = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
}

void set_routing_id (void *socket_, const char *routing_id_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (socket_, routing_id_, strlen (routing_id_)));
}

zlink_routing_id_t make_routing_id (const char *text_)
{
    zlink_routing_id_t routing_id;
    memset (&routing_id, 0, sizeof (routing_id));
    const size_t size = strlen (text_);
    TEST_ASSERT_TRUE (size <= sizeof (routing_id.data));
    routing_id.size = static_cast<uint8_t> (size);
    memcpy (routing_id.data, text_, size);
    return routing_id;
}

void init_part (zlink_msg_t *part_, const char *payload_)
{
    const size_t size = strlen (payload_);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (part_, size));
    if (size != 0)
        memcpy (zlink_msg_data (part_), payload_, size);
}

void init_sized_part (zlink_msg_t *part_, size_t size_, unsigned char fill_)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (part_, size_));
    if (size_ != 0)
        memset (zlink_msg_data (part_), fill_, size_);
}

void assert_consumed (zlink_msg_t *part_)
{
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (part_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (part_));
}

std::string part_text (zlink_msg_t *part_)
{
    return std::string (static_cast<const char *> (zlink_msg_data (part_)),
                        zlink_msg_size (part_));
}

struct received_router_part_t
{
    zlink_routing_id_t source_rid;
    zlink_reply_token_t reply_token;
    zlink_part_flag_t part_flag;
    std::string payload;
};

received_router_part_t receive_router_part (void *router_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (contract_wait_ms);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_reply_token_t token = 0;
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        zlink_part_flag_t part_flag = ZLINK_PART_FINAL;
        const zlink_recv_result_t result = zlink_router_recv_part (
          router_, &source_rid, &token, &part, &part_flag,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            TEST_ASSERT_NOT_NULL (source_rid);
            received_router_part_t received;
            received.source_rid = *source_rid;
            received.reply_token = token;
            received.part_flag = part_flag;
            received.payload = part_text (&part);
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
            return received;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for ROUTER part");
    return received_router_part_t ();
}

struct received_part_t
{
    zlink_part_flag_t part_flag;
    std::string payload;
};

received_part_t receive_part (void *socket_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (contract_wait_ms);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        zlink_part_flag_t part_flag = ZLINK_PART_FINAL;
        const zlink_recv_result_t result = zlink_recv_part (
          socket_, NULL, &part, &part_flag, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            received_part_t received;
            received.part_flag = part_flag;
            received.payload = part_text (&part);
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
            return received;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for socket part");
    return received_part_t ();
}

void init_empty_completion (zlink_completion_t *completion_)
{
    memset (completion_, 0, sizeof (*completion_));
    completion_->struct_size = sizeof (*completion_);
}

zlink_completion_t receive_completion (void *socket_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (contract_wait_ms);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_completion_t completion;
        init_empty_completion (&completion);
        const zlink_recv_result_t result = zlink_completion_recv (
          socket_, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK)
            return completion;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for completion");
    zlink_completion_t unreachable;
    init_empty_completion (&unreachable);
    return unreachable;
}

void assert_no_completion (void *socket_, int duration_ms_ = 50)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (duration_ms_);
    do {
        zlink_completion_t completion;
        init_empty_completion (&completion);
        const zlink_recv_result_t result = zlink_completion_recv (
          socket_, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            const int kind = completion.kind;
            const uint64_t completion_id = completion.completion_id;
            const int request_result = completion.request_result;
            const size_t reply_part_count = completion.reply_part_count;
            zlink_completion_close (&completion);
            char detail[192];
            snprintf (
              detail, sizeof (detail),
              "unexpected completion kind=%d id=%llu request_result=%d parts=%zu",
              kind, static_cast<unsigned long long> (completion_id),
              request_result, reply_part_count);
            TEST_FAIL_MESSAGE (detail);
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        msleep (1);
    } while (std::chrono::steady_clock::now () < deadline);
}

void assert_no_public_part (void *socket_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    zlink_part_flag_t flag = ZLINK_PART_FINAL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_recv_part (socket_, NULL, &part, &flag,
                       ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}

short wait_reusable_poller_events (void *poller_, void *socket_, int timeout_ms_)
{
    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    const int count =
      zlink_poller_wait (poller_, &event, 1, timeout_ms_, &error);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_TRUE (count == 0 || count == 1);
    if (count == 0)
        return 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLER_SOURCE_SOCKET, event.source_kind);
    TEST_ASSERT_EQUAL_PTR (socket_, event.socket);
    TEST_ASSERT_EQUAL_PTR (socket_, event.user_data);
    return event.events;
}

zlink_completion_id_t send_request (void *socket_, const zlink_routing_id_t *rid_,
                                    const char *payload_, uint32_t timeout_ms_)
{
    zlink_msg_t request;
    init_part (&request, payload_);
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (socket_, rid_, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                          ZLINK_PART_FINAL, timeout_ms_, NULL,
                          &completion_id));
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    assert_consumed (&request);
    return completion_id;
}

void send_data (void *socket_, const zlink_routing_id_t *rid_,
                const char *payload_, zlink_part_flag_t flag_)
{
    zlink_msg_t part;
    init_part (&part, payload_);
    const zlink_submit_result_t result =
      rid_ ? zlink_send_part_rid (socket_, rid_, &part,
                                  ZLINK_SEND_FLAGS_NONE, flag_, NULL, NULL)
           : zlink_send_part (socket_, &part, ZLINK_SEND_FLAGS_NONE, flag_,
                              NULL, NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    assert_consumed (&part);
}

void retry_send_data_eventually (void *socket_,
                                 const zlink_routing_id_t *rid_,
                                 const char *payload_)
{
    for (int attempt = 0; attempt != 200; ++attempt) {
        zlink_msg_t part;
        init_part (&part, payload_);
        const zlink_submit_result_t result =
          rid_ ? zlink_send_part_rid (socket_, rid_, &part,
                                      ZLINK_SEND_FLAGS_DONTWAIT,
                                      ZLINK_PART_FINAL, NULL, NULL)
               : zlink_send_part (socket_, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                                  ZLINK_PART_FINAL, NULL, NULL);
        assert_consumed (&part);
        if (result == ZLINK_SUBMIT_OK)
            return;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, result);
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("DATA did not recover after flow/HWM release");
}

void send_reply (void *router_, const received_router_part_t &request_,
                 const char *payload_)
{
    zlink_msg_t reply;
    init_part (&reply, payload_);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router_, &request_.source_rid, request_.reply_token,
                        &reply, ZLINK_PART_FINAL));
    assert_consumed (&reply);
}

void retry_reply_eventually (void *router_,
                             const received_router_part_t &request_,
                             const char *payload_)
{
    for (int attempt = 0; attempt != 200; ++attempt) {
        zlink_msg_t reply;
        init_part (&reply, payload_);
        const zlink_submit_result_t result = zlink_reply_part (
          router_, &request_.source_rid, request_.reply_token, &reply,
          ZLINK_PART_FINAL);
        assert_consumed (&reply);
        if (result == ZLINK_SUBMIT_OK)
            return;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, result);
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("reply did not recover after flow/HWM release");
}

zlink_monitor_status_t read_status (void *socket_)
{
    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    void *monitor = zlink_socket_monitor_open (socket_, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    zlink_monitor_status_t status;
    memset (&status, 0, sizeof (status));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_monitor_status (monitor, &status));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor));
    return status;
}

zlink_monitor_status_t read_monitor_status (void *monitor_)
{
    zlink_monitor_status_t status;
    memset (&status, 0, sizeof (status));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_monitor_status (monitor_, &status));
    return status;
}

zlink_auto_hwm_budget_snapshot_t read_budget_snapshot ()
{
    zlink_auto_hwm_budget_snapshot_t snapshot;
    memset (&snapshot, 0, sizeof (snapshot));
    snapshot.abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    snapshot.struct_size = sizeof (snapshot);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_ctx_get_auto_hwm_budget_snapshot (get_test_context (), &snapshot));
    return snapshot;
}

int read_budget_snapshot_unchecked (
  zlink_auto_hwm_budget_snapshot_t *snapshot_)
{
    memset (snapshot_, 0, sizeof (*snapshot_));
    snapshot_->abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    snapshot_->struct_size = sizeof (*snapshot_);
    return zlink_ctx_get_auto_hwm_budget_snapshot (get_test_context (),
                                                   snapshot_);
}

bool wait_for_probe_event (test_monitor_probe_t *probe_, uint64_t event_,
                           int start_, zlink_monitor_event_t *out_)
{
    int found = -1;
    if (!test_monitor_probe_wait_event_after (probe_, event_, start_,
                                              contract_wait_ms, &found))
        return false;
    if (out_)
        *out_ = test_monitor_probe_record_at (probe_, found);
    return true;
}

bool wait_for_ready_edge (test_monitor_probe_t *probe_, int start_,
                          zlink_monitor_event_t *out_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (contract_wait_ms);
    while (std::chrono::steady_clock::now () < deadline) {
        const int count = test_monitor_probe_count (probe_);
        for (int index = start_; index != count; ++index) {
            const zlink_monitor_event_t event =
              test_monitor_probe_record_at (probe_, index);
            if (event.event == ZLINK_EVENT_CONNECTION_READY
                && (event.flags
                    & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE)
                     != 0) {
                if (out_)
                    *out_ = event;
                return true;
            }
        }
        msleep (1);
    }
    return false;
}

int count_probe_events (test_monitor_probe_t *probe_, uint64_t event_)
{
    int count = 0;
    const int size = test_monitor_probe_count (probe_);
    for (int index = 0; index != size; ++index) {
        if (test_monitor_probe_event_at (probe_, index) == event_)
            ++count;
    }
    return count;
}

int count_ready_edges (test_monitor_probe_t *probe_)
{
    int count = 0;
    const int size = test_monitor_probe_count (probe_);
    for (int index = 0; index != size; ++index) {
        const zlink_monitor_event_t event =
          test_monitor_probe_record_at (probe_, index);
        if (event.event == ZLINK_EVENT_CONNECTION_READY
            && (event.flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE)
                 != 0)
            ++count;
    }
    return count;
}

bool wait_for_flow_paused_count (void *socket_, uint64_t expected_)
{
    return zlink_test_wait_until (contract_wait_ms, [=] {
        const zlink_monitor_status_t status = read_status (socket_);
        return status.flow_paused_connections == expected_;
    });
}

bool wait_for_monitor_flow_paused_count (void *monitor_, uint64_t expected_)
{
    return zlink_test_wait_until (contract_wait_ms, [=] {
        return read_monitor_status (monitor_).flow_paused_connections
               == expected_;
    });
}

struct dr_fixture_t
{
    dr_fixture_t (const char *endpoint_, bool tcp_ = false) :
        router (test_context_socket (ZLINK_SOCKET_ROUTER)),
        dealer (test_context_socket (ZLINK_SOCKET_DEALER)),
        endpoint (endpoint_),
        tcp (tcp_)
    {
        configure_socket (router);
        configure_socket (dealer);
        set_routing_id (router, "sl-router");
        set_routing_id (dealer, "sl-dealer");
    }

    void connect_and_prime ()
    {
        char tcp_endpoint[MAX_SOCKET_STRING];
        if (tcp) {
            bind_loopback_ipv4 (router, tcp_endpoint, sizeof (tcp_endpoint));
            endpoint = tcp_endpoint;
        } else {
            TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                                   zlink_bind (router, endpoint.c_str ()));
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                               zlink_connect (dealer, endpoint.c_str ()));
        send_data (dealer, NULL, "prime", ZLINK_PART_FINAL);
        const received_router_part_t prime = receive_router_part (router);
        TEST_ASSERT_EQUAL_UINT64 (0, prime.reply_token);
        TEST_ASSERT_EQUAL_STRING ("prime", prime.payload.c_str ());
        dealer_rid = prime.source_rid;
    }

    void close ()
    {
        dealer = test_context_socket_close_zero_linger (dealer);
        router = test_context_socket_close_zero_linger (router);
    }

    void *router;
    void *dealer;
    std::string endpoint;
    bool tcp;
    zlink_routing_id_t dealer_rid;
};

struct rr_fixture_t
{
    explicit rr_fixture_t (const char *endpoint_) :
        first (test_context_socket (ZLINK_SOCKET_ROUTER)),
        second (test_context_socket (ZLINK_SOCKET_ROUTER)),
        endpoint (endpoint_)
    {
        configure_socket (first);
        configure_socket (second);
        set_routing_id (first, "sl-router-a");
        set_routing_id (second, "sl-router-b");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_router_option (second,
                                   ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                   "sl-router-a", strlen ("sl-router-a")));
        first_rid = make_routing_id ("sl-router-a");
        second_rid = make_routing_id ("sl-router-b");
    }

    void connect_and_prime ()
    {
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                               zlink_bind (first, endpoint.c_str ()));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                               zlink_connect (second, endpoint.c_str ()));
        send_data (second, &first_rid, "prime-rr", ZLINK_PART_FINAL);
        const received_router_part_t prime = receive_router_part (first);
        TEST_ASSERT_EQUAL_STRING ("prime-rr", prime.payload.c_str ());
        TEST_ASSERT_EQUAL_UINT64 (0, prime.reply_token);
    }

    void close ()
    {
        second = test_context_socket_close_zero_linger (second);
        first = test_context_socket_close_zero_linger (first);
    }

    void *first;
    void *second;
    std::string endpoint;
    zlink_routing_id_t first_rid;
    zlink_routing_id_t second_rid;
};

void run_inproc_ready_order (int bind_type_, int connect_type_,
                             bool connect_first_, const char *endpoint_)
{
    void *bound = test_context_socket (bind_type_);
    void *connected = test_context_socket (connect_type_);
    configure_socket (bound);
    configure_socket (connected);
    set_routing_id (bound, "sl-inproc-bound");
    set_routing_id (connected, "sl-inproc-connected");
    if (connect_type_ == ZLINK_SOCKET_ROUTER) {
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_router_option (connected,
                                   ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                   "sl-inproc-bound",
                                   strlen ("sl-inproc-bound")));
    }

    test_monitor_probe_t bound_probe;
    test_monitor_probe_t connected_probe;
    void *bound_monitor = open_test_monitor_probe (
      bound, ZLINK_EVENT_CONNECTION_READY, &bound_probe);
    void *connected_monitor = open_test_monitor_probe (
      connected, ZLINK_EVENT_CONNECTION_READY, &connected_probe);
    if (connect_first_) {
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                               zlink_connect (connected, endpoint_));
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (bound, endpoint_));
    } else {
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (bound, endpoint_));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                               zlink_connect (connected, endpoint_));
    }

    zlink_monitor_event_t bound_ready;
    zlink_monitor_event_t connected_ready;
    TEST_ASSERT_TRUE (wait_for_ready_edge (&bound_probe, 0, &bound_ready));
    TEST_ASSERT_TRUE (
      wait_for_ready_edge (&connected_probe, 0, &connected_ready));
    TEST_ASSERT_EQUAL_UINT (
      ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION, bound_ready.transport_lane);
    TEST_ASSERT_EQUAL_UINT (
      ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
      connected_ready.transport_lane);
    TEST_ASSERT_EQUAL_UINT64 (1, bound_ready.value);
    TEST_ASSERT_EQUAL_UINT64 (1, connected_ready.value);
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (
      &bound_probe, test_monitor_probe_count (&bound_probe), 100));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (
      &connected_probe, test_monitor_probe_count (&connected_probe), 100));
    TEST_ASSERT_EQUAL_INT (1, count_ready_edges (&bound_probe));
    TEST_ASSERT_EQUAL_INT (1, count_ready_edges (&connected_probe));

    close_test_monitor_probe (&connected_monitor, &connected_probe);
    close_test_monitor_probe (&bound_monitor, &bound_probe);
    test_context_socket_close_zero_linger (connected);
    test_context_socket_close_zero_linger (bound);
}

struct transport_security_t
{
    transport_security_t () : enabled (false) {}

    bool enabled;
    tls_test_files_t files;
};

void configure_transport_security (void *bound_, void *connected_,
                                   const char *scheme_,
                                   transport_security_t *security_)
{
    if (strcmp (scheme_, "tls") != 0 && strcmp (scheme_, "wss") != 0)
        return;
#if defined ZLINK_HAVE_TLS
    security_->files = make_tls_test_files ();
    security_->enabled = true;
    const int trust_system = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (connected_, ZLINK_OPT_TLS_TRUST_SYSTEM,
                        &trust_system, sizeof (trust_system)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (bound_, ZLINK_OPT_TLS_CERT,
                        security_->files.server_cert.c_str (),
                        security_->files.server_cert.size ()));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (bound_, ZLINK_OPT_TLS_KEY,
                        security_->files.server_key.c_str (),
                        security_->files.server_key.size ()));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (connected_, ZLINK_OPT_TLS_CA,
                        security_->files.ca_cert.c_str (),
                        security_->files.ca_cert.size ()));
    const char hostname[] = "localhost";
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (connected_, ZLINK_OPT_TLS_HOSTNAME, hostname,
                        strlen (hostname)));
#else
    (void) bound_;
    (void) connected_;
    (void) security_;
    TEST_FAIL_MESSAGE ("secure transport compiled without TLS support");
#endif
}

void run_public_transport_pair (const char *scheme_, bool router_router_)
{
    const int socket_type =
      router_router_ ? ZLINK_SOCKET_ROUTER : ZLINK_SOCKET_DEALER;
    void *bound = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *connected = test_context_socket (socket_type);
    configure_socket (bound);
    configure_socket (connected);
    set_routing_id (bound, "sl-transport-bound");
    set_routing_id (connected, "sl-transport-connected");
    if (router_router_) {
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_router_option (connected,
                                   ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                   "sl-transport-bound",
                                   strlen ("sl-transport-bound")));
    }

    transport_security_t security;
    configure_transport_security (bound, connected, scheme_, &security);

    test_monitor_probe_t bound_probe;
    test_monitor_probe_t connected_probe;
    const zlink_socket_monitor_event_mask_t mask =
      ZLINK_EVENT_CONNECTED | ZLINK_EVENT_ACCEPTED
      | ZLINK_EVENT_CONNECTION_READY;
    void *bound_monitor = open_test_monitor_probe (bound, mask, &bound_probe);
    void *connected_monitor =
      open_test_monitor_probe (connected, mask, &connected_probe);
    zlink_auto_hwm_budget_snapshot_t inproc_baseline;
    memset (&inproc_baseline, 0, sizeof (inproc_baseline));
    if (strcmp (scheme_, "inproc") == 0) {
        // Socket close is synchronous at the API boundary, while its network
        // pipe endpoints retire on their owning I/O threads.  Fence the prior
        // matrix cell before using a context-wide registry snapshot as this
        // cell's baseline.
        TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
            zlink_auto_hwm_budget_snapshot_t snapshot;
            return read_budget_snapshot_unchecked (&snapshot)
                     == ZLINK_CONFIG_OK
                   && snapshot.active_directional_queue_count == 0
                   && snapshot.active_completion_directional_queue_count == 0;
        }));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_ctx_auto_hwm_recalculate (get_test_context ()));
        inproc_baseline = read_budget_snapshot ();
    }

    std::string bind_endpoint;
    if (strcmp (scheme_, "inproc") == 0) {
        bind_endpoint = router_router_
                          ? "inproc://sl-transport-matrix-rr"
                          : "inproc://sl-transport-matrix-dr";
    } else if (strcmp (scheme_, "ipc") == 0) {
        bind_endpoint = "ipc://" + make_random_ipc_path ();
        if (router_router_)
            bind_endpoint += "-rr";
    } else {
        bind_endpoint = std::string (scheme_) + "://127.0.0.1:*";
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (bound, bind_endpoint.c_str ()));

    char resolved[MAX_SOCKET_STRING];
    size_t resolved_size = sizeof (resolved);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_get_option (bound, ZLINK_OPT_LAST_ENDPOINT, resolved,
                        &resolved_size));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (connected, resolved));

    zlink_monitor_event_t bound_ready;
    zlink_monitor_event_t connected_ready;
    TEST_ASSERT_TRUE (wait_for_ready_edge (&bound_probe, 0, &bound_ready));
    TEST_ASSERT_TRUE (
      wait_for_ready_edge (&connected_probe, 0, &connected_ready));
    TEST_ASSERT_EQUAL_UINT (
      ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION, bound_ready.transport_lane);
    TEST_ASSERT_EQUAL_UINT (
      ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
      connected_ready.transport_lane);

    // Inproc has no CONNECTED/ACCEPTED physical events, so the context's
    // directional queue registry provides the exact physical topology: one
    // Application pipepair for D/R, plus one Completion pipepair only for R/R.
    if (strcmp (scheme_, "inproc") == 0) {
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_ctx_auto_hwm_recalculate (get_test_context ()));
        const zlink_auto_hwm_budget_snapshot_t active = read_budget_snapshot ();
        TEST_ASSERT_EQUAL_UINT64 (
          inproc_baseline.active_directional_queue_count + 2,
          active.active_directional_queue_count);
        TEST_ASSERT_EQUAL_UINT64 (
          inproc_baseline.active_completion_directional_queue_count
            + (router_router_ ? 2 : 0),
          active.active_completion_directional_queue_count);
    }

    if (strcmp (scheme_, "inproc") != 0) {
        const int expected = router_router_ ? 2 : 1;
        TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
            return count_probe_events (&connected_probe, ZLINK_EVENT_CONNECTED)
                     >= expected
                   && count_probe_events (&bound_probe, ZLINK_EVENT_ACCEPTED)
                        >= expected;
        }));
        TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (
          &connected_probe, test_monitor_probe_count (&connected_probe), 100));
        TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (
          &bound_probe, test_monitor_probe_count (&bound_probe), 100));
        TEST_ASSERT_EQUAL_INT (
          expected,
          count_probe_events (&connected_probe, ZLINK_EVENT_CONNECTED));
        TEST_ASSERT_EQUAL_INT (
          expected, count_probe_events (&bound_probe, ZLINK_EVENT_ACCEPTED));

        std::set<uint32_t> connected_lanes;
        std::set<uint32_t> accepted_lanes;
        const int connected_count = test_monitor_probe_count (&connected_probe);
        for (int index = 0; index != connected_count; ++index) {
            const zlink_monitor_event_t event =
              test_monitor_probe_record_at (&connected_probe, index);
            if (event.event == ZLINK_EVENT_CONNECTED)
                connected_lanes.insert (event.transport_lane);
        }
        const int bound_count = test_monitor_probe_count (&bound_probe);
        for (int index = 0; index != bound_count; ++index) {
            const zlink_monitor_event_t event =
              test_monitor_probe_record_at (&bound_probe, index);
            if (event.event == ZLINK_EVENT_ACCEPTED)
                accepted_lanes.insert (event.transport_lane);
        }
        TEST_ASSERT_TRUE (
          connected_lanes.find (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION)
          != connected_lanes.end ());
        TEST_ASSERT_TRUE (
          accepted_lanes.find (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION)
          != accepted_lanes.end ());
        if (router_router_) {
            TEST_ASSERT_TRUE (
              connected_lanes.find (ZLINK_MONITOR_TRANSPORT_LANE_COMPLETION)
              != connected_lanes.end ());
        }
        TEST_ASSERT_EQUAL_UINT64 (static_cast<size_t> (expected),
                                  connected_lanes.size ());
        // ACCEPTED is emitted before the passive engine has parsed peer READY
        // and therefore before it can know the negotiated lane. Its exact
        // event count still proves the physical listener topology; lane
        // identity is asserted on CONNECTED and post-handshake DISCONNECTED.
        TEST_ASSERT_EQUAL_UINT64 (1, accepted_lanes.size ());
    }

    close_test_monitor_probe (&connected_monitor, &connected_probe);
    close_test_monitor_probe (&bound_monitor, &bound_probe);
    test_context_socket_close_zero_linger (connected);
    test_context_socket_close_zero_linger (bound);
    if (security.enabled)
        cleanup_tls_test_files (security.files);
}

void test_sl_wire_dr_count_one_ready_metadata ()
{
    char endpoint[MAX_SOCKET_STRING];
    fd_t listener =
      bind_socket_resolve_port ("127.0.0.1", "0", endpoint);

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    configure_socket (dealer);
    set_routing_id (dealer, "sl-wire-dealer");
    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (
      dealer, ZLINK_EVENT_CONNECTED | ZLINK_EVENT_CONNECTION_READY, &probe);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));

    fd_t application = accept_and_exchange_hello (
      listener, ZLINK_CORE_SOCKET_ROUTER, "sl-raw-router");
    wire_frame_t ready;
    TEST_ASSERT_TRUE (
      read_control_eventually (application, zlink::zmp_control_ready, &ready));
    zlink::zmp_metadata::properties_t properties;
    TEST_ASSERT_TRUE (ready_properties (ready, &properties));
    unsigned char count = 0;
    unsigned char lane = 0xff;
    TEST_ASSERT_TRUE (property_byte (properties, "Zlink-Lane-Count", &count));
    TEST_ASSERT_TRUE (property_byte (properties, "Zlink-Lane", &lane));
    TEST_ASSERT_EQUAL_UINT8 (1, count);
    TEST_ASSERT_EQUAL_UINT8 (0, lane);
    TEST_ASSERT_FALSE (fd_readable (listener, 200));

    TEST_ASSERT_TRUE (send_valid_ready (
      application, ZLINK_CORE_SOCKET_ROUTER, "sl-raw-router", 1, 0));
    zlink_monitor_event_t ready_event;
    TEST_ASSERT_TRUE (wait_for_ready_edge (&probe, 0, &ready_event));
    TEST_ASSERT_EQUAL_UINT (
      ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION, ready_event.transport_lane);
    TEST_ASSERT_EQUAL_UINT64 (1, ready_event.value);
    TEST_ASSERT_EQUAL_INT (
      1, count_ready_edges (&probe));

    close (application);
    close (listener);
    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (dealer);
}

void test_sl_wire_rr_count_two_ready_fence ()
{
    char endpoint[MAX_SOCKET_STRING];
    fd_t listener =
      bind_socket_resolve_port ("127.0.0.1", "0", endpoint);

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    configure_socket (router);
    set_routing_id (router, "sl-wire-router");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                               "sl-raw-router", strlen ("sl-raw-router")));
    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (
      router, ZLINK_EVENT_CONNECTED | ZLINK_EVENT_CONNECTION_READY, &probe);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (router, endpoint));

    fd_t first = accept_and_exchange_hello (
      listener, ZLINK_CORE_SOCKET_ROUTER, "sl-raw-router");
    fd_t second = accept_and_exchange_hello (
      listener, ZLINK_CORE_SOCKET_ROUTER, "sl-raw-router");
    wire_frame_t first_ready;
    wire_frame_t second_ready;
    TEST_ASSERT_TRUE (
      read_control_eventually (first, zlink::zmp_control_ready, &first_ready));
    TEST_ASSERT_TRUE (read_control_eventually (
      second, zlink::zmp_control_ready, &second_ready));

    zlink::zmp_metadata::properties_t first_properties;
    zlink::zmp_metadata::properties_t second_properties;
    TEST_ASSERT_TRUE (ready_properties (first_ready, &first_properties));
    TEST_ASSERT_TRUE (ready_properties (second_ready, &second_properties));
    unsigned char first_count = 0;
    unsigned char second_count = 0;
    unsigned char first_lane = 0xff;
    unsigned char second_lane = 0xff;
    TEST_ASSERT_TRUE (property_byte (
      first_properties, "Zlink-Lane-Count", &first_count));
    TEST_ASSERT_TRUE (property_byte (
      second_properties, "Zlink-Lane-Count", &second_count));
    TEST_ASSERT_TRUE (
      property_byte (first_properties, "Zlink-Lane", &first_lane));
    TEST_ASSERT_TRUE (
      property_byte (second_properties, "Zlink-Lane", &second_lane));
    TEST_ASSERT_EQUAL_UINT8 (2, first_count);
    TEST_ASSERT_EQUAL_UINT8 (2, second_count);
    TEST_ASSERT_TRUE ((first_lane == 0 && second_lane == 1)
                      || (first_lane == 1 && second_lane == 0));

    TEST_ASSERT_TRUE (send_valid_ready (
      first, ZLINK_CORE_SOCKET_ROUTER, "sl-raw-router", 2, first_lane));
    // CONNECTED delivery for the second physical lane may still be queued in
    // the monitor worker. Only the logical ready edge is fenced by the second
    // READY, so do not treat that expected physical event as a failure.
    msleep (100);
    TEST_ASSERT_EQUAL_INT (
      0, count_ready_edges (&probe));
    TEST_ASSERT_TRUE (send_valid_ready (
      second, ZLINK_CORE_SOCKET_ROUTER, "sl-raw-router", 2, second_lane));
    zlink_monitor_event_t ready_event;
    TEST_ASSERT_TRUE (wait_for_ready_edge (&probe, 0, &ready_event));
    TEST_ASSERT_EQUAL_UINT64 (1, ready_event.value);
    TEST_ASSERT_EQUAL_UINT (
      ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION, ready_event.transport_lane);
    TEST_ASSERT_FALSE (fd_readable (listener, 200));

    close (second);
    close (first);
    close (listener);
    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (router);
}

void test_sl_wire_nonpaired_patterns_omit_lane_metadata ()
{
    struct pattern_t
    {
        int local_type;
        int peer_type;
    };
    const pattern_t patterns[] = {
      {ZLINK_SOCKET_PAIR, ZLINK_CORE_SOCKET_PAIR},
      {ZLINK_SOCKET_SUB, ZLINK_CORE_SOCKET_PUB},
      {ZLINK_SOCKET_XSUB, ZLINK_CORE_SOCKET_XPUB}};

    for (size_t index = 0; index != sizeof (patterns) / sizeof (patterns[0]);
         ++index) {
        char endpoint[MAX_SOCKET_STRING];
        fd_t listener =
          bind_socket_resolve_port ("127.0.0.1", "0", endpoint);
        void *socket = test_context_socket (patterns[index].local_type);
        configure_socket (socket);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                               zlink_connect (socket, endpoint));
        fd_t connection = accept_and_exchange_hello (
          listener, patterns[index].peer_type, NULL);
        wire_frame_t ready;
        TEST_ASSERT_TRUE (read_control_eventually (
          connection, zlink::zmp_control_ready, &ready));
        zlink::zmp_metadata::properties_t properties;
        TEST_ASSERT_TRUE (ready_properties (ready, &properties));
        TEST_ASSERT_TRUE (properties.find ("Zlink-Lane-Count")
                          == properties.end ());
        TEST_ASSERT_TRUE (properties.find ("Zlink-Lane")
                          == properties.end ());
        TEST_ASSERT_FALSE (fd_readable (listener, 100));
        const unsigned char unused = 0;
        TEST_ASSERT_TRUE (send_ready (
          connection, patterns[index].peer_type, NULL, false, &unused, 0,
          false, &unused, 0));
        close (connection);
        close (listener);
        test_context_socket_close_zero_linger (socket);
    }
}

void test_sl_wire_symmetric_direction_and_inproc_order ()
{
    run_inproc_ready_order (
      ZLINK_SOCKET_ROUTER, ZLINK_SOCKET_DEALER, false,
      "inproc://sl-owner-dr-bind-first");
    run_inproc_ready_order (
      ZLINK_SOCKET_DEALER, ZLINK_SOCKET_ROUTER, false,
      "inproc://sl-owner-rd-bind-first");
    run_inproc_ready_order (
      ZLINK_SOCKET_ROUTER, ZLINK_SOCKET_DEALER, true,
      "inproc://sl-owner-dr-connect-first");
    run_inproc_ready_order (
      ZLINK_SOCKET_ROUTER, ZLINK_SOCKET_ROUTER, true,
      "inproc://sl-owner-rr-connect-first");
}

void test_sl_wire_mandatory_lane_count_rejections ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    configure_socket (router);
    const int handshake_ivl = 500;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_HANDSHAKE_IVL, &handshake_ivl,
                        sizeof (handshake_ivl)));
    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (
      router, ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL
                | ZLINK_EVENT_DISCONNECTED
                | ZLINK_EVENT_CONNECTION_READY,
      &probe);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));

    struct invalid_ready_t
    {
        bool include_count;
        unsigned char count[2];
        size_t count_size;
        unsigned char lane;
    };
    const invalid_ready_t invalid[] = {
      {false, {1, 0}, 0, 0}, {true, {0, 0}, 0, 0},
      {true, {1, 0}, 2, 0},  {true, {0, 0}, 1, 0},
      {true, {3, 0}, 1, 0},  {true, {2, 0}, 1, 0},
      {true, {1, 0}, 1, 1}};

    for (size_t index = 0; index != sizeof (invalid) / sizeof (invalid[0]);
         ++index) {
        const int event_start = test_monitor_probe_count (&probe);
        fd_t peer = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
        TEST_ASSERT_NOT_EQUAL (retired_fd, peer);
        set_raw_recv_timeout (peer, contract_wait_ms);
        const std::string rid = "sl-invalid-" + std::to_string (index);
        TEST_ASSERT_TRUE (
          send_hello (peer, ZLINK_CORE_SOCKET_DEALER, rid.c_str ()));
        TEST_ASSERT_TRUE (send_ready (
          peer, ZLINK_CORE_SOCKET_DEALER, rid.c_str (),
          invalid[index].include_count, invalid[index].count,
          invalid[index].count_size, true, &invalid[index].lane, 1));
        TEST_ASSERT_TRUE (wait_for_raw_close (peer));
        int protocol_index = -1;
        TEST_ASSERT_TRUE (test_monitor_probe_wait_event_after (
          &probe, ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL, event_start,
          contract_wait_ms, &protocol_index));
        const zlink_monitor_event_t protocol_event =
          test_monitor_probe_record_at (&probe, protocol_index);
        TEST_ASSERT_EQUAL_HEX64 (
          ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_READY,
          protocol_event.value);
        int disconnected_index = -1;
        TEST_ASSERT_TRUE (test_monitor_probe_wait_event_after (
          &probe, ZLINK_EVENT_DISCONNECTED, protocol_index + 1,
          contract_wait_ms, &disconnected_index));
        const zlink_monitor_event_t disconnected_event =
          test_monitor_probe_record_at (&probe, disconnected_index);
        TEST_ASSERT_EQUAL_UINT64 (protocol_event.connection_id,
                                  disconnected_event.connection_id);
        close (peer);
    }

    const int duplicate_event_start = test_monitor_probe_count (&probe);
    fd_t duplicate_a = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    fd_t duplicate_b = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, duplicate_a);
    TEST_ASSERT_NOT_EQUAL (retired_fd, duplicate_b);
    set_raw_recv_timeout (duplicate_a, contract_wait_ms);
    set_raw_recv_timeout (duplicate_b, contract_wait_ms);
    const unsigned char two = 2;
    const unsigned char zero = 0;
    TEST_ASSERT_TRUE (
      send_hello (duplicate_a, ZLINK_CORE_SOCKET_ROUTER, "sl-duplicate"));
    TEST_ASSERT_TRUE (
      send_hello (duplicate_b, ZLINK_CORE_SOCKET_ROUTER, "sl-duplicate"));
    TEST_ASSERT_TRUE (send_ready (
      duplicate_a, ZLINK_CORE_SOCKET_ROUTER, "sl-duplicate", true, &two, 1,
      true, &zero, 1));
    TEST_ASSERT_TRUE (send_ready (
      duplicate_b, ZLINK_CORE_SOCKET_ROUTER, "sl-duplicate", true, &two, 1,
      true, &zero, 1));
    TEST_ASSERT_TRUE (wait_for_raw_close (duplicate_a));
    TEST_ASSERT_TRUE (wait_for_raw_close (duplicate_b));
    TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
        return count_probe_events (
                 &probe, ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL)
                 >= 9
               && count_probe_events (&probe, ZLINK_EVENT_DISCONNECTED) >= 9;
    }));
    int duplicate_disconnects = 0;
    const int duplicate_event_end = test_monitor_probe_count (&probe);
    for (int event_index = duplicate_event_start;
         event_index != duplicate_event_end; ++event_index) {
        const zlink_monitor_event_t disconnected_event =
          test_monitor_probe_record_at (&probe, event_index);
        if (disconnected_event.event != ZLINK_EVENT_DISCONNECTED)
            continue;
        TEST_ASSERT_NOT_EQUAL (0, disconnected_event.connection_id);
        bool matching_protocol = false;
        for (int prior = duplicate_event_start; prior != event_index; ++prior) {
            const zlink_monitor_event_t protocol_event =
              test_monitor_probe_record_at (&probe, prior);
            if (protocol_event.event
                  == ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL
                && protocol_event.connection_id
                     == disconnected_event.connection_id) {
                TEST_ASSERT_EQUAL_HEX64 (
                  ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_READY,
                  protocol_event.value);
                matching_protocol = true;
                break;
            }
        }
        TEST_ASSERT_TRUE (matching_protocol);
        ++duplicate_disconnects;
    }
    TEST_ASSERT_EQUAL_INT (2, duplicate_disconnects);
    close (duplicate_b);
    close (duplicate_a);

    const int missing_lane_event_start = test_monitor_probe_count (&probe);
    fd_t missing_lane = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, missing_lane);
    set_raw_recv_timeout (missing_lane, contract_wait_ms);
    TEST_ASSERT_TRUE (
      send_hello (missing_lane, ZLINK_CORE_SOCKET_ROUTER, "sl-missing-lane"));
    TEST_ASSERT_TRUE (send_ready (
      missing_lane, ZLINK_CORE_SOCKET_ROUTER, "sl-missing-lane", true, &two,
      1, true, &zero, 1));
    TEST_ASSERT_TRUE (wait_for_raw_close (missing_lane));
    int missing_lane_protocol_index = -1;
    TEST_ASSERT_TRUE (test_monitor_probe_wait_event_after (
      &probe, ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL,
      missing_lane_event_start, contract_wait_ms,
      &missing_lane_protocol_index));
    const zlink_monitor_event_t missing_lane_protocol_event =
      test_monitor_probe_record_at (&probe, missing_lane_protocol_index);
    TEST_ASSERT_EQUAL_HEX64 (
      ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_READY,
      missing_lane_protocol_event.value);
    int missing_lane_disconnected_index = -1;
    TEST_ASSERT_TRUE (test_monitor_probe_wait_event_after (
      &probe, ZLINK_EVENT_DISCONNECTED, missing_lane_protocol_index + 1,
      contract_wait_ms, &missing_lane_disconnected_index));
    const zlink_monitor_event_t missing_lane_disconnected_event =
      test_monitor_probe_record_at (&probe, missing_lane_disconnected_index);
    TEST_ASSERT_EQUAL_UINT64 (
      missing_lane_protocol_event.connection_id,
      missing_lane_disconnected_event.connection_id);
    close (missing_lane);

    TEST_ASSERT_EQUAL_INT (
      0, count_ready_edges (&probe));
    zlink_msg_t no_part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&no_part));
    const zlink_routing_id_t *rid = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t flag = ZLINK_PART_FINAL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_router_recv_part (router, &rid, &token, &no_part, &flag,
                              ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&no_part));

    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (router);
}

void test_sl_wire_old_peer_without_lane_count_rejected ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    configure_socket (router);
    const int handshake_ivl = 500;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_HANDSHAKE_IVL, &handshake_ivl,
                        sizeof (handshake_ivl)));
    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (
      router, ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL
                | ZLINK_EVENT_DISCONNECTED
                | ZLINK_EVENT_CONNECTION_READY,
      &probe);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));

    fd_t old_application = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    fd_t old_completion = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, old_application);
    TEST_ASSERT_NOT_EQUAL (retired_fd, old_completion);
    set_raw_recv_timeout (old_application, contract_wait_ms);
    set_raw_recv_timeout (old_completion, contract_wait_ms);
    const unsigned char ignored = 0;
    const unsigned char lane0 = 0;
    const unsigned char lane1 = 1;
    TEST_ASSERT_TRUE (
      send_hello (old_application, ZLINK_CORE_SOCKET_DEALER, "sl-old-peer"));
    TEST_ASSERT_TRUE (
      send_hello (old_completion, ZLINK_CORE_SOCKET_DEALER, "sl-old-peer"));
    TEST_ASSERT_TRUE (send_ready (
      old_application, ZLINK_CORE_SOCKET_DEALER, "sl-old-peer", false,
      &ignored, 0, true, &lane0, 1));
    TEST_ASSERT_TRUE (send_ready (
      old_completion, ZLINK_CORE_SOCKET_DEALER, "sl-old-peer", false,
      &ignored, 0, true, &lane1, 1));
    const char payload[] = "must-not-deliver";
    (void) send_wire_frame (old_application, 0, zlink::zmp_kind_data, 0,
                            payload, sizeof (payload) - 1);
    TEST_ASSERT_TRUE (wait_for_raw_close (old_application));
    TEST_ASSERT_TRUE (wait_for_raw_close (old_completion));
    TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
        return count_probe_events (
                 &probe, ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL)
                 >= 2
               && count_probe_events (&probe, ZLINK_EVENT_DISCONNECTED) >= 2;
    }));
    int old_peer_disconnects = 0;
    const int old_peer_event_end = test_monitor_probe_count (&probe);
    for (int event_index = 0; event_index != old_peer_event_end;
         ++event_index) {
        const zlink_monitor_event_t disconnected_event =
          test_monitor_probe_record_at (&probe, event_index);
        if (disconnected_event.event != ZLINK_EVENT_DISCONNECTED)
            continue;
        TEST_ASSERT_NOT_EQUAL (0, disconnected_event.connection_id);
        bool matching_protocol = false;
        for (int prior = 0; prior != event_index; ++prior) {
            const zlink_monitor_event_t protocol_event =
              test_monitor_probe_record_at (&probe, prior);
            if (protocol_event.event
                  == ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL
                && protocol_event.connection_id
                     == disconnected_event.connection_id) {
                TEST_ASSERT_EQUAL_HEX64 (
                  ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_READY,
                  protocol_event.value);
                matching_protocol = true;
                break;
            }
        }
        TEST_ASSERT_TRUE (matching_protocol);
        ++old_peer_disconnects;
    }
    TEST_ASSERT_EQUAL_INT (2, old_peer_disconnects);
    TEST_ASSERT_EQUAL_INT (
      0, count_ready_edges (&probe));

    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    const zlink_routing_id_t *rid = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t flag = ZLINK_PART_FINAL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_router_recv_part (router, &rid, &token, &part, &flag,
                              ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));

    close (old_completion);
    close (old_application);
    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (router);
}

void test_sl_wire_transport_matrix_counts ()
{
    run_public_transport_pair ("tcp", false);
    run_public_transport_pair ("tcp", true);
    run_public_transport_pair ("inproc", false);
    run_public_transport_pair ("inproc", true);
#if defined ZLINK_HAVE_IPC
    run_public_transport_pair ("ipc", false);
    run_public_transport_pair ("ipc", true);
#else
    TEST_MESSAGE ("IPC matrix cells skipped: ZLINK_HAVE_IPC is not defined");
#endif
#if defined ZLINK_HAVE_TLS
    run_public_transport_pair ("tls", false);
    run_public_transport_pair ("tls", true);
#else
    TEST_MESSAGE ("TLS matrix cells skipped: ZLINK_HAVE_TLS is not defined");
#endif
#if defined ZLINK_HAVE_WS
    run_public_transport_pair ("ws", false);
    run_public_transport_pair ("ws", true);
#else
    TEST_MESSAGE ("WS matrix cells skipped: ZLINK_HAVE_WS is not defined");
#endif
#if defined ZLINK_HAVE_WSS
    run_public_transport_pair ("wss", false);
    run_public_transport_pair ("wss", true);
#else
    TEST_MESSAGE ("WSS matrix cells skipped: ZLINK_HAVE_WSS is not defined");
#endif
}

zlink_submit_result_t try_send_sized_rid (
  void *socket_, const zlink_routing_id_t *rid_, size_t size_,
  zlink_completion_id_t *completion_id_ = NULL)
{
    zlink_msg_t part;
    init_sized_part (&part, size_, 'x');
    zlink_completion_id_t local_id = 0;
    const zlink_submit_result_t result = zlink_send_part_rid (
      socket_, rid_, &part, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
      NULL, completion_id_ ? completion_id_ : &local_id);
    assert_consumed (&part);
    return result;
}

zlink_submit_result_t try_send_sized_nowait (void *socket_, size_t size_)
{
    zlink_msg_t part;
    init_sized_part (&part, size_, 'h');
    const zlink_submit_result_t result = zlink_send_part (
      socket_, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL, NULL);
    assert_consumed (&part);
    return result;
}

zlink_submit_result_t try_send_sized_rid_nowait (
  void *socket_, const zlink_routing_id_t *rid_, size_t size_)
{
    zlink_msg_t part;
    init_sized_part (&part, size_, 'h');
    const zlink_submit_result_t result = zlink_send_part_rid (
      socket_, rid_, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL,
      NULL);
    assert_consumed (&part);
    return result;
}

void assert_request_completion (
  zlink_completion_t *completion_, zlink_completion_id_t expected_id_,
  zlink_request_result_t expected_result_, const char *reply_payload_ = NULL)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion_->kind);
    TEST_ASSERT_EQUAL_UINT64 (expected_id_, completion_->completion_id);
    TEST_ASSERT_EQUAL_INT (expected_result_, completion_->request_result);
    if (reply_payload_) {
        TEST_ASSERT_EQUAL_UINT64 (1, completion_->reply_part_count);
        TEST_ASSERT_NOT_NULL (completion_->reply_parts);
        TEST_ASSERT_EQUAL_STRING (
          reply_payload_, part_text (&completion_->reply_parts[0]).c_str ());
    } else {
        TEST_ASSERT_EQUAL_UINT64 (0, completion_->reply_part_count);
        TEST_ASSERT_NULL (completion_->reply_parts);
    }
    zlink_completion_close (completion_);
}

void test_sl_request_reply_at_head_completion_only ()
{
    dr_fixture_t fixture ("inproc://sl-request-reply-head");
    fixture.connect_and_prime ();
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, fixture.dealer, fixture.dealer,
                        ZLINK_POLLCOMPLETION));

    const zlink_completion_id_t request_id =
      send_request (fixture.dealer, NULL, "head-request", 2000);
    const received_router_part_t request = receive_router_part (fixture.router);
    TEST_ASSERT_NOT_EQUAL (0, request.reply_token);
    send_reply (fixture.router, request, "head-reply");

    const short events =
      wait_reusable_poller_events (poller, fixture.dealer, contract_wait_ms);
    TEST_ASSERT_TRUE ((events & ZLINK_POLLCOMPLETION) != 0);
    zlink_completion_t completion = receive_completion (fixture.dealer);
    assert_request_completion (&completion, request_id, ZLINK_REQUEST_OK,
                               "head-reply");
    assert_no_completion (fixture.dealer);
    assert_no_public_part (fixture.dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, fixture.dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    fixture.close ();
}

void test_sl_request_multipart_data_before_reply_fifo ()
{
    dr_fixture_t fixture ("inproc://sl-request-data-before-reply");
    fixture.connect_and_prime ();
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, fixture.dealer, fixture.dealer,
                        ZLINK_POLLCOMPLETION));
    const zlink_completion_id_t request_id =
      send_request (fixture.dealer, NULL, "fifo-request", 2000);
    const received_router_part_t request = receive_router_part (fixture.router);

    send_data (fixture.router, &request.source_rid, "data-a-prefix",
               ZLINK_PART_MORE);
    send_data (fixture.router, &request.source_rid, "data-a-final",
               ZLINK_PART_FINAL);
    send_reply (fixture.router, request, "fifo-reply");

    received_part_t first = receive_part (fixture.dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, first.part_flag);
    TEST_ASSERT_EQUAL_STRING ("data-a-prefix", first.payload.c_str ());
    zlink_pollitem_t input_item = {fixture.dealer, 0, ZLINK_POLLIN, 0};
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&input_item, 1, 0, NULL));
    TEST_ASSERT_TRUE ((input_item.revents & ZLINK_POLLIN) != 0);
    TEST_ASSERT_EQUAL_INT (
      0, wait_reusable_poller_events (poller, fixture.dealer, 0));
    assert_no_completion (fixture.dealer);

    received_part_t final = receive_part (fixture.dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, final.part_flag);
    TEST_ASSERT_EQUAL_STRING ("data-a-final", final.payload.c_str ());
    const short events =
      wait_reusable_poller_events (poller, fixture.dealer, contract_wait_ms);
    TEST_ASSERT_TRUE ((events & ZLINK_POLLCOMPLETION) != 0);
    zlink_completion_t completion = receive_completion (fixture.dealer);
    assert_request_completion (&completion, request_id, ZLINK_REQUEST_OK,
                               "fifo-reply");
    assert_no_public_part (fixture.dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, fixture.dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    fixture.close ();
}

void test_sl_request_reply_before_data_destinations ()
{
    dr_fixture_t fixture ("inproc://sl-request-reply-before-data");
    fixture.connect_and_prime ();
    const zlink_completion_id_t request_id =
      send_request (fixture.dealer, NULL, "reply-first-request", 2000);
    const received_router_part_t request = receive_router_part (fixture.router);
    send_reply (fixture.router, request, "reply-first");
    send_data (fixture.router, &request.source_rid, "data-after-reply",
               ZLINK_PART_FINAL);

    zlink_completion_t completion = receive_completion (fixture.dealer);
    assert_request_completion (&completion, request_id, ZLINK_REQUEST_OK,
                               "reply-first");
    const received_part_t data = receive_part (fixture.dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, data.part_flag);
    TEST_ASSERT_EQUAL_STRING ("data-after-reply", data.payload.c_str ());
    assert_no_completion (fixture.dealer);
    assert_no_public_part (fixture.dealer);
    fixture.close ();
}

void test_sl_request_timeout_wins_once_over_late_reply ()
{
    dr_fixture_t fixture ("inproc://sl-request-timeout-wins");
    const uint64_t hwm = 256;
    const uint64_t one_pending = 1;
    const int send_timeout = 25;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.router, ZLINK_OPT_SNDHWM, &hwm,
                        sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.dealer, ZLINK_OPT_RCVHWM, &hwm,
                        sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.router, ZLINK_OPT_PENDING_MAX_MSGS,
                        &one_pending, sizeof (one_pending)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.router, ZLINK_OPT_SNDTIMEO, &send_timeout,
                        sizeof (send_timeout)));
    fixture.connect_and_prime ();
    const zlink_completion_id_t request_id =
      send_request (fixture.dealer, NULL, "timeout-request", 100);
    const received_router_part_t request = receive_router_part (fixture.router);

    // Put an ordinary DATA record ahead of the REPLY, then hold the shared
    // D/R Application pipe under both the low HWM and remote PAUSED gates.
    send_data (fixture.router, &request.source_rid, "preceding-data",
               ZLINK_PART_FINAL);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.dealer,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (fixture.router, 1));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      try_send_sized_rid_nowait (fixture.router, &request.source_rid, 1024));

    zlink_msg_t blocked_reply;
    init_part (&blocked_reply, "late-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_reply_part (fixture.router, &request.source_rid,
                        request.reply_token, &blocked_reply,
                        ZLINK_PART_FINAL));
    assert_consumed (&blocked_reply);

    zlink_completion_t completion = receive_completion (fixture.dealer);
    assert_request_completion (&completion, request_id,
                               ZLINK_REQUEST_TIMED_OUT);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.dealer,
                                           ZLINK_RECEIVE_FLOW_RUNNING));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (fixture.router, 0));
    const received_part_t preceding = receive_part (fixture.dealer);
    TEST_ASSERT_EQUAL_STRING ("preceding-data", preceding.payload.c_str ());
    retry_reply_eventually (fixture.router, request, "late-reply");
    assert_no_completion (fixture.dealer, 100);
    assert_no_public_part (fixture.dealer);
    fixture.close ();
}

void test_sl_request_reply_submit_obeys_dr_backpressure ()
{
    dr_fixture_t fixture ("inproc://sl-request-reply-backpressure");
    const uint64_t hwm = 256;
    const uint64_t one_pending = 1;
    const int send_timeout = 25;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.router, ZLINK_OPT_SNDHWM, &hwm,
                        sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.dealer, ZLINK_OPT_RCVHWM, &hwm,
                        sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.router, ZLINK_OPT_PENDING_MAX_MSGS,
                        &one_pending, sizeof (one_pending)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.router, ZLINK_OPT_SNDTIMEO, &send_timeout,
                        sizeof (send_timeout)));
    fixture.connect_and_prime ();

    const zlink_completion_id_t request_id =
      send_request (fixture.dealer, NULL, "backpressure-request", 3000);
    const received_router_part_t request = receive_router_part (fixture.router);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.dealer,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (fixture.router, 1));

    size_t accepted = 0;
    bool reached_backpressure = false;
    for (size_t attempt = 0; attempt != 32; ++attempt) {
        zlink_completion_id_t completion_id = 0;
        const zlink_submit_result_t result = try_send_sized_rid (
          fixture.router, &request.source_rid, 1024, &completion_id);
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            reached_backpressure = true;
            break;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
        ++accepted;
    }
    TEST_ASSERT_TRUE (reached_backpressure);

    bool routed_send_sequence_still_active = false;
    int routed_send_sequence_family = -1;
    const std::shared_ptr<zlink::part_helper_internal::handle_state_t>
      helper_state = zlink::part_helper_internal::find_socket_state (
        as_socket (fixture.router));
    if (helper_state) {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        routed_send_sequence_still_active = helper_state->send.active;
        routed_send_sequence_family =
          static_cast<int> (helper_state->send.spec.family);
    }
    char routed_send_diagnostic[96];
    snprintf (routed_send_diagnostic, sizeof (routed_send_diagnostic),
              "failed FINAL retained send sequence family=%d",
              routed_send_sequence_family);
    TEST_ASSERT_FALSE_MESSAGE (routed_send_sequence_still_active,
                               routed_send_diagnostic);

    zlink_msg_t blocked_reply;
    init_part (&blocked_reply, "blocked-reply");
    const zlink_submit_result_t blocked_reply_result = zlink_reply_part (
      fixture.router, &request.source_rid, request.reply_token,
      &blocked_reply, ZLINK_PART_FINAL);
    const int blocked_reply_errno = zlink_errno ();
    char blocked_reply_diagnostic[96];
    snprintf (blocked_reply_diagnostic, sizeof (blocked_reply_diagnostic),
              "reply result=%d errno=%d rid_size=%u token=%llu",
              blocked_reply_result, blocked_reply_errno,
              static_cast<unsigned> (request.source_rid.size),
              static_cast<unsigned long long> (request.reply_token));
    TEST_ASSERT_EQUAL_INT_MESSAGE (ZLINK_SUBMIT_BACKPRESSURED,
                                   blocked_reply_result,
                                   blocked_reply_diagnostic);
    assert_consumed (&blocked_reply);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.dealer,
                                           ZLINK_RECEIVE_FLOW_RUNNING));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (fixture.router, 0));
    for (size_t index = 0; index != accepted; ++index) {
        const received_part_t drained = receive_part (fixture.dealer);
        TEST_ASSERT_EQUAL_UINT64 (1024, drained.payload.size ());
    }
    retry_reply_eventually (fixture.router, request, "retried-reply");
    zlink_completion_t completion = receive_completion (fixture.dealer);
    assert_request_completion (&completion, request_id, ZLINK_REQUEST_OK,
                               "retried-reply");
    fixture.close ();
}

void test_sl_request_rr_reply_isolated_from_application_pause ()
{
    rr_fixture_t fixture ("inproc://sl-request-rr-isolation");
    fixture.connect_and_prime ();
    const zlink_completion_id_t second_request_id = send_request (
      fixture.second, &fixture.first_rid, "request-second-to-first", 2000);
    const zlink_completion_id_t first_request_id = send_request (
      fixture.first, &fixture.second_rid, "request-first-to-second", 2000);
    const received_router_part_t request_at_first =
      receive_router_part (fixture.first);
    const received_router_part_t request_at_second =
      receive_router_part (fixture.second);
    TEST_ASSERT_NOT_EQUAL (0, request_at_first.reply_token);
    TEST_ASSERT_NOT_EQUAL (0, request_at_second.reply_token);
    TEST_ASSERT_EQUAL_STRING ("request-second-to-first",
                              request_at_first.payload.c_str ());
    TEST_ASSERT_EQUAL_STRING ("request-first-to-second",
                              request_at_second.payload.c_str ());

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.second,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (fixture.first, 1));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.first,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (fixture.second, 1));

    send_reply (fixture.first, request_at_first, "reply-first-to-second");
    send_reply (fixture.second, request_at_second, "reply-second-to-first");
    zlink_completion_t completion_at_second =
      receive_completion (fixture.second);
    assert_request_completion (&completion_at_second, second_request_id,
                               ZLINK_REQUEST_OK, "reply-first-to-second");
    zlink_completion_t completion_at_first = receive_completion (fixture.first);
    assert_request_completion (&completion_at_first, first_request_id,
                               ZLINK_REQUEST_OK, "reply-second-to-first");
    assert_no_completion (fixture.first);
    assert_no_completion (fixture.second);
    fixture.close ();
}

void test_sl_request_router_to_dealer_type_restriction ()
{
    dr_fixture_t fixture ("inproc://sl-request-type-restriction");
    fixture.connect_and_prime ();

    zlink_msg_t request;
    init_part (&request, "not-a-router-peer");
    zlink_completion_id_t completion_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_ADMITTED,
      zlink_request_part (fixture.router, &fixture.dealer_rid, &request,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 100, NULL,
                          &completion_id));
    TEST_ASSERT_EQUAL_INT (EPROTOTYPE, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    assert_consumed (&request);

    send_data (fixture.router, &fixture.dealer_rid, "ordinary-data",
               ZLINK_PART_FINAL);
    const received_part_t data = receive_part (fixture.dealer);
    TEST_ASSERT_EQUAL_STRING ("ordinary-data", data.payload.c_str ());
    fixture.close ();
}

void assert_no_router_part (void *router_)
{
    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t token = 0;
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    zlink_part_flag_t flag = ZLINK_PART_FINAL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_router_recv_part (router_, &source_rid, &token, &part, &flag,
                              ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, token);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}

bool decode_flow_control (const wire_frame_t &frame_, unsigned char *state_,
                          uint64_t *epoch_)
{
    if ((frame_.flags & zlink::zmp_flag_control) == 0
        || frame_.body.size () != zlink::flow_state::frame_size
        || memcmp (&frame_.body[0], zlink::flow_state::frame_name,
                   zlink::flow_state::frame_name_size)
             != 0)
        return false;
    if (frame_.body[zlink::flow_state::frame_name_size]
        != zlink::flow_state::frame_protocol_version)
        return false;
    *state_ = frame_.body[zlink::flow_state::frame_name_size + 1];
    *epoch_ = zlink::flow_state::get_uint64_be (
      &frame_.body[zlink::flow_state::frame_name_size + 2]);
    return true;
}

bool is_weight_control (const wire_frame_t &frame_, uint32_t *weight_)
{
    static const char name[] = "WEIGHT";
    if ((frame_.flags & zlink::zmp_flag_control) == 0
        || frame_.body.size () != sizeof (name) - 1 + 4
        || memcmp (&frame_.body[0], name, sizeof (name) - 1) != 0)
        return false;
    *weight_ = zlink::get_uint32 (&frame_.body[sizeof (name) - 1]);
    return true;
}

bool read_flow_control_eventually (fd_t fd_, unsigned char expected_state_,
                                   uint64_t *epoch_out_)
{
    for (int attempt = 0; attempt != 32; ++attempt) {
        wire_frame_t frame;
        if (!read_wire_frame (fd_, &frame))
            return false;
        unsigned char state = 0xff;
        uint64_t epoch = 0;
        if (!decode_flow_control (frame, &state, &epoch))
            continue;
        if (state == expected_state_) {
            if (epoch_out_)
                *epoch_out_ = epoch;
            return true;
        }
    }
    return false;
}

void send_raw_flow_control (fd_t fd_, unsigned char state_, uint64_t epoch_)
{
    std::vector<unsigned char> body (zlink::flow_state::frame_size);
    memcpy (&body[0], zlink::flow_state::frame_name,
            zlink::flow_state::frame_name_size);
    body[zlink::flow_state::frame_name_size] =
      zlink::flow_state::frame_protocol_version;
    body[zlink::flow_state::frame_name_size + 1] = state_;
    zlink::flow_state::put_uint64_be (
      &body[zlink::flow_state::frame_name_size + 2], epoch_);
    TEST_ASSERT_TRUE (send_wire_frame (
      fd_, zlink::zmp_flag_control, zlink::zmp_kind_data, 0, &body[0],
      body.size ()));
}

void test_sl_flow_dr_application_control_is_not_public_data ()
{
    dr_fixture_t fixture ("inproc://sl-flow-dr-application");
    fixture.connect_and_prime ();
    test_monitor_probe_t router_probe;
    test_monitor_probe_t dealer_probe;
    const zlink_socket_monitor_event_mask_t mask =
      ZLINK_EVENT_SEND_FLOW_PAUSED | ZLINK_EVENT_SEND_FLOW_RESUMED;
    void *router_monitor =
      open_test_monitor_probe (fixture.router, mask, &router_probe);
    void *dealer_monitor =
      open_test_monitor_probe (fixture.dealer, mask, &dealer_probe);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.dealer,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    zlink_monitor_event_t router_paused;
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &router_probe, ZLINK_EVENT_SEND_FLOW_PAUSED, 0, &router_paused));
    TEST_ASSERT_EQUAL_UINT (
      ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION, router_paused.transport_lane);
    assert_no_router_part (fixture.router);
    assert_no_public_part (fixture.dealer);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.dealer,
                                           ZLINK_RECEIVE_FLOW_RUNNING));
    zlink_monitor_event_t router_resumed;
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &router_probe, ZLINK_EVENT_SEND_FLOW_RESUMED, 0, &router_resumed));
    TEST_ASSERT_EQUAL_UINT64 (router_paused.connection_id,
                              router_resumed.connection_id);
    send_data (fixture.router, &fixture.dealer_rid, "router-after-running",
               ZLINK_PART_FINAL);
    TEST_ASSERT_EQUAL_STRING (
      "router-after-running", receive_part (fixture.dealer).payload.c_str ());

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    zlink_monitor_event_t dealer_paused;
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &dealer_probe, ZLINK_EVENT_SEND_FLOW_PAUSED, 0, &dealer_paused));
    TEST_ASSERT_EQUAL_UINT (
      ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION, dealer_paused.transport_lane);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router,
                                           ZLINK_RECEIVE_FLOW_RUNNING));
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &dealer_probe, ZLINK_EVENT_SEND_FLOW_RESUMED, 0, NULL));
    send_data (fixture.dealer, NULL, "dealer-after-running",
               ZLINK_PART_FINAL);
    TEST_ASSERT_EQUAL_STRING (
      "dealer-after-running",
      receive_router_part (fixture.router).payload.c_str ());

    close_test_monitor_probe (&dealer_monitor, &dealer_probe);
    close_test_monitor_probe (&router_monitor, &router_probe);
    fixture.close ();
}

void test_sl_flow_rr_completion_control_progresses_under_pause ()
{
    rr_fixture_t fixture ("inproc://sl-flow-rr-completion");
    const uint64_t hwm = 4096;
    const int no_wait = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.first, ZLINK_OPT_SNDHWM, &hwm,
                        sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.second, ZLINK_OPT_RCVHWM, &hwm,
                        sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.first, ZLINK_OPT_SNDTIMEO, &no_wait,
                        sizeof (no_wait)));
    fixture.connect_and_prime ();

    const zlink_completion_id_t request_id = send_request (
      fixture.second, &fixture.first_rid, "rr-flow-request", 2000);
    const received_router_part_t request = receive_router_part (fixture.first);

    size_t application_accepted = 0;
    for (; application_accepted != 256; ++application_accepted) {
        const zlink_submit_result_t result = try_send_sized_rid_nowait (
          fixture.first, &fixture.second_rid, 1024);
        if (result == ZLINK_SUBMIT_BACKPRESSURED)
            break;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_ASSERT_TRUE (application_accepted > 0 && application_accepted < 256);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.second,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (fixture.first, 1));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      try_send_sized_rid_nowait (fixture.first, &fixture.second_rid, 1024));
    send_reply (fixture.first, request, "rr-flow-reply");
    zlink_completion_t completion = receive_completion (fixture.second);
    assert_request_completion (&completion, request_id, ZLINK_REQUEST_OK,
                               "rr-flow-reply");

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.second,
                                           ZLINK_RECEIVE_FLOW_RUNNING));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (fixture.first, 0));
    // RUNNING crosses the Completion lane, but it does not erase the separate
    // Application HWM cause until the queued DATA is actually dequeued.
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      try_send_sized_rid_nowait (fixture.first, &fixture.second_rid, 1024));
    for (size_t index = 0; index != application_accepted; ++index) {
        const received_router_part_t drained = receive_router_part (fixture.second);
        TEST_ASSERT_EQUAL_UINT64 (1024, drained.payload.size ());
    }
    retry_send_data_eventually (fixture.first, &fixture.second_rid,
                                "rr-after-hwm-drain");
    TEST_ASSERT_EQUAL_STRING (
      "rr-after-hwm-drain",
      receive_router_part (fixture.second).payload.c_str ());
    fixture.close ();
}

void test_sl_flow_dr_normal_kinds_share_pause_and_hwm_gate ()
{
    dr_fixture_t fixture ("inproc://sl-flow-normal-kinds");
    const uint64_t hwm = 256;
    const uint64_t one_pending_request = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.dealer, ZLINK_OPT_SNDHWM, &hwm,
                        sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.router, ZLINK_OPT_RCVHWM, &hwm,
                        sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (fixture.dealer, ZLINK_OPT_PENDING_MAX_MSGS,
                        &one_pending_request,
                        sizeof (one_pending_request)));
    fixture.connect_and_prime ();

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (fixture.dealer, 1));
    bool data_blocked = false;
    size_t data_accepted = 0;
    int data_wait_context = 0x5d;
    zlink_completion_id_t data_wait_token = 0;
    for (int attempt = 0; attempt != 32; ++attempt) {
        zlink_msg_t data;
        init_sized_part (&data, 1024, 'd');
        zlink_completion_id_t completion_id = 0;
        const zlink_submit_result_t result = zlink_send_part (
          fixture.dealer, &data, ZLINK_SEND_FLAGS_DONTWAIT,
          ZLINK_PART_FINAL, &data_wait_context, &completion_id);
        assert_consumed (&data);
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            TEST_ASSERT_NOT_EQUAL (0, completion_id);
            data_wait_token = completion_id;
            data_blocked = true;
            break;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
        TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
        ++data_accepted;
    }
    TEST_ASSERT_TRUE (data_blocked);
    TEST_ASSERT_NOT_EQUAL (0, data_wait_token);
    zlink_completion_t no_completion;
    init_empty_completion (&no_completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (fixture.dealer, &no_completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    zlink_completion_close (&no_completion);

    zlink_msg_t request;
    init_part (&request, "request-shares-gate");
    int request_wait_context = 0x5e;
    zlink_completion_id_t request_wait_token = 0;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      zlink_request_part (fixture.dealer, NULL, &request,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 1000,
                          &request_wait_context, &request_wait_token));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_NOT_EQUAL (0, request_wait_token);
    TEST_ASSERT_NOT_EQUAL (data_wait_token, request_wait_token);
    assert_consumed (&request);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.router,
                                           ZLINK_RECEIVE_FLOW_RUNNING));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (fixture.dealer, 0));
    for (size_t index = 0; index != data_accepted; ++index) {
        const received_router_part_t drained =
          receive_router_part (fixture.router);
        TEST_ASSERT_EQUAL_UINT64 (0, drained.reply_token);
    }
    zlink_pollitem_t writable = {fixture.dealer, 0, ZLINK_POLLOUT, 0};
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1,
                           zlink_poll (&writable, 1, contract_wait_ms,
                                       &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLOUT, writable.revents);

    bool saw_data_wait = false;
    bool saw_request_wait = false;
    for (size_t index = 0; index != 2; ++index) {
        zlink_completion_t completion = receive_completion (fixture.dealer);
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion.kind);
        TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
        TEST_ASSERT_EQUAL_INT (0, completion.send_terminal_errno);
        if (completion.completion_id == data_wait_token) {
            TEST_ASSERT_EQUAL_PTR (&data_wait_context,
                                   completion.user_context);
            saw_data_wait = true;
        } else {
            TEST_ASSERT_EQUAL_UINT64 (request_wait_token,
                                      completion.completion_id);
            TEST_ASSERT_EQUAL_PTR (&request_wait_context,
                                   completion.user_context);
            saw_request_wait = true;
        }
        zlink_completion_close (&completion);
    }
    TEST_ASSERT_TRUE (saw_data_wait);
    TEST_ASSERT_TRUE (saw_request_wait);

    zlink_msg_t retried_request;
    init_part (&retried_request, "request-shares-gate");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (fixture.dealer, NULL, &retried_request,
                          ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 1000,
                          NULL, &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_consumed (&retried_request);
    const received_router_part_t admitted = receive_router_part (fixture.router);
    TEST_ASSERT_NOT_EQUAL (0, admitted.reply_token);
    send_reply (fixture.router, admitted, "normal-kinds-reply");
    zlink_completion_t completion = receive_completion (fixture.dealer);
    assert_request_completion (&completion, request_id, ZLINK_REQUEST_OK,
                               "normal-kinds-reply");

    zlink_msg_t retried_data;
    init_sized_part (&retried_data, 1024, 'd');
    zlink_completion_id_t retry_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (fixture.dealer, &retried_data,
                       ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, NULL,
                       &retry_id));
    TEST_ASSERT_EQUAL_UINT64 (0, retry_id);
    assert_consumed (&retried_data);
    const received_router_part_t retried = receive_router_part (fixture.router);
    TEST_ASSERT_EQUAL_UINT64 (0, retried.reply_token);
    TEST_ASSERT_EQUAL_UINT64 (1024, retried.payload.size ());
    assert_no_completion (fixture.dealer);
    fixture.close ();
}

void test_sl_flow_control_boundary_coalesces_latest_state ()
{
    char endpoint[MAX_SOCKET_STRING];
    fd_t listener =
      bind_socket_resolve_port ("127.0.0.1", "0", endpoint);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    configure_socket (dealer);
    set_routing_id (dealer, "sl-boundary-dealer");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    fd_t application = accept_and_exchange_hello (
      listener, ZLINK_CORE_SOCKET_ROUTER, "sl-boundary-router");
    wire_frame_t ready;
    TEST_ASSERT_TRUE (
      read_control_eventually (application, zlink::zmp_control_ready, &ready));
    TEST_ASSERT_TRUE (send_valid_ready (
      application, ZLINK_CORE_SOCKET_ROUTER, "sl-boundary-router", 1, 0));
    msleep (50);

    set_raw_recv_timeout (application, 50);
    while (fd_readable (application, 10)) {
        wire_frame_t ignored;
        if (!read_wire_frame (application, &ignored))
            break;
    }
    set_raw_recv_timeout (application, contract_wait_ms);

    zlink_msg_t prefix;
    init_part (&prefix, "multipart-prefix");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &prefix, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_MORE, NULL, NULL));
    assert_consumed (&prefix);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (dealer,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    const int weight = 37;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_dealer_option (dealer, ZLINK_DEALER_OPT_WEIGHT, &weight,
                               sizeof (weight)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (dealer,
                                           ZLINK_RECEIVE_FLOW_RUNNING));
    send_data (dealer, NULL, "multipart-final", ZLINK_PART_FINAL);

    bool saw_prefix = false;
    bool saw_final = false;
    bool saw_weight = false;
    bool saw_running = false;
    bool saw_paused = false;
    int weight_order = -1;
    int running_order = -1;
    for (int order = 0; order != 16 && !saw_running; ++order) {
        wire_frame_t frame;
        TEST_ASSERT_TRUE (read_wire_frame (application, &frame));
        if ((frame.flags & zlink::zmp_flag_control) == 0) {
            if (!saw_prefix) {
                saw_prefix = true;
                TEST_ASSERT_TRUE ((frame.flags & zlink::zmp_flag_more) != 0);
                TEST_ASSERT_EQUAL_UINT64 (strlen ("multipart-prefix"),
                                          frame.body.size ());
                TEST_ASSERT_EQUAL_MEMORY ("multipart-prefix", &frame.body[0],
                                          frame.body.size ());
            } else if (!saw_final) {
                saw_final = true;
                TEST_ASSERT_EQUAL_INT (0,
                                       frame.flags & zlink::zmp_flag_more);
                TEST_ASSERT_EQUAL_UINT64 (strlen ("multipart-final"),
                                          frame.body.size ());
                TEST_ASSERT_EQUAL_MEMORY ("multipart-final", &frame.body[0],
                                          frame.body.size ());
            }
            continue;
        }
        uint32_t observed_weight = 0;
        if (is_weight_control (frame, &observed_weight)) {
            if (observed_weight == static_cast<uint32_t> (weight)) {
                saw_weight = true;
                weight_order = order;
            }
            continue;
        }
        unsigned char state = 0xff;
        uint64_t epoch = 0;
        if (decode_flow_control (frame, &state, &epoch)) {
            TEST_ASSERT_NOT_EQUAL (0, epoch);
            if (state == zlink::flow_state::receive_flow_paused)
                saw_paused = true;
            if (state == zlink::flow_state::receive_flow_running) {
                saw_running = true;
                running_order = order;
            }
        }
    }
    close (application);
    close (listener);
    test_context_socket_close_zero_linger (dealer);

    TEST_ASSERT_TRUE (saw_prefix);
    TEST_ASSERT_TRUE (saw_final);
    TEST_ASSERT_TRUE (saw_weight);
    TEST_ASSERT_TRUE (saw_running);
    TEST_ASSERT_FALSE (saw_paused);
    TEST_ASSERT_TRUE (weight_order >= 0 && running_order > weight_order);

    // A transport that has exchanged HELLO but has not admitted peer READY is
    // still initial/inactive. FLOWSTATE and WEIGHT remain held until READY
    // makes the Application pipe current.
    char initial_endpoint[MAX_SOCKET_STRING];
    fd_t initial_listener =
      bind_socket_resolve_port ("127.0.0.1", "0", initial_endpoint);
    void *initial_dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    configure_socket (initial_dealer);
    set_routing_id (initial_dealer, "sl-boundary-initial-dealer");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK, zlink_connect (initial_dealer, initial_endpoint));
    fd_t initial_application = accept_and_exchange_hello (
      initial_listener, ZLINK_CORE_SOCKET_ROUTER,
      "sl-boundary-initial-router");
    wire_frame_t initial_ready;
    TEST_ASSERT_TRUE (read_control_eventually (
      initial_application, zlink::zmp_control_ready, &initial_ready));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (initial_dealer,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    const int initial_weight = 61;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_dealer_option (initial_dealer, ZLINK_DEALER_OPT_WEIGHT,
                               &initial_weight, sizeof (initial_weight)));
    TEST_ASSERT_FALSE (fd_readable (initial_application, 150));
    TEST_ASSERT_TRUE (send_valid_ready (
      initial_application, ZLINK_CORE_SOCKET_ROUTER,
      "sl-boundary-initial-router", 1, 0));
    bool initial_saw_pause = false;
    bool initial_saw_weight = false;
    for (int attempt = 0;
         attempt != 16 && (!initial_saw_pause || !initial_saw_weight);
         ++attempt) {
        wire_frame_t frame;
        TEST_ASSERT_TRUE (read_wire_frame (initial_application, &frame));
        unsigned char state = 0xff;
        uint64_t epoch = 0;
        uint32_t observed_weight = 0;
        if (decode_flow_control (frame, &state, &epoch)
            && state == zlink::flow_state::receive_flow_paused) {
            TEST_ASSERT_NOT_EQUAL (0, epoch);
            initial_saw_pause = true;
        } else if (is_weight_control (frame, &observed_weight)
                   && observed_weight
                        == static_cast<uint32_t> (initial_weight)) {
            initial_saw_weight = true;
        }
    }
    TEST_ASSERT_TRUE (initial_saw_pause);
    TEST_ASSERT_TRUE (initial_saw_weight);
    close (initial_application);
    close (initial_listener);
    test_context_socket_close_zero_linger (initial_dealer);

    // Normal DATA has filled the Application HWM and the same pipe is also
    // remote-PAUSED. Control updates must still reach the peer on that exact
    // lane without making ordinary DATA writable.
    dr_fixture_t blocked ("inproc://sl-flow-control-boundary-blocked");
    const uint64_t blocked_hwm =
      3u * (1024u + static_cast<uint64_t> (sizeof (zlink::msg_t)));
    const int no_wait = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (blocked.dealer, ZLINK_OPT_SNDHWM, &blocked_hwm,
                        sizeof (blocked_hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (blocked.router, ZLINK_OPT_RCVHWM, &blocked_hwm,
                        sizeof (blocked_hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (blocked.dealer, ZLINK_OPT_SNDTIMEO, &no_wait,
                        sizeof (no_wait)));
    blocked.connect_and_prime ();
    uint64_t blocked_pair_id = 0;
    uint64_t blocked_generation = 0;
    const uintptr_t blocked_router_instance =
      reinterpret_cast<uintptr_t> (as_socket (blocked.router));
    TEST_ASSERT_TRUE (resolve_ready_pair_identity (
      blocked.dealer,
      reinterpret_cast<const unsigned char *> (&blocked_router_instance),
      sizeof (blocked_router_instance), &blocked_pair_id, &blocked_generation));
    size_t blocked_accepted = 0;
    for (; blocked_accepted != 256; ++blocked_accepted) {
        const zlink_submit_result_t result =
          try_send_sized_nowait (blocked.dealer, 1024);
        if (result == ZLINK_SUBMIT_BACKPRESSURED)
            break;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_ASSERT_TRUE (blocked_accepted > 0 && blocked_accepted < 256);
    TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
        bool active = false;
        bool hwm_full = false;
        bool remote_paused = false;
        return as_socket (blocked.dealer)->test_application_pipe_flow_probe (
                 blocked_pair_id, blocked_generation, &active, &hwm_full,
                 &remote_paused)
               && !active && hwm_full && !remote_paused;
    }));

    test_monitor_probe_t blocked_probe;
    void *blocked_monitor = open_test_monitor_probe (
      blocked.router,
      ZLINK_EVENT_SEND_FLOW_PAUSED | ZLINK_EVENT_PEER_WEIGHT_CHANGED,
      &blocked_probe);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (blocked.router,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (blocked.dealer, 1));
    TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
        bool active = false;
        bool hwm_full = false;
        bool remote_paused = false;
        return as_socket (blocked.dealer)->test_application_pipe_flow_probe (
                 blocked_pair_id, blocked_generation, &active, &hwm_full,
                 &remote_paused)
               && !active && hwm_full && remote_paused;
    }));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (blocked.dealer,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    const int blocked_weight = 73;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_dealer_option (blocked.dealer, ZLINK_DEALER_OPT_WEIGHT,
                               &blocked_weight, sizeof (blocked_weight)));
    zlink_monitor_event_t blocked_flow;
    zlink_monitor_event_t blocked_weight_event;
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &blocked_probe, ZLINK_EVENT_SEND_FLOW_PAUSED, 0, &blocked_flow));
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &blocked_probe, ZLINK_EVENT_PEER_WEIGHT_CHANGED, 0,
      &blocked_weight_event));
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            blocked_flow.transport_lane);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            blocked_weight_event.transport_lane);
    TEST_ASSERT_EQUAL_UINT64 (blocked_flow.connection_id,
                              blocked_weight_event.connection_id);
    TEST_ASSERT_EQUAL_UINT64 (static_cast<uint64_t> (blocked_weight),
                              blocked_weight_event.value);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED,
                           try_send_sized_nowait (blocked.dealer, 1024));

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (blocked.router,
                                           ZLINK_RECEIVE_FLOW_RUNNING));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (blocked.dealer, 0));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (blocked.dealer,
                                           ZLINK_RECEIVE_FLOW_RUNNING));
    for (size_t index = 0; index != blocked_accepted; ++index)
        (void) receive_router_part (blocked.router);
    close_test_monitor_probe (&blocked_monitor, &blocked_probe);
    blocked.close ();
}

void test_sl_flow_stale_generation_is_fenced ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    configure_socket (dealer);
    set_routing_id (dealer, "sl-stale-local");
    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (
      dealer, ZLINK_EVENT_SEND_FLOW_PAUSED
                | ZLINK_EVENT_SEND_FLOW_RESUMED
                | ZLINK_EVENT_FLOW_STATE_STALE,
      &probe);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (dealer, endpoint, sizeof (endpoint));

    const zlink_routing_id_t peer_rid = make_routing_id ("sl-stale-peer");
    fd_t old_connection = connect_raw_peer (
      endpoint, ZLINK_CORE_SOCKET_ROUTER, "sl-stale-peer", 1, 0);
    uint64_t old_pair_id = 0;
    uint64_t old_generation = 0;
    TEST_ASSERT_TRUE (resolve_ready_pair (
      dealer, "sl-stale-peer", &old_pair_id, &old_generation));
    zlink::pipe_t *const retired_application =
      as_socket (dealer)->test_retain_application_pipe (old_pair_id,
                                                        old_generation);
    TEST_ASSERT_NOT_NULL (retired_application);

    send_raw_flow_control (old_connection,
                           zlink::flow_state::receive_flow_paused, 10);
    zlink_monitor_event_t paused;
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &probe, ZLINK_EVENT_SEND_FLOW_PAUSED, 0, &paused));
    const zlink_monitor_status_t before_current_stale =
      read_monitor_status (monitor);
    TEST_ASSERT_EQUAL_UINT64 (1,
                              before_current_stale.flow_paused_connections);

    // Duplicate and regressing epochs on the current connection are rejected
    // individually and retain that connection's public event identity.
    send_raw_flow_control (old_connection,
                           zlink::flow_state::receive_flow_paused, 10);
    send_raw_flow_control (old_connection,
                           zlink::flow_state::receive_flow_running, 9);
    const bool observed_both_current_stale =
      test_monitor_probe_wait_count (&probe, 3, contract_wait_ms);
    if (!observed_both_current_stale) {
        as_socket (dealer)->test_release_pipe (retired_application);
        close (old_connection);
        close_test_monitor_probe (&monitor, &probe);
        test_context_socket_close_zero_linger (dealer);
    }
    TEST_ASSERT_TRUE (observed_both_current_stale);
    TEST_ASSERT_TRUE (
      test_monitor_probe_wait_no_additional (&probe, 3, 100));
    for (int index = 1; index != 3; ++index) {
        const zlink_monitor_event_t stale =
          test_monitor_probe_record_at (&probe, index);
        TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_FLOW_STATE_STALE, stale.event);
        TEST_ASSERT_EQUAL_UINT32 (
          ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH,
          stale.flags & ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH);
        TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                                stale.transport_lane);
        TEST_ASSERT_EQUAL_UINT64 (paused.connection_id, stale.connection_id);
        TEST_ASSERT_EQUAL_UINT64 (index == 1 ? 10 : 9, stale.value);
    }
    const zlink_monitor_status_t after_current_stale =
      read_monitor_status (monitor);
    TEST_ASSERT_EQUAL_UINT64 (
      before_current_stale.flow_state_stale_total + 2,
      after_current_stale.flow_state_stale_total);
    TEST_ASSERT_EQUAL_UINT64 (1,
                              after_current_stale.flow_paused_connections);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect_rid (dealer, &peer_rid));
    TEST_ASSERT_TRUE (wait_for_raw_close (old_connection));
    close (old_connection);
    TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
        return as_socket (dealer)->test_pair_pipe (old_pair_id, old_generation,
                                                   false)
                 == NULL
               && read_monitor_status (monitor).flow_paused_connections == 0;
    }));

    fd_t current_connection = connect_raw_peer (
      endpoint, ZLINK_CORE_SOCKET_ROUTER, "sl-stale-peer", 1, 0);
    uint64_t current_pair_id = 0;
    uint64_t current_generation = 0;
    TEST_ASSERT_TRUE (resolve_ready_pair (
      dealer, "sl-stale-peer", &current_pair_id, &current_generation));
    TEST_ASSERT_TRUE (current_pair_id != old_pair_id
                      || current_generation != old_generation);

    const zlink_monitor_status_t before_retired = read_monitor_status (monitor);
    const int before_retired_events = test_monitor_probe_count (&probe);
    as_socket (dealer)->test_consume_late_flow_state_frame (
      retired_application, true, 100);
    TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
        return read_monitor_status (monitor).flow_state_stale_total
               == before_retired.flow_state_stale_total + 1;
    }));
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (
      &probe, before_retired_events, 100));
    TEST_ASSERT_EQUAL_UINT64 (0,
                              read_monitor_status (monitor).flow_paused_connections);

    send_data (dealer, NULL, "new-generation-data", ZLINK_PART_FINAL);
    wire_frame_t delivered;
    do {
        TEST_ASSERT_TRUE (read_wire_frame (current_connection, &delivered));
    } while ((delivered.flags & zlink::zmp_flag_control) != 0);
    TEST_ASSERT_EQUAL_UINT64 (strlen ("new-generation-data"),
                              delivered.body.size ());
    TEST_ASSERT_EQUAL_MEMORY ("new-generation-data", &delivered.body[0],
                              delivered.body.size ());

    as_socket (dealer)->test_release_pipe (retired_application);
    close (current_connection);
    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (dealer);
}

void test_sl_flow_reconnect_resynchronizes_absolute_state ()
{
    char endpoint[MAX_SOCKET_STRING];
    fd_t listener =
      bind_socket_resolve_port ("127.0.0.1", "0", endpoint);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    configure_socket (dealer);
    set_routing_id (dealer, "sl-resync-dealer");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));

    fd_t first = accept_and_exchange_hello (
      listener, ZLINK_CORE_SOCKET_ROUTER, "sl-resync-router");
    wire_frame_t first_ready;
    TEST_ASSERT_TRUE (read_control_eventually (
      first, zlink::zmp_control_ready, &first_ready));
    TEST_ASSERT_TRUE (send_valid_ready (
      first, ZLINK_CORE_SOCKET_ROUTER, "sl-resync-router", 1, 0));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (dealer,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    uint64_t first_epoch = 0;
    TEST_ASSERT_TRUE (read_flow_control_eventually (
      first, zlink::flow_state::receive_flow_paused, &first_epoch));
    TEST_ASSERT_NOT_EQUAL (0, first_epoch);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect (dealer, endpoint));
    TEST_ASSERT_TRUE (wait_for_raw_close (first));
    close (first);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    fd_t second = accept_and_exchange_hello (
      listener, ZLINK_CORE_SOCKET_ROUTER, "sl-resync-router");
    wire_frame_t second_ready;
    TEST_ASSERT_TRUE (read_control_eventually (
      second, zlink::zmp_control_ready, &second_ready));
    TEST_ASSERT_TRUE (send_valid_ready (
      second, ZLINK_CORE_SOCKET_ROUTER, "sl-resync-router", 1, 0));
    uint64_t second_epoch = 0;
    TEST_ASSERT_TRUE (read_flow_control_eventually (
      second, zlink::flow_state::receive_flow_paused, &second_epoch));
    TEST_ASSERT_NOT_EQUAL (0, second_epoch);
    TEST_ASSERT_FALSE (fd_readable (listener, 200));

    close (second);
    close (listener);
    test_context_socket_close_zero_linger (dealer);

    rr_fixture_t rr ("inproc://sl-flow-resync-rr");
    const int no_wait = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (rr.first, ZLINK_OPT_SNDTIMEO, &no_wait,
                        sizeof (no_wait)));
    rr.connect_and_prime ();
    const uintptr_t rr_second_instance =
      reinterpret_cast<uintptr_t> (as_socket (rr.second));
    test_monitor_probe_t rr_probe;
    void *rr_monitor = open_test_monitor_probe (
      rr.first, ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED,
      &rr_probe);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (rr.second,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (wait_for_monitor_flow_paused_count (rr_monitor, 1));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect (rr.second, rr.endpoint.c_str ()));
    TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
        // Explicit disconnect is asynchronous; drive the disconnecting
        // socket owner while observing teardown on the peer monitor.
        (void) as_socket (rr.second)->process_submit_commands ();
        const zlink_monitor_status_t status = read_monitor_status (rr_monitor);
        return as_socket (rr.first)->test_monitor_ready_count () == 0
               && status.flow_paused_connections == 0;
    }));
    const int reconnect_start = test_monitor_probe_count (&rr_probe);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (rr.second, rr.endpoint.c_str ()));
    zlink_monitor_event_t rr_ready;
    TEST_ASSERT_TRUE (
      wait_for_ready_edge (&rr_probe, reconnect_start, &rr_ready));
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            rr_ready.transport_lane);
    uint64_t rr_current_pair_id = 0;
    uint64_t rr_current_generation = 0;
    TEST_ASSERT_TRUE (resolve_ready_pair_identity (
      rr.first,
      reinterpret_cast<const unsigned char *> (&rr_second_instance),
      sizeof (rr_second_instance), &rr_current_pair_id,
      &rr_current_generation));
    assert_physical_pair_topology_by_id (
      rr.first, rr_current_pair_id, rr_current_generation, true);
    TEST_ASSERT_TRUE (wait_for_monitor_flow_paused_count (rr_monitor, 1));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      try_send_sized_rid_nowait (rr.first, &rr.second_rid, 1024));

    // A new request in the opposite Application direction remains usable,
    // and its reply progresses on the new Completion lane even though the
    // resynchronized Application lane toward rr.second is still PAUSED.
    const zlink_completion_id_t request_id = send_request (
      rr.second, &rr.first_rid, "rr-resync-request", 4000);
    const received_router_part_t request = receive_router_part (rr.first);
    send_reply (rr.first, request, "rr-resync-reply");
    zlink_completion_t completion = receive_completion (rr.second);
    assert_request_completion (&completion, request_id, ZLINK_REQUEST_OK,
                               "rr-resync-reply");

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (rr.second,
                                           ZLINK_RECEIVE_FLOW_RUNNING));
    TEST_ASSERT_TRUE (wait_for_monitor_flow_paused_count (rr_monitor, 0));
    send_data (rr.first, &rr.second_rid, "rr-after-resync-running",
               ZLINK_PART_FINAL);
    TEST_ASSERT_EQUAL_STRING (
      "rr-after-resync-running",
      receive_router_part (rr.second).payload.c_str ());
    close_test_monitor_probe (&rr_monitor, &rr_probe);
    rr.close ();
}

void test_sl_flow_snapshot_accounts_dr_reply_as_application ()
{
    dr_fixture_t fixture ("inproc://sl-flow-snapshot-accounting");
    fixture.connect_and_prime ();
    const zlink_completion_id_t request_id =
      send_request (fixture.dealer, NULL, "snapshot-request", 3000);
    const received_router_part_t request = receive_router_part (fixture.router);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.dealer,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (fixture.router, 1));
    const zlink_auto_hwm_budget_snapshot_t baseline = read_budget_snapshot ();

    zlink_msg_t prefix;
    init_sized_part (&prefix, 1024, 'p');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (fixture.router, &request.source_rid,
                        request.reply_token, &prefix, ZLINK_PART_MORE));
    assert_consumed (&prefix);
    // MORE is socket-local staging. Physical provisional accounting begins
    // only after FINAL selects the current route and moves the prefix.
    const zlink_auto_hwm_budget_snapshot_t staged =
      read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.core_queue_accounted_bytes, staged.core_queue_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.current_accounted_bytes, staged.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.provisional_accounted_bytes, staged.provisional_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (baseline.total_messaging_accounted_bytes,
                              staged.total_messaging_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_current_accounted_bytes,
      staged.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_peak_accounted_bytes,
      staged.completion_peak_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_pending_message_count,
      staged.completion_pending_message_count);
    TEST_ASSERT_EQUAL_UINT64 (baseline.active_directional_queue_count,
                              staged.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.active_completion_directional_queue_count,
      staged.active_completion_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (0, staged.application_accounted_bytes);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (fixture.dealer,
                                           ZLINK_RECEIVE_FLOW_RUNNING));
    TEST_ASSERT_TRUE (wait_for_flow_paused_count (fixture.router, 0));
    zlink_msg_t final;
    init_sized_part (&final, 1024, 'f');
    reply_prefix_accounting_gate_t prefix_gate;
    bool prefix_entered = false;
    int provisional_snapshot_rc = -1;
    zlink_auto_hwm_budget_snapshot_t provisional;
    memset (&provisional, 0, sizeof (provisional));
    zlink::socket_reqrep_internal::test_set_request_reply_write_after_prefix_hook (
      &hold_reply_after_first_physical_prefix, &prefix_gate);
    std::thread prefix_observer ([&] {
        prefix_entered = wait_for_reply_prefix_gate (&prefix_gate);
        if (prefix_entered)
            provisional_snapshot_rc =
              read_budget_snapshot_unchecked (&provisional);
        release_reply_prefix_gate (&prefix_gate);
    });
    const zlink_submit_result_t final_result = zlink_reply_part (
      fixture.router, &request.source_rid, request.reply_token, &final,
      ZLINK_PART_FINAL);
    prefix_observer.join ();
    zlink::socket_reqrep_internal::test_set_request_reply_write_after_prefix_hook (
      NULL, NULL);
    TEST_ASSERT_TRUE (prefix_entered);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, provisional_snapshot_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, final_result);
    assert_consumed (&final);
    TEST_ASSERT_TRUE (provisional.core_queue_accounted_bytes
                      > baseline.core_queue_accounted_bytes);
    TEST_ASSERT_TRUE (provisional.current_accounted_bytes
                      > baseline.current_accounted_bytes);
    TEST_ASSERT_TRUE (provisional.provisional_accounted_bytes
                      > baseline.provisional_accounted_bytes);
    TEST_ASSERT_TRUE (provisional.peak_accounted_bytes
                      >= provisional.current_accounted_bytes);
    TEST_ASSERT_TRUE (provisional.total_messaging_accounted_bytes
                      > baseline.total_messaging_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_current_accounted_bytes,
      provisional.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_pending_message_count,
      provisional.completion_pending_message_count);
    TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
        const zlink_auto_hwm_budget_snapshot_t snapshot =
          read_budget_snapshot ();
        return snapshot.current_accounted_bytes
                 > baseline.current_accounted_bytes
               && snapshot.core_queue_accounted_bytes
                    > baseline.core_queue_accounted_bytes;
    }));
    const zlink_auto_hwm_budget_snapshot_t queued = read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (
      queued.core_queue_accounted_bytes - baseline.core_queue_accounted_bytes,
      queued.current_accounted_bytes - baseline.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (baseline.provisional_accounted_bytes,
                              queued.provisional_accounted_bytes);
    TEST_ASSERT_TRUE (queued.peak_accounted_bytes
                      >= queued.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      queued.current_accounted_bytes - baseline.current_accounted_bytes,
      queued.total_messaging_accounted_bytes
        - baseline.total_messaging_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_current_accounted_bytes,
      queued.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_peak_accounted_bytes,
      queued.completion_peak_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.completion_pending_message_count,
      queued.completion_pending_message_count);
    TEST_ASSERT_EQUAL_UINT64 (baseline.active_directional_queue_count,
                              queued.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (
      baseline.active_completion_directional_queue_count,
      queued.active_completion_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (0, queued.application_accounted_bytes);

    zlink_completion_t completion = receive_completion (fixture.dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (2, completion.reply_part_count);
    TEST_ASSERT_EQUAL_UINT64 (1024,
                              zlink_msg_size (&completion.reply_parts[0]));
    TEST_ASSERT_EQUAL_UINT64 (1024,
                              zlink_msg_size (&completion.reply_parts[1]));
    zlink_completion_close (&completion);
    fixture.close ();

    // The same public reply transaction on R/R is accounted exclusively by
    // the physical Completion class: no Application/current/provisional field
    // moves, while Completion current/peak/pending and total messaging do.
    rr_fixture_t rr ("inproc://sl-flow-snapshot-accounting-rr");
    rr.connect_and_prime ();
    const zlink_completion_id_t rr_request_id = send_request (
      rr.first, &rr.second_rid, "snapshot-rr-request", 3000);
    const received_router_part_t rr_request = receive_router_part (rr.second);
    const zlink_auto_hwm_budget_snapshot_t rr_baseline = read_budget_snapshot ();
    TEST_ASSERT_TRUE (
      rr_baseline.active_completion_directional_queue_count > 0);

    zlink_msg_t rr_prefix;
    init_sized_part (&rr_prefix, 1024, 'c');
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (rr.second, &rr_request.source_rid,
                        rr_request.reply_token, &rr_prefix, ZLINK_PART_MORE));
    assert_consumed (&rr_prefix);
    const zlink_auto_hwm_budget_snapshot_t rr_staged =
      read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.core_queue_accounted_bytes,
                              rr_staged.core_queue_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.current_accounted_bytes,
                              rr_staged.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.provisional_accounted_bytes,
                              rr_staged.provisional_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.completion_current_accounted_bytes,
      rr_staged.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.completion_peak_accounted_bytes,
      rr_staged.completion_peak_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.completion_pending_message_count,
      rr_staged.completion_pending_message_count);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.total_messaging_accounted_bytes,
      rr_staged.total_messaging_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.active_directional_queue_count,
                              rr_staged.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.active_completion_directional_queue_count,
      rr_staged.active_completion_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (0, rr_staged.application_accounted_bytes);

    zlink_msg_t rr_final;
    init_sized_part (&rr_final, 1024, 'd');
    reply_prefix_accounting_gate_t rr_prefix_gate;
    bool rr_prefix_entered = false;
    int rr_provisional_snapshot_rc = -1;
    zlink_auto_hwm_budget_snapshot_t rr_provisional;
    memset (&rr_provisional, 0, sizeof (rr_provisional));
    zlink::socket_reqrep_internal::test_set_request_reply_write_after_prefix_hook (
      &hold_reply_after_first_physical_prefix, &rr_prefix_gate);
    std::thread rr_prefix_observer ([&] {
        rr_prefix_entered = wait_for_reply_prefix_gate (&rr_prefix_gate);
        if (rr_prefix_entered)
            rr_provisional_snapshot_rc =
              read_budget_snapshot_unchecked (&rr_provisional);
        release_reply_prefix_gate (&rr_prefix_gate);
    });
    const zlink_submit_result_t rr_final_result = zlink_reply_part (
      rr.second, &rr_request.source_rid, rr_request.reply_token, &rr_final,
      ZLINK_PART_FINAL);
    rr_prefix_observer.join ();
    zlink::socket_reqrep_internal::test_set_request_reply_write_after_prefix_hook (
      NULL, NULL);
    TEST_ASSERT_TRUE (rr_prefix_entered);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, rr_provisional_snapshot_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, rr_final_result);
    assert_consumed (&rr_final);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.core_queue_accounted_bytes,
                              rr_provisional.core_queue_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.current_accounted_bytes,
                              rr_provisional.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.provisional_accounted_bytes,
                              rr_provisional.provisional_accounted_bytes);
    TEST_ASSERT_TRUE (
      rr_provisional.completion_current_accounted_bytes
      > rr_baseline.completion_current_accounted_bytes);
    TEST_ASSERT_TRUE (rr_provisional.completion_peak_accounted_bytes
                      >= rr_provisional.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.completion_pending_message_count,
      rr_provisional.completion_pending_message_count);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_provisional.completion_current_accounted_bytes
        - rr_baseline.completion_current_accounted_bytes,
      rr_provisional.total_messaging_accounted_bytes
        - rr_baseline.total_messaging_accounted_bytes);
    TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
        const zlink_auto_hwm_budget_snapshot_t snapshot =
          read_budget_snapshot ();
        return snapshot.completion_pending_message_count
                 > rr_baseline.completion_pending_message_count
               && snapshot.completion_current_accounted_bytes
                    > rr_baseline.completion_current_accounted_bytes;
    }));
    const zlink_auto_hwm_budget_snapshot_t rr_queued = read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.core_queue_accounted_bytes,
                              rr_queued.core_queue_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.current_accounted_bytes,
                              rr_queued.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.provisional_accounted_bytes,
                              rr_queued.provisional_accounted_bytes);
    TEST_ASSERT_TRUE (rr_queued.completion_current_accounted_bytes
                      > rr_baseline.completion_current_accounted_bytes);
    TEST_ASSERT_TRUE (rr_queued.completion_peak_accounted_bytes
                      >= rr_queued.completion_current_accounted_bytes);
    TEST_ASSERT_TRUE (rr_queued.completion_pending_message_count
                      > rr_baseline.completion_pending_message_count);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_queued.completion_current_accounted_bytes
        - rr_baseline.completion_current_accounted_bytes,
      rr_queued.total_messaging_accounted_bytes
        - rr_baseline.total_messaging_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (rr_baseline.active_directional_queue_count,
                              rr_queued.active_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (
      rr_baseline.active_completion_directional_queue_count,
      rr_queued.active_completion_directional_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (0, rr_queued.application_accounted_bytes);

    zlink_completion_t rr_completion = receive_completion (rr.first);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, rr_completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (rr_request_id, rr_completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, rr_completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (2, rr_completion.reply_part_count);
    TEST_ASSERT_EQUAL_UINT64 (1024,
                              zlink_msg_size (&rr_completion.reply_parts[0]));
    TEST_ASSERT_EQUAL_UINT64 (1024,
                              zlink_msg_size (&rr_completion.reply_parts[1]));
    zlink_completion_close (&rr_completion);
    rr.close ();
}

void test_sl_monitor_lane_values_follow_topology ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    configure_socket (router);
    configure_socket (dealer);
    set_routing_id (router, "sl-lane-router");
    set_routing_id (dealer, "sl-lane-dealer");
    const zlink_socket_monitor_event_mask_t mask =
      ZLINK_EVENT_CONNECTED | ZLINK_EVENT_CONNECTION_READY
      | ZLINK_EVENT_SEND_FLOW_PAUSED | ZLINK_EVENT_SEND_FLOW_RESUMED
      | ZLINK_EVENT_PEER_WEIGHT_CHANGED | ZLINK_EVENT_DISCONNECTED;
    test_monitor_probe_t router_probe;
    test_monitor_probe_t dealer_probe;
    void *router_monitor = open_test_monitor_probe (router, mask, &router_probe);
    void *dealer_monitor = open_test_monitor_probe (dealer, mask, &dealer_probe);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    TEST_ASSERT_TRUE (wait_for_ready_edge (&dealer_probe, 0, NULL));
    send_data (dealer, NULL, "lane-prime", ZLINK_PART_FINAL);
    const received_router_part_t prime = receive_router_part (router);
    TEST_ASSERT_EQUAL_UINT64 (0, prime.reply_token);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (dealer,
                                           ZLINK_RECEIVE_FLOW_PAUSED));
    zlink_monitor_event_t flow;
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &router_probe, ZLINK_EVENT_SEND_FLOW_PAUSED, 0, &flow));
    TEST_ASSERT_EQUAL_UINT (
      ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION, flow.transport_lane);
    TEST_ASSERT_NOT_EQUAL (0, flow.connection_id);
    const int weight = 47;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_dealer_option (dealer, ZLINK_DEALER_OPT_WEIGHT, &weight,
                               sizeof (weight)));
    zlink_monitor_event_t weight_event;
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &router_probe, ZLINK_EVENT_PEER_WEIGHT_CHANGED, 0, &weight_event));
    TEST_ASSERT_EQUAL_UINT64 (static_cast<uint64_t> (weight),
                              weight_event.value);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            weight_event.transport_lane);
    TEST_ASSERT_EQUAL_UINT64 (flow.connection_id, weight_event.connection_id);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_socket_set_receive_flow_state (dealer,
                                           ZLINK_RECEIVE_FLOW_RUNNING));
    zlink_monitor_event_t resumed;
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &router_probe, ZLINK_EVENT_SEND_FLOW_RESUMED, 0, &resumed));
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            resumed.transport_lane);
    TEST_ASSERT_EQUAL_UINT64 (flow.connection_id, resumed.connection_id);

    const int router_disconnect_start = test_monitor_probe_count (&router_probe);
    const int dealer_disconnect_start = test_monitor_probe_count (&dealer_probe);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect (dealer, endpoint));
    zlink_monitor_event_t router_disconnected;
    zlink_monitor_event_t dealer_disconnected;
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &router_probe, ZLINK_EVENT_DISCONNECTED, router_disconnect_start,
      &router_disconnected));
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &dealer_probe, ZLINK_EVENT_DISCONNECTED, dealer_disconnect_start,
      &dealer_disconnected));
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            router_disconnected.transport_lane);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            dealer_disconnected.transport_lane);
    TEST_ASSERT_EQUAL_INT (
      1, count_probe_events (&router_probe, ZLINK_EVENT_DISCONNECTED));
    TEST_ASSERT_EQUAL_INT (
      1, count_probe_events (&dealer_probe, ZLINK_EVENT_DISCONNECTED));

    test_monitor_probe_t *const probes[] = {&router_probe, &dealer_probe};
    for (size_t probe_index = 0; probe_index != 2; ++probe_index) {
        const int event_count = test_monitor_probe_count (probes[probe_index]);
        for (int index = 0; index != event_count; ++index) {
            const zlink_monitor_event_t event =
              test_monitor_probe_record_at (probes[probe_index], index);
            if (event.event == ZLINK_EVENT_CONNECTED
                || event.event == ZLINK_EVENT_CONNECTION_READY
                || event.event == ZLINK_EVENT_PEER_WEIGHT_CHANGED
                || event.event == ZLINK_EVENT_SEND_FLOW_PAUSED
                || event.event == ZLINK_EVENT_SEND_FLOW_RESUMED
                || event.event == ZLINK_EVENT_DISCONNECTED)
                TEST_ASSERT_EQUAL_UINT (
                  ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                  event.transport_lane);
        }
    }

    close_test_monitor_probe (&dealer_monitor, &dealer_probe);
    close_test_monitor_probe (&router_monitor, &router_probe);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);

    // A ROUTER-ROUTER network pair is the only paired topology that exposes a
    // physical Completion lane in CONNECTED events.
    run_public_transport_pair ("tcp", true);
}

void test_sl_monitor_logical_ready_once_per_peer ()
{
    run_inproc_ready_order (
      ZLINK_SOCKET_ROUTER, ZLINK_SOCKET_DEALER, false,
      "inproc://sl-logical-ready-dr");
    run_inproc_ready_order (
      ZLINK_SOCKET_ROUTER, ZLINK_SOCKET_ROUTER, false,
      "inproc://sl-logical-ready-rr");
}

void test_sl_monitor_polling_separates_data_and_completion ()
{
    dr_fixture_t fixture ("inproc://sl-monitor-polling-split");
    fixture.connect_and_prime ();
    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, fixture.dealer, fixture.dealer,
                        ZLINK_POLLCOMPLETION));
    const zlink_completion_id_t request_id =
      send_request (fixture.dealer, NULL, "poll-request", 2000);
    const received_router_part_t request = receive_router_part (fixture.router);
    send_data (fixture.router, &request.source_rid, "poll-prefix",
               ZLINK_PART_MORE);
    send_data (fixture.router, &request.source_rid, "poll-final",
               ZLINK_PART_FINAL);
    send_reply (fixture.router, request, "poll-reply");

    const received_part_t prefix = receive_part (fixture.dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, prefix.part_flag);
    zlink_pollitem_t input_item = {fixture.dealer, 0, ZLINK_POLLIN, 0};
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&input_item, 1, 0, NULL));
    TEST_ASSERT_TRUE ((input_item.revents & ZLINK_POLLIN) != 0);
    TEST_ASSERT_EQUAL_INT (
      0, wait_reusable_poller_events (poller, fixture.dealer, 0));

    const received_part_t final = receive_part (fixture.dealer);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, final.part_flag);
    TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
        return (wait_reusable_poller_events (poller, fixture.dealer, 0)
                & ZLINK_POLLCOMPLETION)
               != 0;
    }));
    // Completion readiness is level-triggered until the completion is
    // drained, independently of the now-empty public DATA receive path.
    short events = wait_reusable_poller_events (poller, fixture.dealer, 0);
    TEST_ASSERT_TRUE ((events & ZLINK_POLLCOMPLETION) != 0);
    TEST_ASSERT_EQUAL_INT (0, events & ZLINK_POLLIN);
    zlink_completion_t completion = receive_completion (fixture.dealer);
    assert_request_completion (&completion, request_id, ZLINK_REQUEST_OK,
                               "poll-reply");
    assert_no_completion (fixture.dealer);
    input_item.revents = 0;
    TEST_ASSERT_EQUAL_INT (0, zlink_poll (&input_item, 1, 0, NULL));
    TEST_ASSERT_EQUAL_INT (
      0, wait_reusable_poller_events (poller, fixture.dealer, 0));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_poller_remove (poller, fixture.dealer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    fixture.close ();
}

void test_sl_monitor_detach_count_one_has_single_lifecycle ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    configure_socket (router);
    configure_socket (dealer);
    set_routing_id (router, "sl-detach-one-router");
    set_routing_id (dealer, "sl-detach-one-dealer");
    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (
      dealer, ZLINK_EVENT_CONNECTED | ZLINK_EVENT_CONNECTION_READY
                | ZLINK_EVENT_DISCONNECTED | ZLINK_EVENT_CONNECT_RETRIED,
      &probe);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
    TEST_ASSERT_TRUE (wait_for_ready_edge (&probe, 0, NULL));
    const int disconnect_start = test_monitor_probe_count (&probe);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect (dealer, endpoint));
    zlink_monitor_event_t disconnected;
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &probe, ZLINK_EVENT_DISCONNECTED, disconnect_start, &disconnected));
    TEST_ASSERT_EQUAL_UINT (
      ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
      disconnected.transport_lane);
    // A CONNECTION_READY ready-count snapshot may be queued immediately after
    // DISCONNECTED.  Let the monitor settle, then count only lifecycle edges.
    msleep (200);
    TEST_ASSERT_EQUAL_INT (
      1, count_probe_events (&probe, ZLINK_EVENT_DISCONNECTED));
    TEST_ASSERT_EQUAL_INT (
      0, count_probe_events (&probe, ZLINK_EVENT_CONNECT_RETRIED));

    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_sl_monitor_detach_count_two_retires_pair ()
{
    void *first = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *second = test_context_socket (ZLINK_SOCKET_ROUTER);
    configure_socket (first);
    configure_socket (second);
    set_routing_id (first, "sl-detach-two-a");
    set_routing_id (second, "sl-detach-two-b");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (second, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                               "sl-detach-two-a",
                               strlen ("sl-detach-two-a")));
    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (
      second, ZLINK_EVENT_CONNECTED | ZLINK_EVENT_CONNECTION_READY
                | ZLINK_EVENT_DISCONNECTED,
      &probe);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (first, endpoint, sizeof (endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (second, endpoint));
    TEST_ASSERT_TRUE (wait_for_ready_edge (&probe, 0, NULL));
    const int before_detach = test_monitor_probe_count (&probe);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect (second, endpoint));
    TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
        int disconnected = 0;
        const int count = test_monitor_probe_count (&probe);
        for (int index = before_detach; index != count; ++index) {
            if (test_monitor_probe_event_at (&probe, index)
                == ZLINK_EVENT_DISCONNECTED)
                ++disconnected;
        }
        return disconnected == 2;
    }));
    std::set<uint32_t> disconnected_lanes;
    const int after_detach = test_monitor_probe_count (&probe);
    for (int index = before_detach; index != after_detach; ++index) {
        const zlink_monitor_event_t event =
          test_monitor_probe_record_at (&probe, index);
        if (event.event == ZLINK_EVENT_DISCONNECTED)
            disconnected_lanes.insert (event.transport_lane);
    }
    TEST_ASSERT_EQUAL_UINT64 (2, disconnected_lanes.size ());
    TEST_ASSERT_TRUE (
      disconnected_lanes.find (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION)
      != disconnected_lanes.end ());
    TEST_ASSERT_TRUE (
      disconnected_lanes.find (ZLINK_MONITOR_TRANSPORT_LANE_COMPLETION)
      != disconnected_lanes.end ());

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (second, endpoint));
    TEST_ASSERT_TRUE (wait_for_ready_edge (&probe, after_detach, NULL));
    TEST_ASSERT_EQUAL_INT (
      2, count_ready_edges (&probe));

    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (second);
    test_context_socket_close_zero_linger (first);
}

void test_sl_monitor_reply_token_reconnect_uses_current_application ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    configure_socket (router);
    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (
      router, ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED,
      &probe);
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));
    fd_t old_connection = connect_raw_peer (
      endpoint, ZLINK_CORE_SOCKET_DEALER, "sl-token-peer", 1, 0);
    TEST_ASSERT_TRUE (wait_for_ready_edge (&probe, 0, NULL));

    const uint64_t request_sequence = 77;
    const char request_payload[] = "raw-token-request";
    TEST_ASSERT_TRUE (send_wire_frame (
      old_connection, 0, zlink::zmp_kind_request, request_sequence,
      request_payload, sizeof (request_payload) - 1));
    const received_router_part_t request = receive_router_part (router);
    TEST_ASSERT_NOT_EQUAL (0, request.reply_token);
    TEST_ASSERT_EQUAL_STRING (request_payload, request.payload.c_str ());

    const int disconnect_start = test_monitor_probe_count (&probe);
    close (old_connection);
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &probe, ZLINK_EVENT_DISCONNECTED, disconnect_start, NULL));
    fd_t current_connection = connect_raw_peer (
      endpoint, ZLINK_CORE_SOCKET_DEALER, "sl-token-peer", 1, 0);
    TEST_ASSERT_TRUE (wait_for_ready_edge (&probe, disconnect_start, NULL));
    send_reply (router, request, "reply-on-current-application");

    wire_frame_t reply;
    do {
        TEST_ASSERT_TRUE (read_wire_frame (current_connection, &reply));
    } while ((reply.flags & zlink::zmp_flag_control) != 0);
    TEST_ASSERT_EQUAL_HEX8 (zlink::zmp_kind_reply, reply.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_sequence, reply.sequence);
    TEST_ASSERT_EQUAL_UINT64 (strlen ("reply-on-current-application"),
                              reply.body.size ());
    TEST_ASSERT_EQUAL_MEMORY ("reply-on-current-application", &reply.body[0],
                              reply.body.size ());

    close (current_connection);
    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (router);

    // In the inverse direction, an old-generation REPLY is kept behind an
    // already visible multipart DATA head. Replacing the same logical RID
    // before that head drains must fence the old REPLY. A request submitted
    // on the replacement generation must still complete exactly once.
    void *requester = test_context_socket (ZLINK_SOCKET_DEALER);
    configure_socket (requester);
    set_routing_id (requester, "sl-token-requester");
    test_monitor_probe_t requester_probe;
    void *requester_monitor = open_test_monitor_probe (
      requester, ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED,
      &requester_probe);
    char requester_endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (requester, requester_endpoint,
                        sizeof (requester_endpoint));
    fd_t retired_responder = connect_raw_peer (
      requester_endpoint, ZLINK_CORE_SOCKET_ROUTER, "sl-token-responder", 1,
      0);
    TEST_ASSERT_TRUE (wait_for_ready_edge (&requester_probe, 0, NULL));

    const zlink_completion_id_t requester_id =
      send_request (requester, NULL, "request-to-replaced-peer", 15000);
    wire_frame_t raw_request;
    do {
        TEST_ASSERT_TRUE (read_wire_frame (retired_responder, &raw_request));
    } while ((raw_request.flags & zlink::zmp_flag_control) != 0);
    TEST_ASSERT_EQUAL_HEX8 (zlink::zmp_kind_request, raw_request.kind);
    TEST_ASSERT_NOT_EQUAL (0, raw_request.sequence);

    const char retired_prefix[] = "retired-data-prefix";
    const char retired_final[] = "retired-data-final";
    const char retired_reply[] = "retired-late-reply";
    TEST_ASSERT_TRUE (send_wire_frame (
      retired_responder, zlink::zmp_flag_more, zlink::zmp_kind_data, 0,
      retired_prefix, sizeof (retired_prefix) - 1));
    TEST_ASSERT_TRUE (send_wire_frame (
      retired_responder, 0, zlink::zmp_kind_data, 0, retired_final,
      sizeof (retired_final) - 1));
    TEST_ASSERT_TRUE (send_wire_frame (
      retired_responder, 0, zlink::zmp_kind_reply, raw_request.sequence,
      retired_reply, sizeof (retired_reply) - 1));
    const received_part_t prefix = receive_part (requester);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, prefix.part_flag);
    TEST_ASSERT_EQUAL_STRING (retired_prefix, prefix.payload.c_str ());

    const int retired_disconnect_start =
      test_monitor_probe_count (&requester_probe);
    close (retired_responder);
    TEST_ASSERT_TRUE (wait_for_probe_event (
      &requester_probe, ZLINK_EVENT_DISCONNECTED,
      retired_disconnect_start, NULL));
    const int replacement_start = test_monitor_probe_count (&requester_probe);
    fd_t current_responder = connect_raw_peer (
      requester_endpoint, ZLINK_CORE_SOCKET_ROUTER, "sl-token-responder", 1,
      0);
    TEST_ASSERT_TRUE (
      wait_for_ready_edge (&requester_probe, replacement_start, NULL));
    const received_part_t final = receive_part (requester);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, final.part_flag);
    TEST_ASSERT_EQUAL_STRING (retired_final, final.payload.c_str ());
    zlink_completion_t retired_completion = receive_completion (requester);
    assert_request_completion (&retired_completion, requester_id,
                               ZLINK_REQUEST_NOT_CONNECTED);
    assert_no_completion (requester, 100);

    const zlink_completion_id_t current_requester_id =
      send_request (requester, NULL, "request-on-current-peer", 3000);
    wire_frame_t current_request;
    do {
        TEST_ASSERT_TRUE (
          read_wire_frame (current_responder, &current_request));
    } while ((current_request.flags & zlink::zmp_flag_control) != 0);
    TEST_ASSERT_EQUAL_HEX8 (zlink::zmp_kind_request, current_request.kind);
    TEST_ASSERT_NOT_EQUAL (raw_request.sequence, current_request.sequence);

    const char current_reply[] = "current-generation-reply";
    TEST_ASSERT_TRUE (send_wire_frame (
      current_responder, 0, zlink::zmp_kind_reply, current_request.sequence,
      current_reply, sizeof (current_reply) - 1));
    zlink_completion_t requester_completion = receive_completion (requester);
    assert_request_completion (&requester_completion, current_requester_id,
                               ZLINK_REQUEST_OK, current_reply);
    assert_no_completion (requester);
    assert_no_public_part (requester);

    close (current_responder);
    close_test_monitor_probe (&requester_monitor, &requester_probe);
    test_context_socket_close_zero_linger (requester);
}

void test_sl_monitor_status_abi_and_flow_detail_stable ()
{
    TEST_ASSERT_EQUAL_UINT32 (4u, ZLINK_MONITOR_STATUS_ABI_VERSION);
    TEST_ASSERT_EQUAL_UINT32 (1u, ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1);
    TEST_ASSERT_EQUAL_INT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION, 0);
    TEST_ASSERT_EQUAL_INT (ZLINK_MONITOR_TRANSPORT_LANE_COMPLETION, 1);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLIN, 1);
    TEST_ASSERT_EQUAL_INT (ZLINK_POLLCOMPLETION, 32);

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
    const zlink_monitor_status_t dealer_status = read_status (dealer);
    const zlink_monitor_status_t router_status = read_status (router);
    const zlink_monitor_status_t pair_status = read_status (pair);
    TEST_ASSERT_EQUAL_UINT32 (ZLINK_MONITOR_STATUS_ABI_VERSION,
                              dealer_status.abi_version);
    TEST_ASSERT_EQUAL_UINT32 (sizeof (zlink_monitor_status_t),
                              dealer_status.struct_size);
    TEST_ASSERT_TRUE (
      (dealer_status.detail_flags & ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE)
      != 0);
    TEST_ASSERT_TRUE (
      (router_status.detail_flags & ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE)
      != 0);
    TEST_ASSERT_EQUAL_UINT32 (
      0, pair_status.detail_flags & ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE);

    const zlink_auto_hwm_budget_snapshot_t budget = read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT32 (ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1,
                              budget.abi_version);
    TEST_ASSERT_EQUAL_UINT32 (sizeof (zlink_auto_hwm_budget_snapshot_t),
                              budget.struct_size);
    TEST_ASSERT_EQUAL_UINT64 (0, budget.application_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              budget.outstanding_application_lease_count);
    TEST_ASSERT_EQUAL_UINT64 (0, budget.retired_queue_count);
    TEST_ASSERT_EQUAL_UINT64 (0, budget.deferred_origin_credit_bytes);
    for (size_t index = 0; index != 8; ++index)
        TEST_ASSERT_EQUAL_UINT64 (0, budget.reserved_u64[index]);

    test_context_socket_close_zero_linger (pair);
    test_context_socket_close_zero_linger (router);
    test_context_socket_close_zero_linger (dealer);
}

void test_sl_monitor_status_ready_is_independent_of_event_mask ()
{
    {
        dr_fixture_t dr ("inproc://sl-monitor-status-zero-mask-dr");
        test_monitor_probe_t probe;
        void *monitor = open_test_monitor_probe (dr.router, 0, &probe);
        dr.connect_and_prime ();
        TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
            return (read_monitor_status (monitor).state_flags
                    & ZLINK_MONITOR_STATE_READY)
                   != 0;
        }));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONNECT_OK, zlink_disconnect (dr.dealer, dr.endpoint.c_str ()));
        TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
            (void) as_socket (dr.dealer)->process_submit_commands ();
            return (read_monitor_status (monitor).state_flags
                    & ZLINK_MONITOR_STATE_READY)
                   == 0;
        }));
        TEST_ASSERT_EQUAL_INT (0, test_monitor_probe_count (&probe));
        close_test_monitor_probe (&monitor, &probe);
        dr.close ();
    }

    {
        rr_fixture_t rr ("inproc://sl-monitor-status-zero-mask-rr");
        test_monitor_probe_t probe;
        void *monitor = open_test_monitor_probe (rr.first, 0, &probe);
        rr.connect_and_prime ();
        TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
            return (read_monitor_status (monitor).state_flags
                    & ZLINK_MONITOR_STATE_READY)
                   != 0;
        }));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONNECT_OK, zlink_disconnect (rr.second, rr.endpoint.c_str ()));
        TEST_ASSERT_TRUE (zlink_test_wait_until (contract_wait_ms, [&] {
            (void) as_socket (rr.second)->process_submit_commands ();
            return (read_monitor_status (monitor).state_flags
                    & ZLINK_MONITOR_STATE_READY)
                   == 0;
        }));
        TEST_ASSERT_EQUAL_INT (0, test_monitor_probe_count (&probe));
        close_test_monitor_probe (&monitor, &probe);
        rr.close ();
    }
}

} // namespace

int main ()
{
    setup_test_environment (180);
    UNITY_BEGIN ();

#define RUN_SINGLE_LANE_CONTRACT_TEST(test_)                                \
    do {                                                                    \
        if (should_run_case (#test_))                                       \
            RUN_TEST (test_);                                               \
    } while (false)

    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_wire_dr_count_one_ready_metadata);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_wire_rr_count_two_ready_fence);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_wire_nonpaired_patterns_omit_lane_metadata);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_wire_symmetric_direction_and_inproc_order);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_wire_mandatory_lane_count_rejections);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_wire_old_peer_without_lane_count_rejected);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_wire_transport_matrix_counts);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_request_reply_at_head_completion_only);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_request_multipart_data_before_reply_fifo);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_request_reply_before_data_destinations);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_request_timeout_wins_once_over_late_reply);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_request_reply_submit_obeys_dr_backpressure);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_request_rr_reply_isolated_from_application_pause);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_request_router_to_dealer_type_restriction);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_flow_dr_application_control_is_not_public_data);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_flow_rr_completion_control_progresses_under_pause);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_flow_dr_normal_kinds_share_pause_and_hwm_gate);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_flow_control_boundary_coalesces_latest_state);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_flow_stale_generation_is_fenced);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_flow_reconnect_resynchronizes_absolute_state);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_flow_snapshot_accounts_dr_reply_as_application);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_monitor_lane_values_follow_topology);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_monitor_logical_ready_once_per_peer);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_monitor_polling_separates_data_and_completion);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_monitor_detach_count_one_has_single_lifecycle);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_monitor_detach_count_two_retires_pair);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_monitor_reply_token_reconnect_uses_current_application);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_monitor_status_abi_and_flow_detail_stable);
    RUN_SINGLE_LANE_CONTRACT_TEST (test_sl_monitor_status_ready_is_independent_of_event_mask);

#undef RUN_SINGLE_LANE_CONTRACT_TEST
    return UNITY_END ();
}
