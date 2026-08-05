/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"

#include "utils/config.hpp"

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <condition_variable>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <vector>

#if defined(ZLINK_HAVE_WINDOWS)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

SETUP_TEARDOWN_TESTCONTEXT

static const size_t stream_routing_id_size = 4;
static const char *const stream_socket_smoke_cases[] = {
  "test_stream_callback_lifecycle",
  "test_stream_recv_api_dispatch_conflict",
  "test_stream_notify_option_remains_available_with_dispatch",
  "test_stream_callback_echo_raw",
  "test_stream_recv_ready_precedes_first_payload_contract",
  "test_stream_recv_handler_delivers_raw_chunks_not_len32be_frames",
  "test_stream_packet_handler_mode_gates",
  "test_stream_packet_framing_contracts",
  "test_stream_packet_disconnect_rid_closes_client",
  "test_stream_packet_ordering_contracts",
  "test_stream_packet_malformed_close_contracts",
  "test_stream_connect_rejected",
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

static void send_stream_msg (void *socket_,
                             const unsigned char routing_id_[stream_routing_id_size],
                             const void *data_,
                             size_t size_)
{
    TEST_ASSERT_EQUAL_INT (static_cast<int> (stream_routing_id_size),
                           TEST_ASSERT_SUCCESS_ERRNO (zlink_send (
                             socket_, routing_id_, stream_routing_id_size, ZLINK_SNDMORE)));
    TEST_ASSERT_EQUAL_INT ((int) size_,
                           TEST_ASSERT_SUCCESS_ERRNO (zlink_send (socket_, data_, size_, 0)));
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

    zlink_msg_t rid_msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&rid_msg));
    const int rid_rc = test_recv_single_msg (&rid_msg, socket_, flags_);
    if (rid_rc < 0) {
        zlink_msg_close (&rid_msg);
        return false;
    }

    if (zlink_msg_size (&rid_msg) != stream_routing_id_size) {
        zlink_msg_close (&rid_msg);
        errno = EPROTO;
        return false;
    }

    rid_out_->size = static_cast<uint8_t> (stream_routing_id_size);
    memcpy (rid_out_->data, zlink_msg_data (&rid_msg), stream_routing_id_size);
    zlink_msg_close (&rid_msg);

    const bool more = test_msg_has_more (&rid_msg);
    if (!more) {
        errno = EPROTO;
        return false;
    }

    return test_recv_single_msg (payload_out_, socket_, flags_) >= 0;
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

static void noop_stream_send_ready_handler (void *, void *)
{
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

struct stream_callback_probe_t
{
    stream_callback_probe_t () :
        socket (NULL),
        expected_payload (NULL),
        expected_size (0),
        calls (0),
        matched (0),
        send_ok (0),
        send_errno (0),
        routing_id_ready (0)
    {
        memset (routing_id, 0, sizeof (routing_id));
    }

    void *socket;
    const unsigned char *expected_payload;
    size_t expected_size;
    std::atomic<int> calls;
    std::atomic<int> matched;
    std::atomic<int> send_ok;
    std::atomic<int> send_errno;
    unsigned char routing_id[stream_routing_id_size];
    std::atomic<int> routing_id_ready;
};

static stream_callback_probe_t *g_stream_callback_probe = NULL;

struct stream_chunk_probe_t
{
    stream_chunk_probe_t () : chunk_sizes (), mutex (), cv () {}

    std::vector<size_t> chunk_sizes;
    std::mutex mutex;
    std::condition_variable cv;
};

static stream_chunk_probe_t *g_stream_chunk_probe = NULL;

struct stream_raw_stash_t
{
    stream_raw_stash_t () : data (), offset (0) {}

    std::vector<unsigned char> data;
    size_t offset;

    size_t available () const { return data.size () - offset; }

    void append (const unsigned char *src_, size_t size_)
    {
        if (!src_ || size_ == 0)
            return;
        if (offset >= data.size ()) {
            data.clear ();
            offset = 0;
        }
        const size_t old_size = data.size ();
        data.resize (old_size + size_);
        memcpy (&data[old_size], src_, size_);
    }

    void compact ()
    {
        if (offset == 0)
            return;
        if (offset >= data.size ()) {
            data.clear ();
            offset = 0;
            return;
        }
        if (offset > 4096 || offset * 2 >= data.size ()) {
            const size_t remain = data.size () - offset;
            memmove (&data[0], &data[offset], remain);
            data.resize (remain);
            offset = 0;
        }
    }

    void reset ()
    {
        data.clear ();
        offset = 0;
    }
};

struct stream_raw_load_probe_t
{
    stream_raw_load_probe_t () :
        socket (NULL),
        expected_payload_size (0),
        echoed_msgs (0),
        control_chunks (0),
        parse_error (0),
        send_fail (0),
        stashes (),
        stash_mu ()
    {
    }

    void *socket;
    size_t expected_payload_size;
    std::atomic<int> echoed_msgs;
    std::atomic<int> control_chunks;
    std::atomic<int> parse_error;
    std::atomic<int> send_fail;
    std::map<std::string, stream_raw_stash_t> stashes;
    std::mutex stash_mu;
};

static stream_raw_load_probe_t *g_stream_raw_load_probe = NULL;

struct stream_packet_record_t
{
    std::vector<unsigned char> header;
    std::vector<unsigned char> body;
    std::vector<unsigned char> source_rid;
};

struct stream_packet_probe_t
{
    stream_packet_probe_t () :
        socket (NULL),
        callbacks (0),
        blocked_once (false),
        block_first_callback (false),
        release_blocked_callback (false),
        blocked_rid_ready (false),
        blocked_callback_started (0),
        same_source_rid_seen (0),
        other_source_rid_seen (0)
    {
        memset (blocked_rid, 0, sizeof (blocked_rid));
    }

    void *socket;
    std::mutex mu;
    std::mutex block_mu;
    std::condition_variable cv;
    std::vector<stream_packet_record_t> records;
    std::atomic<int> callbacks;
    std::atomic<bool> blocked_once;
    bool block_first_callback;
    std::atomic<bool> release_blocked_callback;
    std::atomic<bool> blocked_rid_ready;
    std::atomic<int> blocked_callback_started;
    std::atomic<int> same_source_rid_seen;
    std::atomic<int> other_source_rid_seen;
    unsigned char blocked_rid[stream_routing_id_size];
};

static stream_packet_probe_t *g_stream_packet_probe = NULL;

static bool is_stream_control_chunk (const unsigned char *data_, size_t size_)
{
    (void) data_;
    return size_ == 0;
}

static void release_stream_callback_msg (zlink_msg_t *msg_)
{
    if (msg_)
        (void) zlink_msg_close (msg_);
}

static std::vector<unsigned char> copy_stream_msg_bytes (zlink_msg_t *msg_)
{
    std::vector<unsigned char> out;
    if (!msg_)
        return out;

    const size_t size = zlink_msg_size (msg_);
    const unsigned char *data = static_cast<const unsigned char *> (zlink_msg_data (msg_));
    if (size > 0 && data)
        out.assign (data, data + size);
    return out;
}

static bool stream_rid_matches (const zlink_routing_id_t *lhs_,
                                const unsigned char rhs_[stream_routing_id_size])
{
    if (!lhs_ || lhs_->size != stream_routing_id_size)
        return false;
    return memcmp (lhs_->data, rhs_, stream_routing_id_size) == 0;
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

static void record_stream_packet_message (const zlink_routing_id_t *rid_,
                                          zlink_msg_t *header_,
                                          zlink_msg_t *body_)
{
    stream_packet_probe_t *probe = g_stream_packet_probe;
    if (!probe)
        return;

    stream_packet_record_t record;
    if (rid_ && rid_->size == stream_routing_id_size)
        record.source_rid.assign (rid_->data, rid_->data + rid_->size);
    record.header = copy_stream_msg_bytes (header_);
    record.body = copy_stream_msg_bytes (body_);

    {
        std::lock_guard<std::mutex> lk (probe->mu);
        probe->records.push_back (record);
    }
    probe->callbacks.fetch_add (1, std::memory_order_release);
}

static void stream_packet_callback (
  void *, const zlink_routing_id_t *rid_, zlink_msg_t *header_, zlink_msg_t *body_, void *)
{
    stream_packet_probe_t *probe = g_stream_packet_probe;
    if (!probe || !rid_ || !header_ || !body_) {
        release_stream_callback_msg (header_);
        release_stream_callback_msg (body_);
        return;
    }

    record_stream_packet_message (rid_, header_, body_);

    const bool should_block = probe->block_first_callback
                              && !probe->blocked_once.exchange (true, std::memory_order_acq_rel);
    if (should_block) {
        if (rid_->size == stream_routing_id_size) {
            memcpy (probe->blocked_rid, rid_->data, stream_routing_id_size);
            probe->blocked_rid_ready.store (true, std::memory_order_release);
        }

        probe->blocked_callback_started.fetch_add (1, std::memory_order_release);

        std::unique_lock<std::mutex> lk (probe->block_mu);
        probe->cv.notify_all ();
        probe->cv.wait (lk, [&probe] () {
            return probe->release_blocked_callback.load (std::memory_order_acquire);
        });
    } else if (probe->blocked_rid_ready.load (std::memory_order_acquire)
               && stream_rid_matches (rid_, probe->blocked_rid)) {
        probe->same_source_rid_seen.fetch_add (1, std::memory_order_release);
    } else if (probe->blocked_once.load (std::memory_order_acquire)) {
        probe->other_source_rid_seen.fetch_add (1, std::memory_order_release);
    }

    release_stream_callback_msg (header_);
    release_stream_callback_msg (body_);
}

static void assert_stream_packet_record (const stream_packet_record_t &record_,
                                         const char *header_,
                                         const char *body_)
{
    const size_t header_size = header_ ? strlen (header_) : 0;
    const size_t body_size = body_ ? strlen (body_) : 0;

    TEST_ASSERT_EQUAL_UINT64 (header_size, record_.header.size ());
    TEST_ASSERT_EQUAL_UINT64 (body_size, record_.body.size ());

    if (header_size > 0)
        TEST_ASSERT_EQUAL_MEMORY (header_, &record_.header[0], header_size);
    if (body_size > 0)
        TEST_ASSERT_EQUAL_MEMORY (body_, &record_.body[0], body_size);
    TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, record_.source_rid.size ());
}

static void record_stream_chunk_handler (const zlink_routing_id_t *,
                                         zlink_msg_t *parts_,
                                         size_t part_count_,
                                         void *)
{
    stream_chunk_probe_t *probe = g_stream_chunk_probe;
    if (!probe || !parts_)
        return;

    {
        std::lock_guard<std::mutex> lk (probe->mutex);
        for (size_t i = 0; i < part_count_; ++i) {
            const size_t chunk_size = zlink_msg_size (&parts_[i]);
            if (chunk_size > 0)
                probe->chunk_sizes.push_back (chunk_size);
            (void) zlink_msg_close (&parts_[i]);
        }
    }
    probe->cv.notify_all ();
}

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

struct stream_ordering_callback_probe_t
{
    stream_ordering_callback_probe_t () :
        socket (NULL),
        monitor (NULL),
        ordering (NULL),
        strict_gate (false),
        callback_payloads (0),
        send_fail (0),
        monitor_mu ()
    {
    }

    void *socket;
    void *monitor;
    stream_ordering_probe_t *ordering;
    bool strict_gate;
    std::atomic<int> callback_payloads;
    std::atomic<int> send_fail;
    std::mutex monitor_mu;
};

static stream_ordering_callback_probe_t *g_stream_raw_ordering_callback_probe = NULL;

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
    const uint64_t hwm = 10u * (4096u + sizeof (zlink_msg_t));
    const int timeout_ms = 200;
    const int nodelay = 1;
    const int backlog = backlog_ > 0 ? backlog_ : 256;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
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

static int
stream_raw_ordering_echo_callback (const zlink_routing_id_t *rid_, zlink_msg_t *msg_, void *)
{
    stream_ordering_callback_probe_t *probe = g_stream_raw_ordering_callback_probe;
    if (!probe || !probe->ordering || !rid_ || !msg_)
        return 0;

    const unsigned char *payload = static_cast<const unsigned char *> (zlink_msg_data (msg_));
    const size_t payload_size = zlink_msg_size (msg_);
    if (is_stream_control_chunk (payload, payload_size)) {
        release_stream_callback_msg (msg_);
        return 0;
    }

    probe->callback_payloads.fetch_add (1, std::memory_order_release);
    bool ready = !probe->strict_gate;
    if (probe->monitor) {
        std::lock_guard<std::mutex> lk (probe->monitor_mu);
        ready = wait_stream_ready_for_routing_id (probe->monitor, probe->ordering, rid_, 5000);
    }
    const bool marked_ready = mark_stream_payload_begin (probe->ordering, rid_, probe->strict_gate);
    ready = ready && marked_ready;

    if (!probe->strict_gate || ready) {
        const int send_rc = test_stream_send_single_msg (probe->socket, rid_, msg_, 0);
        if (send_rc != static_cast<int> (payload_size))
            probe->send_fail.fetch_add (1, std::memory_order_release);
    }

    mark_stream_payload_end (probe->ordering, rid_);
    release_stream_callback_msg (msg_);
    return 0;
}

static int
stream_echo_callback (const zlink_routing_id_t *rid_, zlink_msg_t *msgs_, size_t msg_count_)
{
    stream_callback_probe_t *probe = g_stream_callback_probe;
    if (!probe || !rid_ || !msgs_ || msg_count_ == 0)
        return 0;

    for (size_t i = 0; i < msg_count_; ++i) {
        zlink_msg_t *msg = &msgs_[i];
        probe->calls.fetch_add (1, std::memory_order_release);

        const unsigned char *data = static_cast<const unsigned char *> (zlink_msg_data (msg));
        const size_t size = zlink_msg_size (msg);
        if (is_stream_control_chunk (data, size)) {
            release_stream_callback_msg (msg);
            continue;
        }

        if (!probe->expected_payload || size != probe->expected_size) {
            release_stream_callback_msg (msg);
            continue;
        }

        if (memcmp (data, probe->expected_payload, size) != 0) {
            release_stream_callback_msg (msg);
            continue;
        }

        if (rid_->size == stream_routing_id_size) {
            memcpy (probe->routing_id, rid_->data, stream_routing_id_size);
            probe->routing_id_ready.store (1, std::memory_order_release);
        }

        const int send_rc = test_stream_send_single_msg (probe->socket, rid_, msg, 0);
        if (send_rc == static_cast<int> (size))
            probe->send_ok.store (1, std::memory_order_release);
        else
            probe->send_errno.store (errno, std::memory_order_release);

        probe->matched.fetch_add (1, std::memory_order_release);
        release_stream_callback_msg (msg);
    }
    return 0;
}

static int
stream_load_echo_callback (const zlink_routing_id_t *rid_, zlink_msg_t *msgs_, size_t msg_count_)
{
    LIBZLINK_UNUSED (rid_);
    LIBZLINK_UNUSED (msgs_);
    LIBZLINK_UNUSED (msg_count_);
    return 0;
}

static int stream_raw_load_echo_callback (const zlink_routing_id_t *rid_,
                                          zlink_msg_t *msgs_,
                                          size_t msg_count_)
{
    stream_raw_load_probe_t *probe = g_stream_raw_load_probe;
    if (!probe || !rid_ || !msgs_ || msg_count_ == 0)
        return 0;

    for (size_t i = 0; i < msg_count_; ++i) {
        zlink_msg_t *msg = &msgs_[i];
        const unsigned char *payload = static_cast<const unsigned char *> (zlink_msg_data (msg));
        const size_t payload_size = zlink_msg_size (msg);

        if (is_stream_control_chunk (payload, payload_size)) {
            probe->control_chunks.fetch_add (1, std::memory_order_release);
            release_stream_callback_msg (msg);
            continue;
        }

        if ((!payload && payload_size > 0) || payload_size == 0) {
            probe->parse_error.fetch_add (1, std::memory_order_release);
            release_stream_callback_msg (msg);
            continue;
        }

        std::vector<std::vector<unsigned char>> complete_frames;
        bool invalid = false;
        {
            const std::string key (reinterpret_cast<const char *> (rid_->data), rid_->size);
            std::lock_guard<std::mutex> lk (probe->stash_mu);
            stream_raw_stash_t &stash = probe->stashes[key];
            stash.append (payload, payload_size);

            size_t consumed = 0;
            const size_t available = stash.available ();
            while (consumed + 4 <= available) {
                const unsigned char *frame = &stash.data[stash.offset + consumed];
                const size_t body_size = static_cast<size_t> (load_u32_be (frame));

                if (body_size < 8 || body_size > 4 * 1024 * 1024
                    || (probe->expected_payload_size > 0
                        && body_size != probe->expected_payload_size)) {
                    invalid = true;
                    break;
                }

                const size_t frame_size = 4 + body_size;
                if (consumed + frame_size > available)
                    break;

                complete_frames.push_back (std::vector<unsigned char> (frame_size));
                memcpy (&complete_frames.back ()[0], frame, frame_size);
                consumed += frame_size;
            }

            if (invalid) {
                stash.reset ();
            } else if (consumed > 0) {
                stash.offset += consumed;
                stash.compact ();
            }
        }

        if (invalid) {
            probe->parse_error.fetch_add (1, std::memory_order_release);
            release_stream_callback_msg (msg);
            continue;
        }

        for (size_t j = 0; j < complete_frames.size (); ++j) {
            std::vector<unsigned char> &frame = complete_frames[j];
            const int send_rc =
              test_stream_send_bytes (probe->socket, rid_, &frame[0], frame.size (), 0);
            if (send_rc != static_cast<int> (frame.size ())) {
                probe->send_fail.fetch_add (1, std::memory_order_release);
                continue;
            }
            probe->echoed_msgs.fetch_add (1, std::memory_order_release);
        }

        release_stream_callback_msg (msg);
    }

    return 0;
}

static int stream_echo_raw_callback (const zlink_routing_id_t *rid_, zlink_msg_t *msg_, void *)
{
    return stream_echo_callback (rid_, msg_, 1);
}

static int
stream_raw_load_echo_raw_callback (const zlink_routing_id_t *rid_, zlink_msg_t *msg_, void *)
{
    return stream_raw_load_echo_callback (rid_, msg_, 1);
}

static void stream_raw_ordering_msg_handler (const zlink_routing_id_t *rid_,
                                             zlink_msg_t *parts_,
                                             size_t part_count_,
                                             void *userdata_)
{
    LIBZLINK_UNUSED (userdata_);
    for (size_t i = 0; i < part_count_; ++i)
        stream_raw_ordering_echo_callback (rid_, &parts_[i], NULL);
}

static void stream_echo_msg_handler (const zlink_routing_id_t *rid_,
                                     zlink_msg_t *parts_,
                                     size_t part_count_,
                                     void *userdata_)
{
    LIBZLINK_UNUSED (userdata_);
    stream_echo_callback (rid_, parts_, part_count_);
}

static void stream_raw_load_msg_handler (const zlink_routing_id_t *rid_,
                                         zlink_msg_t *parts_,
                                         size_t part_count_,
                                         void *userdata_)
{
    LIBZLINK_UNUSED (userdata_);
    stream_raw_load_echo_callback (rid_, parts_, part_count_);
}

static int attach_stream_msg_handler (void *socket_, zlink_socket_msg_handler_fn handler_)
{
    return zlink_recv_handler (socket_, handler_, NULL);
}

void test_stream_callback_lifecycle ()
{
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_NOT_NULL (sub);

    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_NOT_SUPPORTED,
                           attach_stream_msg_handler (sub, stream_echo_msg_handler));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    test_context_socket_close_zero_linger (sub);

    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream);

    stream_callback_probe_t probe;
    probe.socket = stream;

    g_stream_callback_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (attach_stream_msg_handler (stream, stream_echo_msg_handler));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_BUSY,
                           attach_stream_msg_handler (stream, stream_echo_msg_handler));
    TEST_ASSERT_EQUAL_INT (EBUSY, errno);
    g_stream_callback_probe = NULL;
    test_context_socket_close_zero_linger (stream);
}

void test_stream_recv_api_dispatch_conflict ()
{
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream);

    unsigned char buf[16];
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, zlink_recv (stream, buf, sizeof (buf), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    zlink_msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, test_recv_single_msg (&msg, stream, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));

    TEST_ASSERT_SUCCESS_ERRNO (attach_stream_msg_handler (stream, stream_echo_msg_handler));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, zlink_recv (stream, buf, sizeof (buf), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EBUSY, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, test_recv_single_msg (&msg, stream, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EBUSY, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));

    test_context_socket_close_zero_linger (stream);
}

void test_stream_notify_option_remains_available_with_dispatch ()
{
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream);

    const int enable = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_stream_option (stream, ZLINK_STREAM_OPT_NOTIFY, &enable, sizeof (enable)));

    int actual = 0;
    size_t actual_size = sizeof (actual);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_stream_option (stream, ZLINK_STREAM_OPT_NOTIFY, &actual, &actual_size));
    TEST_ASSERT_EQUAL_INT (sizeof (actual), actual_size);
    TEST_ASSERT_EQUAL_INT (1, actual);

    TEST_ASSERT_SUCCESS_ERRNO (attach_stream_msg_handler (stream, stream_echo_msg_handler));

    actual = 0;
    actual_size = sizeof (actual);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_stream_option (stream, ZLINK_STREAM_OPT_NOTIFY, &actual, &actual_size));
    TEST_ASSERT_EQUAL_INT (sizeof (actual), actual_size);
    TEST_ASSERT_EQUAL_INT (1, actual);

    test_context_socket_close_zero_linger (stream);
}

#if defined(ZLINK_HAVE_WINDOWS)
void test_stream_callback_echo_raw ()
{
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
}

void test_stream_callback_echo_single_zero_byte ()
{
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
}
#else
void test_stream_callback_echo_raw ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_fd, 10000));

    const unsigned char payload[] = "stream-callback-raw";
    stream_callback_probe_t probe;
    probe.socket = server;
    probe.expected_payload = payload;
    probe.expected_size = sizeof (payload) - 1;

    g_stream_callback_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (attach_stream_msg_handler (server, stream_echo_msg_handler));
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet (client_fd, payload, sizeof (payload) - 1));

    unsigned char recv_buf[sizeof (payload)];
    TEST_ASSERT_EQUAL_INT (0, recv_exact (client_fd, recv_buf, sizeof (payload) - 1));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (payload, recv_buf,
                                   static_cast<unsigned int> (sizeof (payload) - 1));

    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.matched, 1, 3000));
    TEST_ASSERT_EQUAL_INT (1, probe.send_ok.load (std::memory_order_acquire));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.routing_id_ready, 1, 3000));

    const unsigned char two_part_reply[] = "stream-direct-2part";
    TEST_ASSERT_EQUAL_INT (static_cast<int> (stream_routing_id_size),
                           TEST_ASSERT_SUCCESS_ERRNO (zlink_send (
                             server, probe.routing_id, stream_routing_id_size, ZLINK_SNDMORE)));
    TEST_ASSERT_EQUAL_INT (static_cast<int> (sizeof (two_part_reply) - 1),
                           TEST_ASSERT_SUCCESS_ERRNO (
                             zlink_send (server, two_part_reply, sizeof (two_part_reply) - 1, 0)));

    unsigned char recv_two_part[sizeof (two_part_reply)];
    TEST_ASSERT_EQUAL_INT (0, recv_exact (client_fd, recv_two_part, sizeof (two_part_reply) - 1));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (two_part_reply, recv_two_part,
                                   static_cast<unsigned int> (sizeof (two_part_reply) - 1));

    g_stream_callback_probe = NULL;

    close_raw_fd (client_fd);
    test_context_socket_close_zero_linger (server);
}

void test_stream_callback_echo_single_zero_byte ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_fd, 10000));

    const unsigned char payload[1] = {0x00};
    stream_callback_probe_t probe;
    probe.socket = server;
    probe.expected_payload = payload;
    probe.expected_size = sizeof (payload);

    g_stream_callback_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (attach_stream_msg_handler (server, stream_echo_msg_handler));
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet (client_fd, payload, sizeof (payload)));

    unsigned char recv_buf[sizeof (payload)];
    TEST_ASSERT_EQUAL_INT (0, recv_exact (client_fd, recv_buf, sizeof (payload)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (payload, recv_buf, static_cast<unsigned int> (sizeof (payload)));

    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.matched, 1, 3000));
    TEST_ASSERT_EQUAL_INT (1, probe.send_ok.load (std::memory_order_acquire));

    g_stream_callback_probe = NULL;
    close_raw_fd (client_fd);
    test_context_socket_close_zero_linger (server);
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

void test_stream_raw_callback_ready_precedes_first_payload_contract ()
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

    stream_ordering_probe_t ordering_probe;

    stream_ordering_callback_probe_t callback_probe;
    callback_probe.socket = server;
    callback_probe.monitor = monitor;
    callback_probe.ordering = &ordering_probe;
    callback_probe.strict_gate = true;
    g_stream_raw_ordering_callback_probe = &callback_probe;
    TEST_ASSERT_SUCCESS_ERRNO (attach_stream_msg_handler (server, stream_raw_ordering_msg_handler));

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_fd, 3000));

    const unsigned char payload[] = "stream-raw-ready-contract";
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet (client_fd, payload, sizeof (payload) - 1));

    unsigned char echoed[sizeof (payload)];
    TEST_ASSERT_EQUAL_INT (static_cast<int> (sizeof (payload) - 1),
                           recv_stream_packet (client_fd, echoed, sizeof (echoed)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (payload, echoed,
                                   static_cast<unsigned int> (sizeof (payload) - 1));

    close_raw_fd (client_fd);

    TEST_ASSERT_TRUE (wait_stream_ordering_counter (&callback_probe.callback_payloads, 1, 5000));
    TEST_ASSERT_TRUE (
      wait_stream_ordering_sets_with_monitor (monitor, &ordering_probe, 1, 1, 5000));
    TEST_ASSERT_EQUAL_INT (0, ordering_probe.payload_before_ready.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (
      0, ordering_probe.disconnect_during_payload.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (1, ordering_probe.strict_accept_count.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, ordering_probe.strict_drop_count.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, callback_probe.send_fail.load (std::memory_order_acquire));

    g_stream_raw_ordering_callback_probe = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (server);
}

void test_stream_recv_handler_delivers_raw_chunks_not_len32be_frames ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_regression_socket (server, 256);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    stream_chunk_probe_t probe;
    g_stream_chunk_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv_handler (server, &record_stream_chunk_handler, NULL));

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_fd, 3000));

    const unsigned char header[4] = {0x00, 0x00, 0x00, 0x08};
    const unsigned char payload[8] = {'r', 'a', 'w', '-', 'c', 'h', 'u', 'n'};

    TEST_ASSERT_EQUAL_INT (0, send_all (client_fd, header, sizeof (header)));
    {
        std::unique_lock<std::mutex> lk (probe.mutex);
        TEST_ASSERT_TRUE (probe.cv.wait_for (lk, std::chrono::seconds (5),
                                             [&probe] () { return !probe.chunk_sizes.empty (); }));
        TEST_ASSERT_EQUAL_UINT (static_cast<unsigned int> (sizeof (header)),
                                static_cast<unsigned int> (probe.chunk_sizes[0]));
    }

    TEST_ASSERT_EQUAL_INT (0, send_all (client_fd, payload, sizeof (payload)));
    {
        std::unique_lock<std::mutex> lk (probe.mutex);
        TEST_ASSERT_TRUE (probe.cv.wait_for (
          lk, std::chrono::seconds (5), [&probe] () { return probe.chunk_sizes.size () >= 2; }));
        TEST_ASSERT_EQUAL_UINT (static_cast<unsigned int> (sizeof (payload)),
                                static_cast<unsigned int> (probe.chunk_sizes[1]));
    }

    g_stream_chunk_probe = NULL;
    close_raw_fd (client_fd);
    test_context_socket_close_zero_linger (server);
#endif
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

void test_stream_raw_multiclient_strict_ready_gating_regression ()
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

    stream_ordering_probe_t ordering_probe;

    stream_ordering_callback_probe_t callback_probe;
    callback_probe.socket = server;
    callback_probe.monitor = monitor;
    callback_probe.ordering = &ordering_probe;
    callback_probe.strict_gate = true;
    g_stream_raw_ordering_callback_probe = &callback_probe;
    TEST_ASSERT_SUCCESS_ERRNO (attach_stream_msg_handler (server, stream_raw_ordering_msg_handler));

    std::atomic<int> client_failures (0);
    std::vector<std::thread> clients;
    clients.reserve (client_count);
    for (int i = 0; i < client_count; ++i) {
        clients.push_back (std::thread (run_stream_raw_client_once, endpoint,
                                        static_cast<uint32_t> (i), payload_size, &client_failures));
    }
    for (size_t i = 0; i < clients.size (); ++i)
        clients[i].join ();

    TEST_ASSERT_TRUE (
      wait_stream_ordering_counter (&callback_probe.callback_payloads, client_count, 10000));
    TEST_ASSERT_TRUE (wait_stream_ordering_sets_with_monitor (monitor, &ordering_probe,
                                                              client_count, client_count, 10000));
    TEST_ASSERT_EQUAL_INT (0, client_failures.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, ordering_probe.payload_before_ready.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (
      0, ordering_probe.disconnect_during_payload.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, ordering_probe.strict_drop_count.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (client_count,
                           ordering_probe.strict_accept_count.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, callback_probe.send_fail.load (std::memory_order_acquire));

    g_stream_raw_ordering_callback_probe = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (server);
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

void test_stream_raw_multiclient_load_integrity ()
{
    const int client_count = 48;
    const int phases = 2;
    const int messages_per_phase = 48;
    const size_t payload_size = 192;
    const int expected_msgs = client_count * phases * messages_per_phase;

    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    stream_raw_load_probe_t probe;
    probe.socket = server;
    probe.expected_payload_size = payload_size;
    g_stream_raw_load_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (attach_stream_msg_handler (server, stream_raw_load_msg_handler));

    std::atomic<int> client_failures (0);
    std::vector<std::thread> clients;
    clients.reserve (client_count);
    for (int i = 0; i < client_count; ++i) {
        clients.push_back (std::thread (run_stream_raw_client_load, endpoint,
                                        static_cast<uint32_t> (i), phases, messages_per_phase,
                                        payload_size, &client_failures));
    }
    for (size_t i = 0; i < clients.size (); ++i)
        clients[i].join ();

    TEST_ASSERT_EQUAL_INT (0, client_failures.load (std::memory_order_acquire));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.echoed_msgs, expected_msgs, 10000));
    TEST_ASSERT_EQUAL_INT (0, probe.parse_error.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, probe.send_fail.load (std::memory_order_acquire));

    g_stream_raw_load_probe = NULL;
    test_context_socket_close_zero_linger (server);
}

void test_stream_raw_multiclient_load_integrity_with_send_ready_handler ()
{
    const int client_count = 48;
    const int phases = 2;
    const int messages_per_phase = 48;
    const size_t payload_size = 192;
    const int expected_msgs = client_count * phases * messages_per_phase;

    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send_ready_handler (server, &noop_stream_send_ready_handler, NULL));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    stream_raw_load_probe_t probe;
    probe.socket = server;
    probe.expected_payload_size = payload_size;
    g_stream_raw_load_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (attach_stream_msg_handler (server, stream_raw_load_msg_handler));

    std::atomic<int> client_failures (0);
    std::vector<std::thread> clients;
    clients.reserve (client_count);
    for (int i = 0; i < client_count; ++i) {
        clients.push_back (std::thread (run_stream_raw_client_load, endpoint,
                                        static_cast<uint32_t> (i), phases, messages_per_phase,
                                        payload_size, &client_failures));
    }
    for (size_t i = 0; i < clients.size (); ++i)
        clients[i].join ();

    TEST_ASSERT_EQUAL_INT (0, client_failures.load (std::memory_order_acquire));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.echoed_msgs, expected_msgs, 10000));
    TEST_ASSERT_EQUAL_INT (0, probe.parse_error.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, probe.send_fail.load (std::memory_order_acquire));

    g_stream_raw_load_probe = NULL;
    test_context_socket_close_zero_linger (server);
}

void test_stream_tcp_basic ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
    void *monitor = zlink_socket_monitor_open (server, &monitor_opts);
    TEST_ASSERT_NOT_NULL (monitor);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (monitor, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    stream_callback_probe_t probe;
    probe.socket = server;
    const unsigned char payload[] = "hello";
    probe.expected_payload = payload;
    probe.expected_size = sizeof (payload) - 1;
    g_stream_callback_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (attach_stream_msg_handler (server, stream_echo_msg_handler));

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet (client_fd, payload, sizeof (payload) - 1));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.matched, 1, 5000));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.send_ok, 1, 5000));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.routing_id_ready, 1, 5000));

    unsigned char server_id[stream_routing_id_size];
    TEST_ASSERT_TRUE (
      wait_monitor_event (monitor, server, ZLINK_EVENT_CONNECTION_READY, server_id, 2000));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (server_id, probe.routing_id, stream_routing_id_size);

    char client_recv_buf[64];
    const int rc = recv_stream_packet (client_fd, client_recv_buf, sizeof (client_recv_buf));
    TEST_ASSERT_EQUAL_INT ((int) sizeof (payload) - 1, rc);
    TEST_ASSERT_EQUAL_STRING_LEN (reinterpret_cast<const char *> (payload), client_recv_buf,
                                  sizeof (payload) - 1);

    close_raw_fd (client_fd);

    TEST_ASSERT_TRUE (
      wait_monitor_event (monitor, server, ZLINK_EVENT_DISCONNECTED, server_id, 10000));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (probe.routing_id, server_id, stream_routing_id_size);

    g_stream_callback_probe = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (server);
}

void test_stream_maxmsgsize ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    const int64_t maxmsgsize = 4;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_MAXMSGSIZE, &maxmsgsize, sizeof (maxmsgsize)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
    void *monitor = zlink_socket_monitor_open (server, &monitor_opts);
    TEST_ASSERT_NOT_NULL (monitor);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (monitor, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    stream_callback_probe_t probe;
    probe.socket = server;
    const unsigned char probe_payload[] = "ok";
    probe.expected_payload = probe_payload;
    probe.expected_size = sizeof (probe_payload) - 1;
    g_stream_callback_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (attach_stream_msg_handler (server, stream_echo_msg_handler));

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);

    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet (client_fd, probe_payload, sizeof (probe_payload) - 1));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.matched, 1, 5000));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.send_ok, 1, 5000));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.routing_id_ready, 1, 5000));

    unsigned char connect_id[stream_routing_id_size];
    TEST_ASSERT_TRUE (
      wait_monitor_event (monitor, server, ZLINK_EVENT_CONNECTION_READY, connect_id, 2000));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (connect_id, probe.routing_id, stream_routing_id_size);

    char client_probe_buf[16];
    const int rc = recv_stream_packet (client_fd, client_probe_buf, sizeof (client_probe_buf));
    TEST_ASSERT_EQUAL_INT ((int) sizeof (probe_payload) - 1, rc);
    TEST_ASSERT_EQUAL_STRING_LEN (reinterpret_cast<const char *> (probe_payload), client_probe_buf,
                                  sizeof (probe_payload) - 1);

    char payload[1024];
    memset (payload, 'A', sizeof (payload));
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet (client_fd, payload, sizeof (payload)));

    unsigned char recv_id[stream_routing_id_size];
    TEST_ASSERT_TRUE (
      wait_monitor_event (monitor, server, ZLINK_EVENT_DISCONNECTED, recv_id, 10000));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (probe.routing_id, recv_id, stream_routing_id_size);

    close_raw_fd (client_fd);
    g_stream_callback_probe = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (server);
}

void test_stream_packet_handler_mode_gates ()
{
    void *ctx = get_test_context ();
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (ctx, ZLINK_IO_THREADS, 1));

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);

    void *stream_recv = test_context_socket (ZLINK_SOCKET_STREAM);
    void *stream_packet = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream_recv);
    TEST_ASSERT_NOT_NULL (stream_packet);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv_handler (stream_recv, stream_raw_load_msg_handler, NULL));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_BUSY, zlink_stream_packet_handler (stream_recv, &stream_packet_callback, NULL));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
    TEST_ASSERT_TRUE (zlink_poller_add (poller, stream_recv, stream_recv, ZLINK_POLLIN)
                      != ZLINK_CONFIG_OK);
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_stream_packet_handler (stream_packet, &stream_packet_callback, NULL));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_BUSY,
                           zlink_recv_handler (stream_packet, stream_raw_load_msg_handler, NULL));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_HANDLER_BUSY, zlink_stream_packet_handler (
                                                 stream_packet, &stream_packet_callback, NULL));
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_TRUE (zlink_recv (stream_packet, NULL, &parts, &part_count, ZLINK_DONTWAIT)
                      != ZLINK_RECV_OK);
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());
    TEST_ASSERT_TRUE (zlink_poller_add (poller, stream_packet, stream_packet, ZLINK_POLLIN)
                      != ZLINK_CONFIG_OK);
    TEST_ASSERT_EQUAL_INT (EBUSY, zlink_errno ());

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_destroy (&poller));
    test_context_socket_close_zero_linger (stream_packet);
    test_context_socket_close_zero_linger (stream_recv);
}

void test_stream_packet_framing_contracts ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    stream_packet_probe_t probe;
    probe.socket = server;
    g_stream_packet_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_stream_packet_handler (server, &stream_packet_callback, NULL));

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_fd, 2000));

    const unsigned char header_one[] = "hdr";
    const unsigned char body_one[] = "body-1";
    TEST_ASSERT_EQUAL_INT (0,
                           send_stream_packet_frame (client_fd, header_one, sizeof (header_one) - 1,
                                                     body_one, sizeof (body_one) - 1));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.callbacks, 1, 5000));

    const unsigned char split_header[] = "split";
    const unsigned char split_body[] = "body-2";
    const std::vector<unsigned char> split_frame = build_stream_packet_frame (
      split_header, sizeof (split_header) - 1, split_body, sizeof (split_body) - 1);
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet (client_fd, &split_frame[0], 4));
    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet (client_fd, &split_frame[4], split_frame.size () - 4));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.callbacks, 2, 5000));

    const unsigned char multi_header_one[] = "m1";
    const unsigned char multi_body_one[] = "payload-a";
    const unsigned char multi_header_two[] = "m2";
    const unsigned char multi_body_two[] = "payload-b";
    const std::vector<unsigned char> multi_frame_one = build_stream_packet_frame (
      multi_header_one, sizeof (multi_header_one) - 1, multi_body_one, sizeof (multi_body_one) - 1);
    const std::vector<unsigned char> multi_frame_two = build_stream_packet_frame (
      multi_header_two, sizeof (multi_header_two) - 1, multi_body_two, sizeof (multi_body_two) - 1);
    std::vector<unsigned char> multi_fragment;
    multi_fragment.reserve (multi_frame_one.size () + multi_frame_two.size ());
    multi_fragment.insert (multi_fragment.end (), multi_frame_one.begin (), multi_frame_one.end ());
    multi_fragment.insert (multi_fragment.end (), multi_frame_two.begin (), multi_frame_two.end ());
    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet (client_fd, &multi_fragment[0], multi_fragment.size ()));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.callbacks, 4, 5000));

    const unsigned char zero_header_body[] = "body-only";
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet_frame (client_fd, NULL, 0, zero_header_body,
                                                        sizeof (zero_header_body) - 1));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.callbacks, 5, 5000));

    const unsigned char zero_body_header[] = "header-only";
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet_frame (client_fd, zero_body_header,
                                                        sizeof (zero_body_header) - 1, NULL, 0));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.callbacks, 6, 5000));

    TEST_ASSERT_EQUAL_INT (0, send_stream_packet_frame (client_fd, NULL, 0, NULL, 0));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.callbacks, 7, 5000));

    {
        std::lock_guard<std::mutex> lk (probe.mu);
        TEST_ASSERT_EQUAL_UINT64 (7, probe.records.size ());
        assert_stream_packet_record (probe.records[0], "hdr", "body-1");
        assert_stream_packet_record (probe.records[1], "split", "body-2");
        assert_stream_packet_record (probe.records[2], "m1", "payload-a");
        assert_stream_packet_record (probe.records[3], "m2", "payload-b");
        assert_stream_packet_record (probe.records[4], "", "body-only");
        assert_stream_packet_record (probe.records[5], "header-only", "");
        assert_stream_packet_record (probe.records[6], "", "");
    }

    close_raw_fd (client_fd);
    g_stream_packet_probe = NULL;
    test_context_socket_close_zero_linger (server);
}

void test_stream_packet_disconnect_rid_closes_client ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    stream_packet_probe_t probe;
    probe.socket = server;
    g_stream_packet_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_stream_packet_handler (server, &stream_packet_callback, NULL));

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_fd, 2000));

    const unsigned char header[] = "bye";
    const unsigned char body[] = "close-me";
    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet_frame (client_fd, header, sizeof (header) - 1, body, sizeof (body) - 1));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.callbacks, 1, 5000));

    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    {
        std::lock_guard<std::mutex> lk (probe.mu);
        TEST_ASSERT_EQUAL_UINT64 (1, probe.records.size ());
        TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, probe.records[0].source_rid.size ());
        rid.size = stream_routing_id_size;
        memcpy (rid.data, &probe.records[0].source_rid[0], stream_routing_id_size);
    }

    const unsigned char closing[] = "session-closing";
    TEST_ASSERT_EQUAL_INT (
      static_cast<int> (sizeof (closing) - 1),
      TEST_ASSERT_SUCCESS_ERRNO (
        test_stream_send_bytes (server, &rid, closing, sizeof (closing) - 1, 0)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect_rid (server, &rid));
    unsigned char observed_closing[sizeof (closing) - 1];
    TEST_ASSERT_EQUAL_INT (
      0, recv_exact (client_fd, observed_closing, sizeof (observed_closing)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (
      closing, observed_closing, static_cast<unsigned int> (sizeof (observed_closing)));
    TEST_ASSERT_TRUE (wait_raw_fd_closed (client_fd));

    close_raw_fd (client_fd);
    g_stream_packet_probe = NULL;
    test_context_socket_close_zero_linger (server);
}

void test_stream_packet_ordering_contracts ()
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_IO_THREADS, 2));

    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    stream_packet_probe_t probe;
    probe.socket = server;
    probe.block_first_callback = true;
    g_stream_packet_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_stream_packet_handler (server, &stream_packet_callback, NULL));

    const int client_one = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_one >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_one, 2000));

    const int client_two = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_two >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_two, 2000));

    msleep (SETTLE_TIME);

    const unsigned char first_header[] = "first";
    const unsigned char first_body[] = "client-one";
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet_frame (client_one, first_header,
                                                        sizeof (first_header) - 1, first_body,
                                                        sizeof (first_body) - 1));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.blocked_callback_started, 1, 5000));

    const unsigned char other_header[] = "other";
    const unsigned char other_body[] = "client-two";
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet_frame (client_two, other_header,
                                                        sizeof (other_header) - 1, other_body,
                                                        sizeof (other_body) - 1));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.other_source_rid_seen, 1, 5000));

    const unsigned char second_header[] = "second";
    const unsigned char second_body[] = "client-one-again";
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet_frame (client_one, second_header,
                                                        sizeof (second_header) - 1, second_body,
                                                        sizeof (second_body) - 1));
    test_sleep_ms (200);
    TEST_ASSERT_EQUAL_INT (0, probe.same_source_rid_seen.load ());
    TEST_ASSERT_EQUAL_INT (2, probe.callbacks.load ());

    probe.release_blocked_callback.store (true, std::memory_order_release);
    probe.cv.notify_all ();
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.callbacks, 3, 5000));
    TEST_ASSERT_EQUAL_INT (1, probe.same_source_rid_seen.load ());

    close_raw_fd (client_two);
    close_raw_fd (client_one);
    g_stream_packet_probe = NULL;
    test_context_socket_close_zero_linger (server);
}

void test_stream_packet_malformed_close_contracts ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    const int64_t maxmsgsize = 4;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_MAXMSGSIZE, &maxmsgsize, sizeof (maxmsgsize)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
    void *monitor = zlink_socket_monitor_open (server, &monitor_opts);
    TEST_ASSERT_NOT_NULL (monitor);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (monitor, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    stream_packet_probe_t probe;
    probe.socket = server;
    g_stream_packet_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_stream_packet_handler (server, &stream_packet_callback, NULL));

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_fd, 2000));

    const unsigned char valid_header[] = "ok";
    const unsigned char valid_body[] = "go";
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet_frame (client_fd, valid_header,
                                                        sizeof (valid_header) - 1, valid_body,
                                                        sizeof (valid_body) - 1));
    TEST_ASSERT_TRUE (wait_counter_at_least (&probe.callbacks, 1, 5000));
    unsigned char expected_rid[stream_routing_id_size];
    {
        std::lock_guard<std::mutex> lk (probe.mu);
        TEST_ASSERT_TRUE (probe.records.size () >= 1);
        TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, probe.records[0].source_rid.size ());
        memcpy (expected_rid, &probe.records[0].source_rid[0], stream_routing_id_size);
    }

    const unsigned char oversize_header[] = "oversize";
    const std::vector<unsigned char> oversize_frame =
      build_stream_packet_frame (oversize_header, sizeof (oversize_header) - 1, NULL, 0);
    TEST_ASSERT_EQUAL_INT (
      0, send_stream_packet (client_fd, &oversize_frame[0], oversize_frame.size ()));

    unsigned char disconnect_rid[stream_routing_id_size];
    TEST_ASSERT_TRUE (
      wait_monitor_event_direct (monitor, ZLINK_EVENT_DISCONNECTED, disconnect_rid, 10000));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (expected_rid, disconnect_rid, stream_routing_id_size);
    TEST_ASSERT_EQUAL_INT (1, probe.callbacks.load (std::memory_order_acquire));

    close_raw_fd (client_fd);
    g_stream_packet_probe = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (server);
}

void test_stream_connect_rejected ()
{
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (stream, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    errno = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_NOT_SUPPORTED,
                           zlink_connect (stream, "tcp://127.0.0.1:5555"));
    TEST_ASSERT_EQUAL_INT (EOPNOTSUPP, errno);

    test_context_socket_close_zero_linger (stream);
}

int main (void)
{
    UNITY_BEGIN ();

    setup_test_environment ();

    if (should_run_stream_socket_test ("test_stream_callback_lifecycle"))
        RUN_TEST (test_stream_callback_lifecycle);
    if (should_run_stream_socket_test ("test_stream_recv_api_dispatch_conflict"))
        RUN_TEST (test_stream_recv_api_dispatch_conflict);
    if (should_run_stream_socket_test ("test_stream_notify_option_remains_available_with_dispatch"))
        RUN_TEST (test_stream_notify_option_remains_available_with_dispatch);
    if (should_run_stream_socket_test ("test_stream_callback_echo_raw"))
        RUN_TEST (test_stream_callback_echo_raw);
    if (should_run_stream_socket_test ("test_stream_callback_echo_single_zero_byte"))
        RUN_TEST (test_stream_callback_echo_single_zero_byte);
    if (should_run_stream_socket_test ("test_stream_recv_ready_precedes_first_payload_contract"))
        RUN_TEST (test_stream_recv_ready_precedes_first_payload_contract);
    if (should_run_stream_socket_test (
          "test_stream_raw_callback_ready_precedes_first_payload_contract"))
        RUN_TEST (test_stream_raw_callback_ready_precedes_first_payload_contract);
    if (should_run_stream_socket_test (
          "test_stream_recv_handler_delivers_raw_chunks_not_len32be_frames"))
        RUN_TEST (test_stream_recv_handler_delivers_raw_chunks_not_len32be_frames);
    if (should_run_stream_socket_test ("test_stream_packet_handler_mode_gates"))
        RUN_TEST (test_stream_packet_handler_mode_gates);
    if (should_run_stream_socket_test ("test_stream_packet_framing_contracts"))
        RUN_TEST (test_stream_packet_framing_contracts);
    if (should_run_stream_socket_test ("test_stream_packet_disconnect_rid_closes_client"))
        RUN_TEST (test_stream_packet_disconnect_rid_closes_client);
    if (should_run_stream_socket_test ("test_stream_packet_ordering_contracts"))
        RUN_TEST (test_stream_packet_ordering_contracts);
    if (should_run_stream_socket_test ("test_stream_packet_malformed_close_contracts"))
        RUN_TEST (test_stream_packet_malformed_close_contracts);
#if !defined(ZLINK_HAVE_WINDOWS)
    if (should_run_stream_socket_test (
          "test_stream_raw_multiclient_strict_ready_gating_regression"))
        RUN_TEST (test_stream_raw_multiclient_strict_ready_gating_regression);
    if (should_run_stream_socket_test (
          "test_stream_recv_multiclient_strict_ready_gating_regression"))
        RUN_TEST (test_stream_recv_multiclient_strict_ready_gating_regression);
    if (should_run_stream_socket_test ("test_stream_raw_multiclient_load_integrity"))
        RUN_TEST (test_stream_raw_multiclient_load_integrity);
    if (should_run_stream_socket_test (
          "test_stream_raw_multiclient_load_integrity_with_send_ready_handler"))
        RUN_TEST (test_stream_raw_multiclient_load_integrity_with_send_ready_handler);
#endif
    if (should_run_stream_socket_test ("test_stream_connect_rejected"))
        RUN_TEST (test_stream_connect_rejected);

    return UNITY_END ();
}
