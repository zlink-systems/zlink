/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"


#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <vector>

#if defined(ZLINK_HAVE_WINDOWS)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

SETUP_TEARDOWN_TESTCONTEXT

static const size_t stream_routing_id_size = 4;
static const char *const stream_socket_smoke_cases[] = {
  "test_stream_no_data_recv_part_locks_raw_mode",
  "test_stream_successful_recv_part_locks_raw_mode",
  "test_stream_raw_inproc_parts_are_independent_final_chunks",
  "test_stream_rejects_unsupported_send_without_poisoning_routed_final",
  "test_stream_notify_records_and_bind_constraint",
  "test_stream_recv_ready_precedes_first_payload_contract",
  "test_stream_phase3_mode_freeze_contract",
  "test_stream_phase3_packet_pull_contract",
  "test_stream_packet_peer_isolation_readiness_and_order",
  "test_stream_phase3_packet_maxmsgsize_contract",
  "test_stream_phase3_packet_enqueue_close_race",
};

static bool should_run_stream_socket_test (const char *name_)
{
    const char *selected = getenv ("ZLINK_TEST_CASE");
    if (selected && *selected)
        return strcmp (selected, name_) == 0;

    for (size_t i = 0;
         i < sizeof (stream_socket_smoke_cases) / sizeof (stream_socket_smoke_cases[0]); ++i) {
        if (strcmp (stream_socket_smoke_cases[i], name_) == 0)
            return true;
    }
    return false;
}
static bool wait_monitor_event (void *monitor_,
                                void *activity_socket_,
                                uint64_t expected_event_,
                                unsigned char routing_id_[stream_routing_id_size],
                                int timeout_ms_)
{
    return wait_monitor_event_routing_id (monitor_, activity_socket_, expected_event_, routing_id_,
                                          stream_routing_id_size, timeout_ms_);
}

static bool wait_monitor_event_direct (void *monitor_,
                                       uint64_t expected_event_,
                                       unsigned char routing_id_[stream_routing_id_size],
                                       int timeout_ms_)
{
    return wait_monitor_event_routing_id (monitor_, NULL, expected_event_, routing_id_,
                                          stream_routing_id_size, timeout_ms_);
}

static bool wait_monitor_ready_edge_direct (
  void *monitor_, unsigned char routing_id_[stream_routing_id_size],
  int timeout_ms_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        const std::chrono::milliseconds remaining =
          std::chrono::duration_cast<std::chrono::milliseconds> (
            deadline - std::chrono::steady_clock::now ());
        if (remaining.count () <= 0)
            return false;

        zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
        const int poll_timeout =
          remaining.count () > 200 ? 200
                                  : static_cast<int> (remaining.count ());
        const int poll_rc = zlink_poll (&item, 1, poll_timeout, NULL);
        if (poll_rc <= 0 || (item.revents & ZLINK_POLLIN) == 0)
            continue;

        for (;;) {
            zlink_monitor_event_t event;
            if (recv_monitor_event_from_socket (monitor_, &event,
                                                ZLINK_DONTWAIT)
                != 0)
                break;
            if (event.event != ZLINK_EVENT_CONNECTION_READY
                || (event.flags
                    & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE)
                     == 0
                || event.routing_id.size != stream_routing_id_size)
                continue;
            memcpy (routing_id_, event.routing_id.data, stream_routing_id_size);
            return true;
        }
    }
    return false;
}

static bool wait_monitor_event_direct_for_rid (
  void *monitor_, uint64_t expected_event_,
  const unsigned char expected_rid_[stream_routing_id_size], int timeout_ms_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        const std::chrono::milliseconds remaining =
          std::chrono::duration_cast<std::chrono::milliseconds> (
            deadline - std::chrono::steady_clock::now ());
        if (remaining.count () <= 0)
            return false;

        unsigned char observed_rid[stream_routing_id_size];
        if (!wait_monitor_event_direct (
              monitor_, expected_event_, observed_rid,
              static_cast<int> (remaining.count ())))
            return false;
        if (memcmp (expected_rid_, observed_rid, stream_routing_id_size) == 0)
            return true;
    }
    return false;
}

static void send_stream_msg (void *socket_,
                             const unsigned char routing_id_[stream_routing_id_size],
                             const void *data_,
                             size_t size_)
{
    zlink_routing_id_t rid = {};
    rid.size = static_cast<uint8_t> (stream_routing_id_size);
    memcpy (rid.data, routing_id_, stream_routing_id_size);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (size_),
      test_stream_send_bytes (socket_, &rid, data_, size_, 0));
}

static bool recv_stream_routing_id_and_payload (void *socket_,
                                                zlink_routing_id_t *rid_out_,
                                                zlink_msg_t *payload_out_,
                                                int flags_)
{
    if (!socket_ || !rid_out_ || !payload_out_) {
        errno = EINVAL;
        return false;
    }

    const zlink_routing_id_t *source_rid = NULL;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    const zlink_recv_result_t recv_rc = zlink_recv_part (
      socket_, &source_rid, payload_out_, &has_more,
      static_cast<zlink_recv_flags_t> (flags_));
    if (recv_rc != ZLINK_RECV_OK)
        return false;

    if (!source_rid || source_rid->size != stream_routing_id_size) {
        const int close_rc = zlink_msg_close (payload_out_);
        TEST_ASSERT_SUCCESS_ERRNO (close_rc);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (payload_out_));
        errno = EPROTO;
        return false;
    }

    *rid_out_ = *source_rid;
    if (has_more != ZLINK_PART_FINAL) {
        const int close_rc = zlink_msg_close (payload_out_);
        TEST_ASSERT_SUCCESS_ERRNO (close_rc);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (payload_out_));
        errno = EPROTO;
        return false;
    }
    return true;
}

static bool wait_stream_notify_record (
  void *socket_, unsigned char routing_id_[stream_routing_id_size], int timeout_ms_)
{
    const int attempts = timeout_ms_ / 10;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
        const zlink_routing_id_t *source_rid = NULL;
        zlink_part_flag_t has_more = ZLINK_PART_MORE;
        const zlink_recv_result_t rc = zlink_recv_part (
          socket_, &source_rid, &part, &has_more,
          static_cast<zlink_recv_flags_t> (ZLINK_DONTWAIT));
        if (rc == ZLINK_RECV_OK) {
            const bool valid = source_rid && source_rid->size == stream_routing_id_size
                               && zlink_msg_size (&part) == 0
                               && has_more == ZLINK_PART_FINAL;
            if (valid)
                memcpy (routing_id_, source_rid->data, stream_routing_id_size);
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
            return valid;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
#if defined(ZLINK_HAVE_WINDOWS)
        Sleep (10);
#else
        usleep (10000);
#endif
    }
    return false;
}

static bool parse_tcp_endpoint (const char *endpoint_, char host_[64], int *port_)
{
    if (!endpoint_ || !host_ || !port_)
        return false;

    char proto[8] = {0};
    int port = 0;
    if (sscanf (endpoint_, "%7[^:]://%63[^:]:%d", proto, host_, &port) != 3)
        return false;

    if (strcmp (proto, "tcp") != 0 || port <= 0 || port > 65535)
        return false;

    *port_ = port;
    return true;
}

#if defined(ZLINK_HAVE_WINDOWS)
static int connect_raw_tcp (const char *endpoint_)
{
    LIBZLINK_UNUSED (endpoint_);
    errno = EOPNOTSUPP;
    return -1;
}

static int send_stream_packet (int fd_, const void *data_, size_t size_)
{
    LIBZLINK_UNUSED (fd_);
    LIBZLINK_UNUSED (data_);
    LIBZLINK_UNUSED (size_);
    return EOPNOTSUPP;
}

static int recv_stream_packet (int fd_, void *buf_, size_t cap_)
{
    LIBZLINK_UNUSED (fd_);
    LIBZLINK_UNUSED (buf_);
    LIBZLINK_UNUSED (cap_);
    return -1;
}

static void close_raw_fd (int fd_)
{
    LIBZLINK_UNUSED (fd_);
}

#else
static int connect_raw_tcp (const char *endpoint_)
{
    char host[64];
    int port = 0;
    if (!parse_tcp_endpoint (endpoint_, host, &port)) {
        errno = EINVAL;
        return -1;
    }

    const int fd = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons (static_cast<uint16_t> (port));
    if (inet_pton (AF_INET, host, &addr.sin_addr) != 1) {
        close (fd);
        errno = EINVAL;
        return -1;
    }

    if (connect (fd, reinterpret_cast<const struct sockaddr *> (&addr), sizeof (addr)) != 0) {
        const int err = errno;
        close (fd);
        errno = err;
        return -1;
    }

    return fd;
}

static int send_all (int fd_, const unsigned char *buf_, size_t size_)
{
    size_t off = 0;
    while (off < size_) {
        const ssize_t n = send (fd_, buf_ + off, size_ - off, 0);
        if (n > 0) {
            off += static_cast<size_t> (n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int send_stream_packet (int fd_, const void *data_, size_t size_)
{
    return send_all (fd_, static_cast<const unsigned char *> (data_), size_);
}

static int recv_stream_packet (int fd_, void *buf_, size_t cap_)
{
    const ssize_t n = recv (fd_, static_cast<unsigned char *> (buf_), cap_, 0);
    if (n <= 0)
        return -1;
    return static_cast<int> (n);
}

static bool wait_raw_fd_closed (int fd_)
{
    unsigned char probe[1];
    const ssize_t n = recv (fd_, probe, sizeof (probe), 0);
    if (n == 0)
        return true;
    if (n < 0 && (errno == ECONNRESET || errno == EPIPE))
        return true;
    return false;
}

static void close_raw_fd (int fd_)
{
    if (fd_ >= 0)
        close (fd_);
}

static int set_raw_fd_timeout (int fd_, int timeout_ms_)
{
    struct timeval tv;
    tv.tv_sec = timeout_ms_ / 1000;
    tv.tv_usec = (timeout_ms_ % 1000) * 1000;
    if (setsockopt (fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv)) != 0)
        return -1;
    if (setsockopt (fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof (tv)) != 0)
        return -1;
    return 0;
}

static int recv_exact (int fd_, void *buf_, size_t size_)
{
    unsigned char *dst = static_cast<unsigned char *> (buf_);
    size_t off = 0;
    while (off < size_) {
        const ssize_t n = recv (fd_, dst + off, size_ - off, 0);
        if (n > 0) {
            off += static_cast<size_t> (n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static uint32_t load_u32_be (const unsigned char *src_)
{
    return (static_cast<uint32_t> (src_[0]) << 24) | (static_cast<uint32_t> (src_[1]) << 16)
           | (static_cast<uint32_t> (src_[2]) << 8) | static_cast<uint32_t> (src_[3]);
}

static void store_u32_be (unsigned char *dst_, uint32_t value_)
{
    dst_[0] = static_cast<unsigned char> ((value_ >> 24) & 0xFF);
    dst_[1] = static_cast<unsigned char> ((value_ >> 16) & 0xFF);
    dst_[2] = static_cast<unsigned char> ((value_ >> 8) & 0xFF);
    dst_[3] = static_cast<unsigned char> (value_ & 0xFF);
}

static void store_u16_be (unsigned char *dst_, uint16_t value_)
{
    dst_[0] = static_cast<unsigned char> ((value_ >> 8) & 0xFF);
    dst_[1] = static_cast<unsigned char> (value_ & 0xFF);
}

#endif

static void test_sleep_ms (int delay_ms_)
{
#if defined(ZLINK_HAVE_WINDOWS)
    Sleep (static_cast<DWORD> (delay_ms_));
#else
    usleep (delay_ms_ * 1000);
#endif
}

static bool wait_counter_at_least (std::atomic<int> *counter_, int expected_, int timeout_ms_)
{
    const int slice_ms = 10;
    const int loops = timeout_ms_ > 0 ? timeout_ms_ / slice_ms + 1 : 1;
    for (int i = 0; i < loops; ++i) {
        if (counter_->load (std::memory_order_acquire) >= expected_)
            return true;
        test_sleep_ms (slice_ms);
    }
    return false;
}

static std::vector<unsigned char> build_stream_packet_frame (const unsigned char *header_,
                                                             size_t header_size_,
                                                             const unsigned char *body_,
                                                             size_t body_size_)
{
    std::vector<unsigned char> frame (6 + header_size_ + body_size_);
    store_u16_be (&frame[0], static_cast<uint16_t> (header_size_));
    store_u32_be (&frame[2], static_cast<uint32_t> (body_size_));
    if (header_size_ > 0 && header_)
        memcpy (&frame[6], header_, header_size_);
    if (body_size_ > 0 && body_)
        memcpy (&frame[6 + header_size_], body_, body_size_);
    return frame;
}

static int send_stream_packet_frame (int fd_,
                                     const unsigned char *header_,
                                     size_t header_size_,
                                     const unsigned char *body_,
                                     size_t body_size_)
{
    const std::vector<unsigned char> frame =
      build_stream_packet_frame (header_, header_size_, body_, body_size_);
    return send_stream_packet (fd_, &frame[0], frame.size ());
}

static zlink_recv_result_t wait_stream_packet_pull (
  void *stream_, const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *header_out_, zlink_msg_t *body_out_, int timeout_ms_)
{
    const int attempts = timeout_ms_ / 10;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const zlink_recv_result_t rc = zlink_stream_recv_packet (
          stream_, source_rid_out_, header_out_, body_out_,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc != ZLINK_RECV_NO_DATA)
            return rc;
        test_sleep_ms (10);
    }
    errno = EAGAIN;
    return ZLINK_RECV_NO_DATA;
}

#if !defined(ZLINK_HAVE_WINDOWS)
static bool pump_packet_until_raw_fd_closed (void *stream_, int fd_, int timeout_ms_)
{
    const int attempts = timeout_ms_ / 10;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        zlink_msg_t header;
        zlink_msg_t body;
        if (zlink_msg_init (&header) != ZLINK_CONFIG_OK
            || zlink_msg_init (&body) != ZLINK_CONFIG_OK)
            return false;
        const zlink_routing_id_t *source_rid = NULL;
        const zlink_recv_result_t rc = zlink_stream_recv_packet (
          stream_, &source_rid, &header, &body, ZLINK_RECV_FLAGS_DONTWAIT);
        (void) zlink_msg_close (&header);
        (void) zlink_msg_close (&body);
        if (rc != ZLINK_RECV_NO_DATA && rc != ZLINK_RECV_OK)
            return false;

        unsigned char probe = 0;
        const ssize_t recv_rc = recv (fd_, &probe, sizeof (probe),
                                      MSG_PEEK | MSG_DONTWAIT);
        if (recv_rc == 0
            || (recv_rc < 0
                && (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN)))
            return true;
        test_sleep_ms (10);
    }
    return false;
}

static const zlink_routing_id_t *send_and_pull_packet_probe (
  int fd_, void *stream_, uint32_t sequence_)
{
    unsigned char header[4];
    store_u32_be (header, sequence_);
    const unsigned char body[] = "validation-probe";
    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet_frame (fd_, header, sizeof (header), body,
                                   sizeof (body) - 1));

    zlink_msg_t received_header;
    zlink_msg_t received_body;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received_body));
    const zlink_routing_id_t *source_rid =
      reinterpret_cast<const zlink_routing_id_t *> (0x1);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      wait_stream_packet_pull (stream_, &source_rid, &received_header,
                               &received_body, 3000));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, source_rid->size);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (header),
                              zlink_msg_size (&received_header));
    TEST_ASSERT_EQUAL_UINT32 (
      sequence_,
      load_u32_be (static_cast<const unsigned char *> (
        zlink_msg_data (&received_header))));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (body, zlink_msg_data (&received_body),
                                   sizeof (body) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received_body));
    return source_rid;
}
#endif

static std::string make_routing_id_key (const unsigned char *data_, size_t size_)
{
    if (!data_ || size_ == 0)
        return std::string ();

    return std::string (reinterpret_cast<const char *> (data_), size_);
}

struct stream_monitor_probe_t
{
    stream_monitor_probe_t () :
        accepted (0),
        connection_ready (0),
        disconnected (0),
        bad_ready_routing_id (0),
        bad_disconnected_routing_id (0),
        ready_routing_ids (),
        disconnected_routing_ids ()
    {
    }

    int accepted;
    int connection_ready;
    int disconnected;
    int bad_ready_routing_id;
    int bad_disconnected_routing_id;
    std::set<std::string> ready_routing_ids;
    std::set<std::string> disconnected_routing_ids;
};

static void record_stream_monitor_event (stream_monitor_probe_t *probe_,
                                         const zlink_monitor_event_t *event_)
{
    if (!probe_ || !event_)
        return;

    switch (event_->event) {
        case ZLINK_EVENT_ACCEPTED:
            ++probe_->accepted;
            break;
        case ZLINK_EVENT_CONNECTION_READY:
            ++probe_->connection_ready;
            if (event_->routing_id.size != stream_routing_id_size) {
                ++probe_->bad_ready_routing_id;
                break;
            }
            probe_->ready_routing_ids.insert (
              make_routing_id_key (event_->routing_id.data, event_->routing_id.size));
            break;
        case ZLINK_EVENT_DISCONNECTED:
            ++probe_->disconnected;
            if (event_->routing_id.size != stream_routing_id_size) {
                ++probe_->bad_disconnected_routing_id;
                break;
            }
            probe_->disconnected_routing_ids.insert (
              make_routing_id_key (event_->routing_id.data, event_->routing_id.size));
            break;
        default:
            break;
    }
}

static void
collect_stream_monitor_events (void *monitor_, stream_monitor_probe_t *probe_, int poll_timeout_ms_)
{
    if (!monitor_ || !probe_)
        return;

    zlink_pollitem_t items[] = {{monitor_, 0, ZLINK_POLLIN, 0}};
    const int rc = zlink_poll (items, 1, poll_timeout_ms_, NULL);
    if (rc <= 0 || (items[0].revents & ZLINK_POLLIN) == 0)
        return;

    for (;;) {
        zlink_monitor_event_t event;
        if (recv_monitor_event_from_socket (monitor_, &event, ZLINK_DONTWAIT) != 0)
            break;
        record_stream_monitor_event (probe_, &event);
    }
}

static bool wait_stream_monitor_progress (void *monitor_,
                                          stream_monitor_probe_t *probe_,
                                          int expected_accepted_,
                                          size_t expected_ready_,
                                          size_t expected_disconnected_,
                                          int timeout_ms_)
{
    const int slice_ms = 20;
    const int loops = timeout_ms_ > 0 ? timeout_ms_ / slice_ms + 1 : 1;
    for (int i = 0; i < loops; ++i) {
        collect_stream_monitor_events (monitor_, probe_, slice_ms);
        if (probe_->accepted >= expected_accepted_
            && probe_->ready_routing_ids.size () >= expected_ready_
            && probe_->disconnected_routing_ids.size () >= expected_disconnected_) {
            return true;
        }
    }

    collect_stream_monitor_events (monitor_, probe_, 0);
    return probe_->accepted >= expected_accepted_
           && probe_->ready_routing_ids.size () >= expected_ready_
           && probe_->disconnected_routing_ids.size () >= expected_disconnected_;
}

struct stream_ordering_probe_t
{
    stream_ordering_probe_t () :
        ready_events (0),
        disconnected_events (0),
        payload_before_ready (0),
        disconnect_during_payload (0),
        strict_drop_count (0),
        strict_accept_count (0),
        payloads_seen (0),
        ready_routing_ids (),
        disconnected_routing_ids (),
        active_payload_routing_ids (),
        seen_payload_routing_ids (),
        mu ()
    {
    }

    std::atomic<int> ready_events;
    std::atomic<int> disconnected_events;
    std::atomic<int> payload_before_ready;
    std::atomic<int> disconnect_during_payload;
    std::atomic<int> strict_drop_count;
    std::atomic<int> strict_accept_count;
    std::atomic<int> payloads_seen;
    std::set<std::string> ready_routing_ids;
    std::set<std::string> disconnected_routing_ids;
    std::set<std::string> active_payload_routing_ids;
    std::set<std::string> seen_payload_routing_ids;
    std::mutex mu;
};

static void collect_stream_ordering_monitor_events (void *monitor_,
                                                    stream_ordering_probe_t *probe_,
                                                    int poll_timeout_ms_)
{
    if (!monitor_ || !probe_)
        return;

    zlink_pollitem_t items[] = {{monitor_, 0, ZLINK_POLLIN, 0}};
    const int rc = zlink_poll (items, 1, poll_timeout_ms_, NULL);
    if (rc <= 0 || (items[0].revents & ZLINK_POLLIN) == 0)
        return;

    for (;;) {
        zlink_monitor_event_t event;
        if (recv_monitor_event_from_socket (monitor_, &event, ZLINK_DONTWAIT) != 0)
            break;

        if (event.routing_id.size != stream_routing_id_size)
            continue;

        const std::string key = make_routing_id_key (event.routing_id.data, event.routing_id.size);
        std::lock_guard<std::mutex> lk (probe_->mu);
        if (event.event == ZLINK_EVENT_CONNECTION_READY) {
            probe_->ready_routing_ids.insert (key);
            probe_->ready_events.fetch_add (1, std::memory_order_release);
        } else if (event.event == ZLINK_EVENT_DISCONNECTED) {
            if (probe_->active_payload_routing_ids.find (key)
                != probe_->active_payload_routing_ids.end ()) {
                probe_->disconnect_during_payload.fetch_add (1, std::memory_order_release);
            }
            probe_->disconnected_routing_ids.insert (key);
            probe_->disconnected_events.fetch_add (1, std::memory_order_release);
        }
    }
}

static bool mark_stream_payload_begin (stream_ordering_probe_t *probe_,
                                       const zlink_routing_id_t *rid_,
                                       bool strict_gate_)
{
    if (!probe_ || !rid_ || rid_->size != stream_routing_id_size)
        return false;

    const std::string key = make_routing_id_key (rid_->data, rid_->size);
    std::lock_guard<std::mutex> lk (probe_->mu);
    probe_->payloads_seen.fetch_add (1, std::memory_order_release);
    probe_->active_payload_routing_ids.insert (key);
    const bool ready = probe_->ready_routing_ids.find (key) != probe_->ready_routing_ids.end ();
    const bool first_payload = probe_->seen_payload_routing_ids.insert (key).second;
    if (first_payload && !ready) {
        probe_->payload_before_ready.fetch_add (1, std::memory_order_release);
    }
    if (strict_gate_) {
        if (ready)
            probe_->strict_accept_count.fetch_add (1, std::memory_order_release);
        else
            probe_->strict_drop_count.fetch_add (1, std::memory_order_release);
    }
    return ready;
}

static void mark_stream_payload_end (stream_ordering_probe_t *probe_,
                                     const zlink_routing_id_t *rid_)
{
    if (!probe_ || !rid_ || rid_->size != stream_routing_id_size)
        return;

    const std::string key = make_routing_id_key (rid_->data, rid_->size);
    std::lock_guard<std::mutex> lk (probe_->mu);
    probe_->active_payload_routing_ids.erase (key);
}

static bool
wait_stream_ordering_counter (std::atomic<int> *counter_, int expected_, int timeout_ms_)
{
    return wait_counter_at_least (counter_, expected_, timeout_ms_);
}

static bool wait_stream_ordering_sets (stream_ordering_probe_t *probe_,
                                       size_t expected_ready_,
                                       size_t expected_disconnected_,
                                       int timeout_ms_)
{
    const int slice_ms = 20;
    const int loops = timeout_ms_ > 0 ? timeout_ms_ / slice_ms + 1 : 1;
    for (int i = 0; i < loops; ++i) {
        {
            std::lock_guard<std::mutex> lk (probe_->mu);
            if (probe_->ready_routing_ids.size () >= expected_ready_
                && probe_->disconnected_routing_ids.size () >= expected_disconnected_) {
                return true;
            }
        }
        test_sleep_ms (slice_ms);
    }

    std::lock_guard<std::mutex> lk (probe_->mu);
    return probe_->ready_routing_ids.size () >= expected_ready_
           && probe_->disconnected_routing_ids.size () >= expected_disconnected_;
}

static bool wait_stream_ordering_sets_with_monitor (void *monitor_,
                                                    stream_ordering_probe_t *probe_,
                                                    size_t expected_ready_,
                                                    size_t expected_disconnected_,
                                                    int timeout_ms_)
{
    const int slice_ms = 20;
    const int loops = timeout_ms_ > 0 ? timeout_ms_ / slice_ms + 1 : 1;
    for (int i = 0; i < loops; ++i) {
        collect_stream_ordering_monitor_events (monitor_, probe_, slice_ms);
        {
            std::lock_guard<std::mutex> lk (probe_->mu);
            if (probe_->ready_routing_ids.size () >= expected_ready_
                && probe_->disconnected_routing_ids.size () >= expected_disconnected_) {
                return true;
            }
        }
    }

    collect_stream_ordering_monitor_events (monitor_, probe_, 0);
    std::lock_guard<std::mutex> lk (probe_->mu);
    return probe_->ready_routing_ids.size () >= expected_ready_
           && probe_->disconnected_routing_ids.size () >= expected_disconnected_;
}

static bool wait_stream_ready_for_routing_id (void *monitor_,
                                              stream_ordering_probe_t *probe_,
                                              const zlink_routing_id_t *rid_,
                                              int timeout_ms_)
{
    if (!monitor_ || !probe_ || !rid_ || rid_->size != stream_routing_id_size)
        return false;

    const std::string key = make_routing_id_key (rid_->data, rid_->size);
    const int slice_ms = 20;
    const int loops = timeout_ms_ > 0 ? timeout_ms_ / slice_ms + 1 : 1;
    for (int i = 0; i < loops; ++i) {
        collect_stream_ordering_monitor_events (monitor_, probe_, slice_ms);
        {
            std::lock_guard<std::mutex> lk (probe_->mu);
            if (probe_->ready_routing_ids.find (key) != probe_->ready_routing_ids.end ()) {
                return true;
            }
        }
    }

    collect_stream_ordering_monitor_events (monitor_, probe_, 0);
    std::lock_guard<std::mutex> lk (probe_->mu);
    return probe_->ready_routing_ids.find (key) != probe_->ready_routing_ids.end ();
}

static void configure_stream_regression_socket (void *socket_, int backlog_)
{
    TEST_ASSERT_NOT_NULL (socket_);

    const int zero = 0;
    zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_RAW;
    const uint64_t hwm = 10u * (4096u + sizeof (zlink_msg_t));
    const int timeout_ms = 200;
    const int nodelay = 1;
    const int backlog = backlog_ > 0 ? backlog_ : 256;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (socket_, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_BACKLOG, &backlog, sizeof (backlog)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_TCP_NODELAY, &nodelay, sizeof (nodelay)));
}

void test_stream_no_data_recv_part_locks_raw_mode ()
{
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream);
    zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_RAW;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (stream, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));

    zlink_msg_t part;
    const char retained_payload[] = "retained";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&part, sizeof (retained_payload)));
    memcpy (zlink_msg_data (&part), retained_payload, sizeof (retained_payload));
    const zlink_routing_id_t retained_rid = {};
    const zlink_routing_id_t *source_rid = &retained_rid;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_recv_part (stream, &source_rid, &part, &has_more,
                       ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_PTR (&retained_rid, source_rid);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, has_more);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (retained_payload), zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_MEMORY (retained_payload, zlink_msg_data (&part),
                              sizeof (retained_payload));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));

    test_context_socket_close_zero_linger (stream);
}

#if defined(ZLINK_HAVE_WINDOWS)
void test_stream_successful_recv_part_locks_raw_mode ()
{
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
}
#else
void test_stream_successful_recv_part_locks_raw_mode ()
{
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream);
    zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_RAW;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (stream, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));

    const int recv_timeout_ms = 3000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (stream, ZLINK_OPT_RCVTIMEO, &recv_timeout_ms,
                        sizeof (recv_timeout_ms)));
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (stream, endpoint, sizeof (endpoint));
    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);

    const unsigned char payload[] = "raw-part-mode";
    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet (client_fd, payload, sizeof (payload) - 1));

    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
    const zlink_routing_id_t *source_rid = NULL;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv_part (stream, &source_rid, &part, &has_more,
                       ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, source_rid->size);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (payload) - 1, zlink_msg_size (&part));
    TEST_ASSERT_EQUAL_MEMORY (payload, zlink_msg_data (&part),
                              sizeof (payload) - 1);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));

    close_raw_fd (client_fd);
    test_context_socket_close_zero_linger (stream);
}
#endif

void test_stream_raw_inproc_parts_are_independent_final_chunks ()
{
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    void *peer = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (stream);
    TEST_ASSERT_NOT_NULL (peer);

    const zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_RAW;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (stream, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));

    const char endpoint[] =
      "inproc://stream-raw-independent-final-chunks";
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (stream, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (peer, endpoint));

    const char *const payloads[] = {"multipart-head", "multipart-tail",
                                    "independent-final"};
    const zlink_part_flag_t send_flags[] = {
      ZLINK_PART_MORE, ZLINK_PART_FINAL, ZLINK_PART_FINAL};
    for (size_t i = 0; i != 3; ++i) {
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_msg_init_size (&part, strlen (payloads[i])));
        memcpy (zlink_msg_data (&part), payloads[i], strlen (payloads[i]));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (peer, &part, ZLINK_SEND_FLAGS_NONE,
                           send_flags[i], NULL, NULL));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    }

    zlink_routing_id_t expected_rid;
    memset (&expected_rid, 0, sizeof (expected_rid));
    for (size_t i = 0; i != 3; ++i) {
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
        const zlink_routing_id_t *source_rid = NULL;
        zlink_part_flag_t has_more = ZLINK_PART_MORE;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_recv_part (stream, &source_rid, &part, &has_more,
                           ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_NOT_NULL (source_rid);
        TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, source_rid->size);
        if (i == 0)
            expected_rid = *source_rid;
        else
            TEST_ASSERT_EQUAL_UINT8_ARRAY (
              expected_rid.data, source_rid->data, stream_routing_id_size);
        TEST_ASSERT_EQUAL_UINT64 (strlen (payloads[i]),
                                  zlink_msg_size (&part));
        TEST_ASSERT_EQUAL_MEMORY (payloads[i], zlink_msg_data (&part),
                                  strlen (payloads[i]));
        TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    }

    test_context_socket_close_zero_linger (peer);
    test_context_socket_close_zero_linger (stream);
}

#if defined(ZLINK_HAVE_WINDOWS)
void test_stream_rejects_unsupported_send_without_poisoning_routed_final ()
{
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
}
#else
void test_stream_rejects_unsupported_send_without_poisoning_routed_final ()
{
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream);
    zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_RAW;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (stream, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));

    const int recv_timeout_ms = 3000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (stream, ZLINK_OPT_RCVTIMEO, &recv_timeout_ms,
                        sizeof (recv_timeout_ms)));
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (stream, endpoint, sizeof (endpoint));
    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_fd, recv_timeout_ms));

    const unsigned char hello[] = "hello";
    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet (client_fd, hello, sizeof (hello) - 1));

    zlink_msg_t incoming;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&incoming));
    const zlink_routing_id_t *source_rid = NULL;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv_part (stream, &source_rid, &incoming, &has_more,
                       ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, source_rid->size);
    zlink_routing_id_t target = *source_rid;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&incoming));

    zlink_msg_t unrouted;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&unrouted, 3));
    memcpy (zlink_msg_data (&unrouted), "BAD", 3);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_SUPPORTED,
      zlink_send_part (stream, &unrouted, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&unrouted));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&unrouted));

    const zlink_send_flags_t flags[] = {
      ZLINK_SEND_FLAGS_NONE, ZLINK_SEND_FLAGS_DONTWAIT};
    for (size_t i = 0; i < sizeof (flags) / sizeof (flags[0]); ++i) {
        zlink_msg_t prefix;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&prefix, 3));
        memcpy (zlink_msg_data (&prefix), "BAD", 3);
        zlink_completion_id_t completion_id = 99;
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_NOT_SUPPORTED,
          zlink_send_part_rid (stream, &target, &prefix, flags[i],
                               ZLINK_PART_MORE, NULL, &completion_id));
        TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
        TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
        TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&prefix));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&prefix));
    }

    const unsigned char expected[] = "GOOD";
    zlink_msg_t routed;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_msg_init_size (&routed, sizeof (expected) - 1));
    memcpy (zlink_msg_data (&routed), expected, sizeof (expected) - 1);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (stream, &target, &routed, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&routed));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&routed));

    unsigned char received[sizeof (expected) - 1];
    TEST_ASSERT_EQUAL_INT (
      0, recv_exact (client_fd, received, sizeof (received)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (expected, received, sizeof (received));

    close_raw_fd (client_fd);
    test_context_socket_close_zero_linger (stream);
}
#endif

#if defined(ZLINK_HAVE_WINDOWS)
void test_stream_notify_records_and_bind_constraint ()
{
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
}
#else
void test_stream_notify_records_and_bind_constraint ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    const int enable = 1;
    zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_RAW;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_NOTIFY, &enable, sizeof (enable)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    errno = 0;
    TEST_ASSERT_TRUE (
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_NOTIFY, &zero, sizeof (zero))
      != ZLINK_CONFIG_OK);
    TEST_ASSERT_EQUAL_INT (EBUSY, errno);

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);

    unsigned char connected_rid[stream_routing_id_size];
    TEST_ASSERT_TRUE (wait_stream_notify_record (server, connected_rid, 5000));

    close_raw_fd (client_fd);

    unsigned char disconnected_rid[stream_routing_id_size];
    TEST_ASSERT_TRUE (wait_stream_notify_record (server, disconnected_rid, 5000));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (connected_rid, disconnected_rid, stream_routing_id_size);

    test_context_socket_close_zero_linger (server);

    void *disabled = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (disabled);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (disabled, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (disabled, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_stream_option (disabled, ZLINK_STREAM_OPT_NOTIFY, &zero, sizeof (zero)));

    bind_loopback_ipv4 (disabled, endpoint, sizeof (endpoint));
    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
    void *monitor = zlink_socket_monitor_open (disabled, &monitor_opts);
    TEST_ASSERT_NOT_NULL (monitor);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (monitor, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    const int disabled_client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (disabled_client_fd >= 0);

    unsigned char ignored_rid[stream_routing_id_size];
    TEST_ASSERT_TRUE (
      wait_monitor_event (monitor, disabled, ZLINK_EVENT_CONNECTION_READY, ignored_rid, 5000));
    TEST_ASSERT_FALSE (wait_stream_notify_record (disabled, ignored_rid, 100));

    close_raw_fd (disabled_client_fd);

    TEST_ASSERT_TRUE (
      wait_monitor_event (monitor, disabled, ZLINK_EVENT_DISCONNECTED, ignored_rid, 5000));
    TEST_ASSERT_FALSE (wait_stream_notify_record (disabled, ignored_rid, 100));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (disabled);
}
#endif

void test_stream_recv_ready_precedes_first_payload_contract ()
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_IO_THREADS, 8));

    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_regression_socket (server, 256);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
    void *monitor = zlink_socket_monitor_open (server, &monitor_opts);
    TEST_ASSERT_NOT_NULL (monitor);
    const int monitor_linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (monitor, ZLINK_OPT_LINGER, &monitor_linger, sizeof (monitor_linger)));

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_fd, 3000));

    const unsigned char payload[] = "stream-recv-ready-contract";
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet (client_fd, payload, sizeof (payload) - 1));

    zlink_routing_id_t rid;
    zlink_msg_t payload_msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&payload_msg));
    TEST_ASSERT_TRUE (recv_stream_routing_id_and_payload (server, &rid, &payload_msg, 0));

    stream_ordering_probe_t ordering_probe;
    TEST_ASSERT_TRUE (wait_stream_ready_for_routing_id (monitor, &ordering_probe, &rid, 5000));
    TEST_ASSERT_TRUE (mark_stream_payload_begin (&ordering_probe, &rid, true));

    TEST_ASSERT_EQUAL_INT (
      static_cast<int> (sizeof (payload) - 1),
      TEST_ASSERT_SUCCESS_ERRNO (test_stream_send_single_msg (server, &rid, &payload_msg, 0)));
    mark_stream_payload_end (&ordering_probe, &rid);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&payload_msg));

    unsigned char echoed[sizeof (payload)];
    TEST_ASSERT_EQUAL_INT (static_cast<int> (sizeof (payload) - 1),
                           recv_stream_packet (client_fd, echoed, sizeof (echoed)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (payload, echoed,
                                   static_cast<unsigned int> (sizeof (payload) - 1));

    close_raw_fd (client_fd);

    TEST_ASSERT_TRUE (
      wait_stream_ordering_sets_with_monitor (monitor, &ordering_probe, 1, 1, 5000));
    TEST_ASSERT_EQUAL_INT (0, ordering_probe.payload_before_ready.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (
      0, ordering_probe.disconnect_during_payload.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (1, ordering_probe.strict_accept_count.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, ordering_probe.strict_drop_count.load (std::memory_order_acquire));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (server);
}

static void run_stream_raw_client_load (const char *endpoint_,
                                        uint32_t client_id_,
                                        int phases_,
                                        int messages_per_phase_,
                                        size_t payload_size_,
                                        std::atomic<int> *client_failures_)
{
    if (!endpoint_ || !client_failures_)
        return;

    std::vector<unsigned char> payload (payload_size_);
    std::vector<unsigned char> frame (payload_size_ + 4);
    std::vector<unsigned char> recv_frame (payload_size_ + 4);

    for (int phase = 0; phase < phases_; ++phase) {
        const int fd = connect_raw_tcp (endpoint_);
        if (fd < 0) {
            client_failures_->fetch_add (1, std::memory_order_release);
            return;
        }
        if (set_raw_fd_timeout (fd, 5000) != 0) {
            close_raw_fd (fd);
            client_failures_->fetch_add (1, std::memory_order_release);
            return;
        }

        for (int i = 0; i < messages_per_phase_; ++i) {
            const uint32_t seq = static_cast<uint32_t> (phase * messages_per_phase_ + i);
            store_u32_be (&payload[0], client_id_);
            store_u32_be (&payload[4], seq);
            for (size_t j = 8; j < payload_size_; ++j)
                payload[j] = static_cast<unsigned char> ((client_id_ + seq + j) & 0xFF);

            store_u32_be (&frame[0], static_cast<uint32_t> (payload_size_));
            memcpy (&frame[4], &payload[0], payload_size_);

            const size_t split =
              1 + ((static_cast<size_t> (client_id_) + seq) % (frame.size () - 1));
            if (send_stream_packet (fd, &frame[0], split) != 0
                || send_stream_packet (fd, &frame[split], frame.size () - split) != 0
                || recv_exact (fd, &recv_frame[0], recv_frame.size ()) != 0
                || load_u32_be (&recv_frame[0]) != payload_size_
                || memcmp (&recv_frame[4], &payload[0], payload_size_) != 0) {
                client_failures_->fetch_add (1, std::memory_order_release);
                break;
            }
        }

        close_raw_fd (fd);
        if (client_failures_->load (std::memory_order_acquire) > 0)
            return;
    }
}

static void run_stream_raw_client_once (const char *endpoint_,
                                        uint32_t client_id_,
                                        size_t payload_size_,
                                        std::atomic<int> *client_failures_)
{
    run_stream_raw_client_load (endpoint_, client_id_, 1, 1, payload_size_, client_failures_);
}

void test_stream_recv_multiclient_strict_ready_gating_regression ()
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_IO_THREADS, 8));

    const int client_count = 64;
    const size_t payload_size = 32;

    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_regression_socket (server, 2048);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
    void *monitor = zlink_socket_monitor_open (server, &monitor_opts);
    TEST_ASSERT_NOT_NULL (monitor);
    const int monitor_linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (monitor, ZLINK_OPT_LINGER, &monitor_linger, sizeof (monitor_linger)));

    std::atomic<int> client_failures (0);
    std::vector<std::thread> clients;
    clients.reserve (client_count);
    for (int i = 0; i < client_count; ++i) {
        clients.push_back (std::thread (run_stream_raw_client_once, endpoint,
                                        static_cast<uint32_t> (i), payload_size, &client_failures));
    }

    stream_ordering_probe_t ordering_probe;
    int received = 0;
    int send_fail = 0;
    while (received < client_count) {
        zlink_routing_id_t rid;
        zlink_msg_t payload_msg;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&payload_msg));
        TEST_ASSERT_TRUE (recv_stream_routing_id_and_payload (server, &rid, &payload_msg, 0));

        TEST_ASSERT_TRUE (wait_stream_ready_for_routing_id (monitor, &ordering_probe, &rid, 5000));
        TEST_ASSERT_TRUE (mark_stream_payload_begin (&ordering_probe, &rid, true));

        if (test_stream_send_single_msg (server, &rid, &payload_msg, 0) < 0) {
            ++send_fail;
        }
        mark_stream_payload_end (&ordering_probe, &rid);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&payload_msg));
        ++received;
    }

    for (size_t i = 0; i < clients.size (); ++i)
        clients[i].join ();

    TEST_ASSERT_TRUE (wait_stream_ordering_sets_with_monitor (monitor, &ordering_probe,
                                                              client_count, client_count, 10000));
    TEST_ASSERT_EQUAL_INT (0, client_failures.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, ordering_probe.payload_before_ready.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (
      0, ordering_probe.disconnect_during_payload.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, ordering_probe.strict_drop_count.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (client_count,
                           ordering_probe.strict_accept_count.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, send_fail);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (server);
}

void test_stream_phase3_mode_freeze_contract ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    zlink_stream_recv_mode_t observed = ZLINK_STREAM_RECV_MODE_RAW;
    size_t observed_size = sizeof (observed);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_get_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &observed,
                               &observed_size));
    TEST_ASSERT_EQUAL_INT (ZLINK_STREAM_RECV_MODE_UNSPECIFIED, observed);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (observed), observed_size);

    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_INVALID_ARGUMENT,
      zlink_bind (server, "tcp://127.0.0.1:*"));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());

    zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_RAW;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));

    errno = 0;
    TEST_ASSERT_TRUE (zlink_bind (server, "not-an-endpoint") != ZLINK_BIND_OK);
    TEST_ASSERT_TRUE (zlink_errno () != 0);

    mode = ZLINK_STREAM_RECV_MODE_PACKET;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));

    const int notify_enabled = 1;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_SUPPORTED,
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_NOTIFY,
                               &notify_enabled, sizeof (notify_enabled)));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_STATE,
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_STATE,
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_NOTIFY, &zero,
                               sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());

    void *client = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (client);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_INVALID_ARGUMENT,
                           zlink_connect (client, endpoint));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    mode = ZLINK_STREAM_RECV_MODE_RAW;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (client, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_INVALID_STATE,
      zlink_set_stream_option (client, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());

    zlink_msg_t raw_header;
    zlink_msg_t raw_body;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&raw_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&raw_body));
    const zlink_routing_id_t *raw_source_rid = NULL;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NOT_SUPPORTED,
      zlink_stream_recv_packet (client, &raw_source_rid, &raw_header,
                                &raw_body, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());
    TEST_ASSERT_NULL (raw_source_rid);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&raw_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&raw_body));

    void *notify_first = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (notify_first);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (notify_first, ZLINK_STREAM_OPT_NOTIFY,
                               &notify_enabled, sizeof (notify_enabled)));
    mode = ZLINK_STREAM_RECV_MODE_PACKET;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_NOT_SUPPORTED,
      zlink_set_stream_option (notify_first, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());
    observed = ZLINK_STREAM_RECV_MODE_RAW;
    observed_size = sizeof (observed);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_get_stream_option (notify_first, ZLINK_STREAM_OPT_RECV_MODE,
                               &observed, &observed_size));
    TEST_ASSERT_EQUAL_INT (ZLINK_STREAM_RECV_MODE_UNSPECIFIED, observed);

    test_context_socket_close_zero_linger (notify_first);
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

#if defined(ZLINK_HAVE_WINDOWS)
void test_stream_packet_peer_isolation_readiness_and_order ()
{
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
}
#else
void test_stream_packet_peer_isolation_readiness_and_order ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    const zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, server, server, ZLINK_POLLIN));

    const int peer_a_fd = connect_raw_tcp (endpoint);
    const int peer_b_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (peer_a_fd >= 0);
    TEST_ASSERT_TRUE (peer_b_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (peer_a_fd, 3000));
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (peer_b_fd, 3000));

    const unsigned char peer_a_header[] = "peer-a-large";
    const std::vector<unsigned char> peer_a_body (64 * 1024, 0xa5);
    const std::vector<unsigned char> peer_a_frame = build_stream_packet_frame (
      peer_a_header, sizeof (peer_a_header) - 1, &peer_a_body[0],
      peer_a_body.size ());
    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet (peer_a_fd, &peer_a_frame[0],
                             peer_a_frame.size () - 1));

    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (0,
                           zlink_poller_wait (poller, &event, 1, 50, NULL));

    struct packet_expectation_t
    {
        const char *header;
        const char *body;
    };
    const packet_expectation_t peer_b_packets[] = {
      {"peer-b-1", "first"}, {"peer-b-2", "second"}};
    const size_t peer_b_packet_count =
      sizeof (peer_b_packets) / sizeof (peer_b_packets[0]);

    for (size_t i = 0; i != peer_b_packet_count; ++i)
        TEST_ASSERT_EQUAL_INT (
          0, send_stream_packet_frame (
               peer_b_fd,
               reinterpret_cast<const unsigned char *> (peer_b_packets[i].header),
               strlen (peer_b_packets[i].header),
               reinterpret_cast<const unsigned char *> (peer_b_packets[i].body),
               strlen (peer_b_packets[i].body)));

    zlink_routing_id_t peer_b_rid;
    memset (&peer_b_rid, 0, sizeof (peer_b_rid));
    for (size_t i = 0; i != peer_b_packet_count; ++i) {
        memset (&event, 0, sizeof (event));
        TEST_ASSERT_EQUAL_INT (
          1, zlink_poller_wait (poller, &event, 1, i == 0 ? 3000 : 1000,
                                NULL));
        TEST_ASSERT_EQUAL_PTR (server, event.socket);
        TEST_ASSERT_TRUE ((event.events & ZLINK_POLLIN) != 0);

        zlink_msg_t header;
        zlink_msg_t body;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&header));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&body));
        const zlink_routing_id_t *source_rid = NULL;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_stream_recv_packet (server, &source_rid, &header, &body,
                                    ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_NOT_NULL (source_rid);
        TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, source_rid->size);
        if (i == 0)
            peer_b_rid = *source_rid;
        else
            TEST_ASSERT_EQUAL_UINT8_ARRAY (peer_b_rid.data, source_rid->data,
                                           peer_b_rid.size);
        const size_t expected_header_size = strlen (peer_b_packets[i].header);
        const size_t expected_body_size = strlen (peer_b_packets[i].body);
        TEST_ASSERT_EQUAL_UINT64 (expected_header_size, zlink_msg_size (&header));
        TEST_ASSERT_EQUAL_MEMORY (peer_b_packets[i].header,
                                  zlink_msg_data (&header), expected_header_size);
        TEST_ASSERT_EQUAL_UINT64 (expected_body_size, zlink_msg_size (&body));
        TEST_ASSERT_EQUAL_MEMORY (peer_b_packets[i].body,
                                  zlink_msg_data (&body), expected_body_size);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&header));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&body));
    }

    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (0,
                           zlink_poller_wait (poller, &event, 1, 50, NULL));

    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet (peer_a_fd, &peer_a_frame.back (), 1));
    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (
      1, zlink_poller_wait (poller, &event, 1, 3000, NULL));
    TEST_ASSERT_EQUAL_PTR (server, event.socket);
    TEST_ASSERT_TRUE ((event.events & ZLINK_POLLIN) != 0);

    zlink_msg_t header;
    zlink_msg_t body;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&body));
    const zlink_routing_id_t *source_rid = NULL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_stream_recv_packet (server, &source_rid, &header, &body,
                                ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, source_rid->size);
    TEST_ASSERT_TRUE (
      memcmp (source_rid->data, peer_b_rid.data, source_rid->size) != 0);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (peer_a_header) - 1,
                              zlink_msg_size (&header));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (peer_a_header, zlink_msg_data (&header),
                                   sizeof (peer_a_header) - 1);
    TEST_ASSERT_EQUAL_UINT64 (peer_a_body.size (), zlink_msg_size (&body));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (
      &peer_a_body[0], zlink_msg_data (&body),
      static_cast<unsigned int> (peer_a_body.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&body));

    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (0,
                           zlink_poller_wait (poller, &event, 1, 50, NULL));

    close_raw_fd (peer_b_fd);
    close_raw_fd (peer_a_fd);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (server);
}
#endif

#if defined(ZLINK_HAVE_WINDOWS)
void test_stream_phase3_packet_pull_contract ()
{
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
}
#else
void test_stream_phase3_packet_pull_contract ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    const uint64_t one_byte_hwm = 1;
    zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (server, ZLINK_OPT_RCVHWM, &one_byte_hwm,
                        sizeof (one_byte_hwm)));
    uint64_t observed_hwm = 0;
    size_t observed_hwm_size = sizeof (observed_hwm);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_get_option (server, ZLINK_OPT_RCVHWM, &observed_hwm,
                        &observed_hwm_size));
    TEST_ASSERT_EQUAL_UINT64 (sizeof (observed_hwm), observed_hwm_size);
    TEST_ASSERT_EQUAL_UINT64 (one_byte_hwm, observed_hwm);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, server, server, ZLINK_POLLIN));

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_fd, 2000));

    const unsigned char header_bytes[] = "hdr";
    const unsigned char body_bytes[] = "packet-body";
    const std::vector<unsigned char> first = build_stream_packet_frame (
      header_bytes, sizeof (header_bytes) - 1, body_bytes,
      sizeof (body_bytes) - 1);
    // The first record alone exceeds the one-byte receive credit. Appending a
    // second complete record to the same raw input exercises the pending-input
    // boundary: consuming the first record releases credit, keeps POLLIN set
    // for the second, and consuming the second clears POLLIN.
    const std::vector<unsigned char> second =
      build_stream_packet_frame (NULL, 0, NULL, 0);

    TEST_ASSERT_EQUAL_INT (0, send_stream_packet (client_fd, &first[0], 3));
    test_sleep_ms (20);

    zlink_msg_t partial_header;
    zlink_msg_t partial_body;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&partial_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&partial_body));
    const zlink_routing_id_t *source_rid =
      reinterpret_cast<const zlink_routing_id_t *> (0x1);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_stream_recv_packet (server, &source_rid, &partial_header,
                                &partial_body, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_PTR (
      reinterpret_cast<const zlink_routing_id_t *> (0x1), source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&partial_header));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&partial_body));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&partial_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&partial_body));

    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (poller, &event, 1, 50, NULL));

    std::vector<unsigned char> remaining (first.begin () + 3, first.end ());
    remaining.insert (remaining.end (), second.begin (), second.end ());
    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet (client_fd, &remaining[0], remaining.size ()));
    TEST_ASSERT_EQUAL_INT (1,
                           zlink_poller_wait (poller, &event, 1, 3000, NULL));
    TEST_ASSERT_EQUAL_PTR (server, event.socket);
    TEST_ASSERT_TRUE ((event.events & ZLINK_POLLIN) != 0);

    zlink_msg_t wrong_family;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&wrong_family));
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NOT_SUPPORTED,
      zlink_recv_part (server, &source_rid, &wrong_family, &has_more,
                       ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, zlink_errno ());
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&wrong_family));

    zlink_msg_t invalid_header;
    zlink_msg_t invalid_body;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&invalid_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&invalid_body, 1));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_STATE,
      zlink_stream_recv_packet (server, &source_rid, &invalid_header,
                                &invalid_body, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&invalid_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&invalid_body));

    zlink_msg_t header;
    zlink_msg_t body;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&body));
    source_rid = NULL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      wait_stream_packet_pull (server, &source_rid, &header, &body, 3000));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, source_rid->size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY (header_bytes, zlink_msg_data (&header),
                                   sizeof (header_bytes) - 1);
    TEST_ASSERT_EQUAL_UINT8_ARRAY (body_bytes, zlink_msg_data (&body),
                                   sizeof (body_bytes) - 1);

    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (1,
                           zlink_poller_wait (poller, &event, 1, 1000, NULL));
    TEST_ASSERT_TRUE ((event.events & ZLINK_POLLIN) != 0);

    zlink_msg_t empty_header;
    zlink_msg_t empty_body;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&empty_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&empty_body));
    source_rid = NULL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      wait_stream_packet_pull (server, &source_rid, &empty_header, &empty_body,
                               3000));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&empty_header));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&empty_body));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&empty_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&empty_body));

    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (poller, &event, 1, 50, NULL));

    // Every valid-handle packet-recv entry invalidates the previous borrowed
    // RID, even when a later output/flag validation fails. Those failures must
    // not overwrite the caller's source pointer or message content.
    const zlink_routing_id_t *borrowed_rid =
      send_and_pull_packet_probe (client_fd, server, 1);
    const zlink_routing_id_t *sentinel_rid =
      reinterpret_cast<const zlink_routing_id_t *> (0x1);
    zlink_msg_t validation_body;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&validation_body));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_HANDLE,
      zlink_stream_recv_packet (server, &sentinel_rid, NULL, &validation_body,
                                ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EFAULT, zlink_errno ());
    TEST_ASSERT_EQUAL_PTR (
      reinterpret_cast<const zlink_routing_id_t *> (0x1), sentinel_rid);
    TEST_ASSERT_EQUAL_UINT8 (0, borrowed_rid->size);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&validation_body));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&validation_body));

    borrowed_rid = send_and_pull_packet_probe (client_fd, server, 2);
    sentinel_rid = reinterpret_cast<const zlink_routing_id_t *> (0x1);
    zlink_msg_t validation_header;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&validation_header));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_HANDLE,
      zlink_stream_recv_packet (server, &sentinel_rid, &validation_header,
                                NULL, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EFAULT, zlink_errno ());
    TEST_ASSERT_EQUAL_PTR (
      reinterpret_cast<const zlink_routing_id_t *> (0x1), sentinel_rid);
    TEST_ASSERT_EQUAL_UINT8 (0, borrowed_rid->size);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&validation_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&validation_header));

    borrowed_rid = send_and_pull_packet_probe (client_fd, server, 3);
    sentinel_rid = reinterpret_cast<const zlink_routing_id_t *> (0x1);
    zlink_msg_t aliased_output;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&aliased_output));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_STATE,
      zlink_stream_recv_packet (server, &sentinel_rid, &aliased_output,
                                &aliased_output, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_PTR (
      reinterpret_cast<const zlink_routing_id_t *> (0x1), sentinel_rid);
    TEST_ASSERT_EQUAL_UINT8 (0, borrowed_rid->size);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&aliased_output));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&aliased_output));

    borrowed_rid = send_and_pull_packet_probe (client_fd, server, 4);
    sentinel_rid = reinterpret_cast<const zlink_routing_id_t *> (0x1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&validation_header, 3));
    memcpy (zlink_msg_data (&validation_header), "old", 3);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&validation_body));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_STATE,
      zlink_stream_recv_packet (server, &sentinel_rid, &validation_header,
                                &validation_body, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_PTR (
      reinterpret_cast<const zlink_routing_id_t *> (0x1), sentinel_rid);
    TEST_ASSERT_EQUAL_UINT8 (0, borrowed_rid->size);
    TEST_ASSERT_EQUAL_UINT64 (3, zlink_msg_size (&validation_header));
    TEST_ASSERT_EQUAL_MEMORY ("old", zlink_msg_data (&validation_header), 3);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&validation_body));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&validation_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&validation_body));

    borrowed_rid = send_and_pull_packet_probe (client_fd, server, 5);
    sentinel_rid = reinterpret_cast<const zlink_routing_id_t *> (0x1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&validation_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&validation_body));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_STATE,
      zlink_stream_recv_packet (
        server, &sentinel_rid, &validation_header, &validation_body,
        static_cast<zlink_recv_flags_t> (0x80)));
    TEST_ASSERT_EQUAL_INT (EINVAL, zlink_errno ());
    TEST_ASSERT_EQUAL_PTR (
      reinterpret_cast<const zlink_routing_id_t *> (0x1), sentinel_rid);
    TEST_ASSERT_EQUAL_UINT8 (0, borrowed_rid->size);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&validation_header));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&validation_body));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&validation_header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&validation_body));

    // Keep the queue at one-packet credit while a sustained ordered stream is
    // offered. The raw peer must hit TCP backpressure, then resume as public
    // packet pulls release credit; every sequence must arrive exactly once.
    const int requested_send_buffer = 4096;
    TEST_ASSERT_EQUAL_INT (
      0, setsockopt (client_fd, SOL_SOCKET, SO_SNDBUF,
                     &requested_send_buffer, sizeof (requested_send_buffer)));
    const int old_fd_flags = fcntl (client_fd, F_GETFL, 0);
    TEST_ASSERT_TRUE (old_fd_flags >= 0);
    TEST_ASSERT_EQUAL_INT (
      0, fcntl (client_fd, F_SETFL, old_fd_flags | O_NONBLOCK));

    const uint32_t pressure_packet_count = 128;
    const size_t pressure_body_size = 32 * 1024;
    const std::vector<unsigned char> pressure_body (pressure_body_size, 0xA5);
    std::vector<unsigned char> pressure_frame;
    size_t pressure_frame_offset = 0;
    uint32_t pressure_sent = 0;
    uint32_t pressure_received = 0;
    bool saw_sender_backpressure = false;
    bool saw_sender_resume_after_drain = false;

    const auto send_pressure_until_blocked = [&] () -> bool {
        bool progressed = false;
        while (pressure_sent < pressure_packet_count) {
            if (pressure_frame.empty ()) {
                unsigned char sequence_header[4];
                store_u32_be (sequence_header, pressure_sent);
                pressure_frame = build_stream_packet_frame (
                  sequence_header, sizeof (sequence_header),
                  &pressure_body[0], pressure_body.size ());
                pressure_frame_offset = 0;
            }

            const ssize_t written = send (
              client_fd, &pressure_frame[pressure_frame_offset],
              pressure_frame.size () - pressure_frame_offset, MSG_NOSIGNAL);
            if (written > 0) {
                progressed = true;
                if (saw_sender_backpressure && pressure_received != 0)
                    saw_sender_resume_after_drain = true;
                pressure_frame_offset += static_cast<size_t> (written);
                if (pressure_frame_offset == pressure_frame.size ()) {
                    pressure_frame.clear ();
                    pressure_frame_offset = 0;
                    ++pressure_sent;
                }
                continue;
            }
            if (written < 0 && errno == EINTR)
                continue;
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                saw_sender_backpressure = true;
                return progressed;
            }
            TEST_FAIL_MESSAGE ("raw STREAM pressure sender failed");
        }
        return progressed;
    };

    TEST_ASSERT_TRUE (send_pressure_until_blocked ());
    TEST_ASSERT_TRUE_MESSAGE (
      saw_sender_backpressure,
      "raw sender never observed bounded STREAM receive backpressure");

    const std::chrono::steady_clock::time_point pressure_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (15);
    while ((pressure_sent != pressure_packet_count
            || pressure_received != pressure_packet_count)
           && std::chrono::steady_clock::now () < pressure_deadline) {
        bool progressed = false;

        zlink_msg_t pressure_header;
        zlink_msg_t pressure_received_body;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&pressure_header));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&pressure_received_body));
        const zlink_routing_id_t *pressure_rid = NULL;
        const zlink_recv_result_t recv_rc = zlink_stream_recv_packet (
          server, &pressure_rid, &pressure_header, &pressure_received_body,
          ZLINK_RECV_FLAGS_DONTWAIT);
        if (recv_rc == ZLINK_RECV_OK) {
            progressed = true;
            TEST_ASSERT_NOT_NULL (pressure_rid);
            TEST_ASSERT_EQUAL_UINT64 (sizeof (uint32_t),
                                      zlink_msg_size (&pressure_header));
            TEST_ASSERT_EQUAL_UINT32 (
              pressure_received,
              load_u32_be (static_cast<const unsigned char *> (
                zlink_msg_data (&pressure_header))));
            TEST_ASSERT_EQUAL_UINT64 (
              pressure_body.size (),
              zlink_msg_size (&pressure_received_body));
            TEST_ASSERT_EQUAL_UINT8_ARRAY (
              &pressure_body[0], zlink_msg_data (&pressure_received_body),
              static_cast<unsigned int> (pressure_body.size ()));
            ++pressure_received;
        } else {
            TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, recv_rc);
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        }
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&pressure_header));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&pressure_received_body));

        if (pressure_sent != pressure_packet_count
            && send_pressure_until_blocked ())
            progressed = true;
        if (!progressed)
            test_sleep_ms (1);
    }
    TEST_ASSERT_EQUAL_UINT32 (pressure_packet_count, pressure_sent);
    TEST_ASSERT_EQUAL_UINT32 (pressure_packet_count, pressure_received);
    TEST_ASSERT_TRUE_MESSAGE (
      saw_sender_resume_after_drain,
      "raw sender did not resume after packet receive credit was released");

    close_raw_fd (client_fd);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (server);

    TEST_ASSERT_EQUAL_UINT8_ARRAY (header_bytes, zlink_msg_data (&header),
                                   sizeof (header_bytes) - 1);
    TEST_ASSERT_EQUAL_UINT8_ARRAY (body_bytes, zlink_msg_data (&body),
                                   sizeof (body_bytes) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&body));
}
#endif

#if defined(ZLINK_HAVE_WINDOWS)
void test_stream_phase3_packet_maxmsgsize_contract ()
{
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
}
#else
void test_stream_phase3_packet_maxmsgsize_contract ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    int64_t maxmsgsize = 6;
    zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (server, ZLINK_OPT_MAXMSGSIZE, &maxmsgsize,
                        sizeof (maxmsgsize)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
    void *monitor = zlink_socket_monitor_open (server, &monitor_opts);
    TEST_ASSERT_NOT_NULL (monitor);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (monitor, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, server, server, ZLINK_POLLIN));

    const int good_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (good_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (good_fd, 2000));
    unsigned char good_ready_rid[stream_routing_id_size];
    TEST_ASSERT_TRUE (
      wait_monitor_ready_edge_direct (monitor, good_ready_rid, 5000));

    const unsigned char snapshot_header[] = "abc";
    const unsigned char snapshot_body[] = "def";
    const std::vector<unsigned char> snapshot_frame = build_stream_packet_frame (
      snapshot_header, sizeof (snapshot_header) - 1, snapshot_body,
      sizeof (snapshot_body) - 1);
    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet (good_fd, &snapshot_frame[0], 6));

    zlink_poller_event_t event;
    memset (&event, 0, sizeof (event));
    TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (poller, &event, 1, 200, NULL));

    maxmsgsize = 4;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (server, ZLINK_OPT_MAXMSGSIZE, &maxmsgsize,
                        sizeof (maxmsgsize)));
    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet (good_fd, &snapshot_frame[6],
                             snapshot_frame.size () - 6));

    zlink_msg_t header;
    zlink_msg_t body;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&body));
    const zlink_routing_id_t *source_rid = NULL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      wait_stream_packet_pull (server, &source_rid, &header, &body, 3000));
    TEST_ASSERT_NOT_NULL (source_rid);
    zlink_routing_id_t good_rid = *source_rid;
    TEST_ASSERT_EQUAL_UINT8_ARRAY (snapshot_header, zlink_msg_data (&header),
                                   sizeof (snapshot_header) - 1);
    TEST_ASSERT_EQUAL_UINT8_ARRAY (snapshot_body, zlink_msg_data (&body),
                                   sizeof (snapshot_body) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&body));

    const uint16_t malformed_header_sizes[] = {5, 0, 3};
    const uint32_t malformed_body_sizes[] = {0, 5, 2};
    for (size_t case_index = 0; case_index < 3; ++case_index) {
        const int bad_fd = connect_raw_tcp (endpoint);
        TEST_ASSERT_TRUE (bad_fd >= 0);
        unsigned char bad_ready_rid[stream_routing_id_size];
        TEST_ASSERT_TRUE (
          wait_monitor_ready_edge_direct (monitor, bad_ready_rid, 5000));
        unsigned char prefix[6];
        store_u16_be (prefix, malformed_header_sizes[case_index]);
        store_u32_be (prefix + 2, malformed_body_sizes[case_index]);
        TEST_ASSERT_EQUAL_INT (0, send_stream_packet (bad_fd, prefix, sizeof (prefix)));
        TEST_ASSERT_TRUE (
          pump_packet_until_raw_fd_closed (server, bad_fd, 3000));
        TEST_ASSERT_TRUE (wait_monitor_event_direct_for_rid (
          monitor, ZLINK_EVENT_DISCONNECTED, bad_ready_rid, 5000));
        close_raw_fd (bad_fd);
    }

    const unsigned char final_header[] = "h";
    const unsigned char final_body[] = "ok";
    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet_frame (good_fd, final_header,
                                   sizeof (final_header) - 1, final_body,
                                   sizeof (final_body) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&body));
    source_rid = NULL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      wait_stream_packet_pull (server, &source_rid, &header, &body, 3000));
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT8_ARRAY (good_rid.data, source_rid->data,
                                   stream_routing_id_size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY (final_header, zlink_msg_data (&header),
                                   sizeof (final_header) - 1);
    TEST_ASSERT_EQUAL_UINT8_ARRAY (final_body, zlink_msg_data (&body),
                                   sizeof (final_body) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&header));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&body));

    // Both zero and negative MAXMSGSIZE values mean unlimited for newly
    // accepted packet-mode connections. Exercise header, body, and aggregate
    // sizes above the prior positive limit, and prove the connection remains
    // usable through the public packet pull API.
    const int64_t unlimited_values[] = {0, -1};
    for (size_t case_index = 0; case_index < 2; ++case_index) {
        maxmsgsize = unlimited_values[case_index];
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (server, ZLINK_OPT_MAXMSGSIZE, &maxmsgsize,
                            sizeof (maxmsgsize)));

        const int unlimited_fd = connect_raw_tcp (endpoint);
        TEST_ASSERT_TRUE (unlimited_fd >= 0);
        TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (unlimited_fd, 2000));
        unsigned char unlimited_ready_rid[stream_routing_id_size];
        TEST_ASSERT_TRUE (wait_monitor_ready_edge_direct (
          monitor, unlimited_ready_rid, 5000));

        const unsigned char unlimited_header[] = "header-above-six";
        const unsigned char unlimited_body[] = "body-above-six-as-well";
        TEST_ASSERT_EQUAL_INT (
          0, send_stream_packet_frame (
               unlimited_fd, unlimited_header, sizeof (unlimited_header) - 1,
               unlimited_body, sizeof (unlimited_body) - 1));

        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&header));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&body));
        source_rid = NULL;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          wait_stream_packet_pull (server, &source_rid, &header, &body, 3000));
        TEST_ASSERT_NOT_NULL (source_rid);
        TEST_ASSERT_EQUAL_UINT8_ARRAY (
          unlimited_ready_rid, source_rid->data, stream_routing_id_size);
        TEST_ASSERT_EQUAL_UINT8_ARRAY (
          unlimited_header, zlink_msg_data (&header),
          sizeof (unlimited_header) - 1);
        TEST_ASSERT_EQUAL_UINT8_ARRAY (
          unlimited_body, zlink_msg_data (&body), sizeof (unlimited_body) - 1);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&header));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&body));

        close_raw_fd (unlimited_fd);
        TEST_ASSERT_TRUE (wait_monitor_event_direct_for_rid (
          monitor, ZLINK_EVENT_DISCONNECTED, unlimited_ready_rid, 5000));
    }

    close_raw_fd (good_fd);
    TEST_ASSERT_TRUE (wait_monitor_event_direct_for_rid (
      monitor, ZLINK_EVENT_DISCONNECTED, good_ready_rid, 5000));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (server);
}
#endif

#if defined(ZLINK_HAVE_WINDOWS)
void test_stream_phase3_packet_enqueue_close_race ()
{
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
}
#else
void test_stream_phase3_packet_enqueue_close_race ()
{
    const int iterations = 20;
    const int zero = 0;
    const unsigned char header_bytes[] = "race";
    const unsigned char body_bytes[] = "enqueue-close";

    for (int iteration = 0; iteration < iterations; ++iteration) {
        void *server = test_context_socket (ZLINK_SOCKET_STREAM);
        TEST_ASSERT_NOT_NULL (server);
        zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                                   sizeof (mode)));

        char endpoint[MAX_SOCKET_STRING];
        bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

        void *poller = zlink_poller_new ();
        TEST_ASSERT_NOT_NULL (poller);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (poller, server, server, ZLINK_POLLIN));

        const int client_fd = connect_raw_tcp (endpoint);
        TEST_ASSERT_TRUE (client_fd >= 0);

        std::atomic<bool> wait_entered (false);
        std::atomic<int> wait_rc (-2);
        std::atomic<int> wait_error (ZLINK_CONFIG_OK);
        std::atomic<int> wait_errno (0);
        std::thread poller_owner ([&] {
            zlink_poller_event_t event;
            memset (&event, 0, sizeof (event));
            zlink_config_result_t error = ZLINK_CONFIG_OK;
            wait_entered.store (true, std::memory_order_release);
            const int rc = zlink_poller_wait (poller, &event, 1, 500, &error);
            wait_errno.store (zlink_errno (), std::memory_order_release);
            wait_error.store (error, std::memory_order_release);
            wait_rc.store (rc, std::memory_order_release);
        });

        while (!wait_entered.load (std::memory_order_acquire))
            std::this_thread::yield ();
        test_sleep_ms (20);

        TEST_ASSERT_EQUAL_INT (
          0, send_stream_packet_frame (client_fd, header_bytes,
                                       sizeof (header_bytes) - 1, body_bytes,
                                       sizeof (body_bytes) - 1));

        zlink_close_result_t close_rc = ZLINK_CLOSE_BUSY;
        for (int attempt = 0; attempt < 2000; ++attempt) {
            errno = 0;
            close_rc = zlink_close (server);
            if (close_rc == ZLINK_CLOSE_OK)
                break;
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_BUSY, close_rc);
            TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
            test_sleep_ms (1);
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, close_rc);
        test_context_socket_mark_closed (server);

        poller_owner.join ();
        const int observed_wait_rc = wait_rc.load (std::memory_order_acquire);
        TEST_ASSERT_TRUE (observed_wait_rc == 0 || observed_wait_rc == 1
                          || observed_wait_rc == -1);
        if (observed_wait_rc == -1) {
            const int observed_error = wait_error.load (std::memory_order_acquire);
            const int observed_errno = wait_errno.load (std::memory_order_acquire);
            TEST_ASSERT_TRUE (
              observed_error == ZLINK_CONFIG_INVALID_HANDLE
              || observed_error == ZLINK_CONFIG_INVALID_STATE);
            TEST_ASSERT_TRUE (observed_errno == EFAULT || observed_errno == ESHUTDOWN
                              || observed_errno == ETERM);
        }

        close_raw_fd (client_fd);
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    }
}
#endif

int main (void)
{
    UNITY_BEGIN ();

    setup_test_environment ();

    if (should_run_stream_socket_test (
          "test_stream_no_data_recv_part_locks_raw_mode"))
        RUN_TEST (test_stream_no_data_recv_part_locks_raw_mode);
    if (should_run_stream_socket_test (
          "test_stream_successful_recv_part_locks_raw_mode"))
        RUN_TEST (test_stream_successful_recv_part_locks_raw_mode);
    if (should_run_stream_socket_test (
          "test_stream_raw_inproc_parts_are_independent_final_chunks"))
        RUN_TEST (test_stream_raw_inproc_parts_are_independent_final_chunks);
    if (should_run_stream_socket_test (
          "test_stream_rejects_unsupported_send_without_poisoning_routed_final"))
        RUN_TEST (
          test_stream_rejects_unsupported_send_without_poisoning_routed_final);
    if (should_run_stream_socket_test (
          "test_stream_notify_records_and_bind_constraint"))
        RUN_TEST (test_stream_notify_records_and_bind_constraint);
    if (should_run_stream_socket_test (
          "test_stream_recv_ready_precedes_first_payload_contract"))
        RUN_TEST (test_stream_recv_ready_precedes_first_payload_contract);
#if !defined(ZLINK_HAVE_WINDOWS)
    if (should_run_stream_socket_test (
          "test_stream_recv_multiclient_strict_ready_gating_regression"))
        RUN_TEST (test_stream_recv_multiclient_strict_ready_gating_regression);
#endif
    if (should_run_stream_socket_test ("test_stream_phase3_mode_freeze_contract"))
        RUN_TEST (test_stream_phase3_mode_freeze_contract);
    if (should_run_stream_socket_test ("test_stream_phase3_packet_pull_contract"))
        RUN_TEST (test_stream_phase3_packet_pull_contract);
    if (should_run_stream_socket_test (
          "test_stream_packet_peer_isolation_readiness_and_order"))
        RUN_TEST (test_stream_packet_peer_isolation_readiness_and_order);
    if (should_run_stream_socket_test (
          "test_stream_phase3_packet_maxmsgsize_contract"))
        RUN_TEST (test_stream_phase3_packet_maxmsgsize_contract);
    if (should_run_stream_socket_test (
          "test_stream_phase3_packet_enqueue_close_race"))
        RUN_TEST (test_stream_phase3_packet_enqueue_close_race);

    return UNITY_END ();
}
