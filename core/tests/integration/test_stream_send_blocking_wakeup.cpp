/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
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

namespace
{
const int kSendTimeoutMs = 1000;
const int kWakeupSendTimeoutMs = 5000;
const size_t kPayloadSize = 4096;
const uint64_t kStreamHwm = 10u * (kPayloadSize + sizeof (zlink_msg_t));
const int kRouteIdSize = 4;
const int kSocketBufBytes = 4096;
const int kProbeTimeoutMs = 250;
const int kLingerMs = 0;
const int kFillDeadlineMs = 5000;

struct stream_route_probe_t
{
    stream_route_probe_t () : ready (0), rid () { memset (&rid, 0, sizeof (rid)); }

    std::atomic<int> ready;
    zlink_routing_id_t rid;
};

stream_route_probe_t *g_stream_route_probe = NULL;

struct stream_routed_ready_event_t
{
    zlink_routing_id_t rid;
    uint64_t pair_id;
    uint64_t pair_generation;
    zlink_send_complete_result_t result;
};

struct stream_routed_ready_probe_t
{
    std::mutex sync;
    std::condition_variable changed;
    std::vector<stream_routed_ready_event_t> events;
};

void capture_stream_routed_ready (void *,
                                  const zlink_send_complete_event_t *event_,
                                  void *userdata_)
{
    stream_routed_ready_probe_t *probe =
      static_cast<stream_routed_ready_probe_t *> (userdata_);
    if (!probe || !event_)
        return;

    stream_routed_ready_event_t copy;
    memset (&copy, 0, sizeof (copy));
    copy.rid = event_->peer_rid;
    copy.pair_id = event_->transport_pair_id;
    copy.pair_generation = event_->transport_pair_generation;
    copy.result = event_->result;
    {
        std::lock_guard<std::mutex> lock (probe->sync);
        probe->events.push_back (copy);
    }
    probe->changed.notify_all ();
}

bool wait_stream_routed_ready (stream_routed_ready_probe_t *probe_,
                               const zlink_routing_id_t *rid_,
                               zlink_send_complete_result_t result_,
                               int timeout_ms_ = 3000)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    std::unique_lock<std::mutex> lock (probe_->sync);
    while (true) {
        for (size_t i = 0; i < probe_->events.size (); ++i) {
            const stream_routed_ready_event_t &event = probe_->events[i];
            if (event.result == result_ && event.rid.size == rid_->size
                && memcmp (event.rid.data, rid_->data, rid_->size) == 0)
                return true;
        }
        if (probe_->changed.wait_until (lock, deadline)
            == std::cv_status::timeout)
            return false;
    }
}

void configure_stream_socket (void *socket_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &kLingerMs, sizeof (kLingerMs)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &kStreamHwm, sizeof (kStreamHwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &kStreamHwm, sizeof (kStreamHwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDBUF, &kSocketBufBytes, sizeof (kSocketBufBytes)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVBUF, &kSocketBufBytes, sizeof (kSocketBufBytes)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &kSendTimeoutMs, sizeof (kSendTimeoutMs)));
}

#if defined(ZLINK_HAVE_WINDOWS)
int connect_raw_tcp (const char *endpoint_)
{
    LIBZLINK_UNUSED (endpoint_);
    errno = EOPNOTSUPP;
    return -1;
}

int send_all (int fd_, const unsigned char *buf_, size_t size_)
{
    LIBZLINK_UNUSED (fd_);
    LIBZLINK_UNUSED (buf_);
    LIBZLINK_UNUSED (size_);
    errno = EOPNOTSUPP;
    return -1;
}

int recv_raw (int fd_, unsigned char *buf_, size_t cap_)
{
    LIBZLINK_UNUSED (fd_);
    LIBZLINK_UNUSED (buf_);
    LIBZLINK_UNUSED (cap_);
    errno = EOPNOTSUPP;
    return -1;
}

void close_raw_fd (int fd_)
{
    LIBZLINK_UNUSED (fd_);
}

void set_raw_timeout (int fd_, int timeout_ms_)
{
    LIBZLINK_UNUSED (fd_);
    LIBZLINK_UNUSED (timeout_ms_);
}

#else
bool parse_tcp_endpoint (const char *endpoint_, char host_[64], int *port_)
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

int connect_raw_tcp (const char *endpoint_)
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
        const int err = errno;
        close (fd);
        errno = err;
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

int send_all (int fd_, const unsigned char *buf_, size_t size_)
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

int recv_raw (int fd_, unsigned char *buf_, size_t cap_)
{
    const ssize_t n = recv (fd_, buf_, cap_, 0);
    if (n < 0 && errno == EINTR)
        return recv_raw (fd_, buf_, cap_);
    if (n <= 0)
        return -1;
    return static_cast<int> (n);
}

bool recv_raw_exact (int fd_, unsigned char *buf_, size_t size_)
{
    size_t received = 0;
    while (received < size_) {
        const int rc = recv_raw (fd_, buf_ + received, size_ - received);
        if (rc <= 0)
            return false;
        received += static_cast<size_t> (rc);
    }
    return true;
}

void close_raw_fd (int fd_)
{
    if (fd_ >= 0)
        close (fd_);
}

void set_raw_timeout (int fd_, int timeout_ms_)
{
    const int rcvbuf_rc =
      setsockopt (fd_, SOL_SOCKET, SO_RCVBUF, &kSocketBufBytes, sizeof (kSocketBufBytes));
    TEST_ASSERT_EQUAL_INT (0, rcvbuf_rc);

    struct timeval tv;
    tv.tv_sec = timeout_ms_ / 1000;
    tv.tv_usec = (timeout_ms_ % 1000) * 1000;
    const int rcv_rc = setsockopt (fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
    TEST_ASSERT_EQUAL_INT (0, rcv_rc);
    const int snd_rc = setsockopt (fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof (tv));
    TEST_ASSERT_EQUAL_INT (0, snd_rc);
}
#endif

bool wait_stream_route_ready (stream_route_probe_t *probe_, int timeout_ms_)
{
    if (!probe_)
        return false;

    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        if (probe_->ready.load (std::memory_order_acquire) != 0)
            return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return probe_->ready.load (std::memory_order_acquire) != 0;
}

int capture_stream_route_callback (const zlink_routing_id_t *rid_, zlink_msg_t *msg_, void *)
{
    if (msg_) {
        if (g_stream_route_probe && rid_ && zlink_msg_size (msg_) > 0 && rid_->size == kRouteIdSize
            && g_stream_route_probe->ready.load (std::memory_order_acquire) == 0) {
            g_stream_route_probe->rid.size = rid_->size;
            memcpy (g_stream_route_probe->rid.data, rid_->data, kRouteIdSize);
            g_stream_route_probe->ready.store (1, std::memory_order_release);
        }
        (void) zlink_msg_close (msg_);
    }
    return 0;
}

int capture_stream_route_raw_callback (const zlink_routing_id_t *rid_, zlink_msg_t *msg_)
{
    return capture_stream_route_callback (rid_, msg_, NULL);
}

//  Reserve one record against an exact STREAM target.
zlink_submit_result_t park_stream_record (
  void *server_, const zlink_routed_submit_target_t *target_,
  const char *text_, zlink_send_op_id_t *op_id_out_)
{
    zlink_msg_t part;
    const size_t size = strlen (text_);
    if (zlink_msg_init_size (&part, size) != 0)
        return ZLINK_SUBMIT_INTERNAL_ERROR;
    memcpy (zlink_msg_data (&part), text_, size);

    zlink_send_async_options_t options;
    memset (&options, 0, sizeof (options));
    options.struct_size = sizeof (options);
    options.target = target_;

    const zlink_submit_result_t rc =
      zlink_send_async (server_, &part, 1, &options, op_id_out_);
    if (rc != ZLINK_SUBMIT_OK)
        zlink_msg_close (&part);
    return rc;
}

bool wait_stream_poller_no_event (void *poller_,
                                  zlink_poller_event_t *event_,
                                  zlink_config_result_t *error_out_,
                                  long timeout_ms_)
{
    if (!poller_ || !event_ || !error_out_)
        return false;

    const int rc = zlink_poller_wait (poller_, event_, 1, timeout_ms_, error_out_);
    if (rc == 0)
        return true;
    return rc == -1 && errno == EAGAIN;
}

void establish_stream_route (void *server_, int raw_fd_, zlink_routing_id_t *rid_)
{
    stream_route_probe_t probe;
    g_stream_route_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_stream_attach_raw (server_, &capture_stream_route_raw_callback));

    const unsigned char request = 0xA5;
    TEST_ASSERT_EQUAL_INT (0, send_all (raw_fd_, &request, sizeof (request)));
    TEST_ASSERT_TRUE (wait_stream_route_ready (&probe, 5000));

    *rid_ = probe.rid;
    g_stream_route_probe = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_stream_detach (server_));
}

void fill_stream_send_queue_until_hwm (void *server_, const zlink_routing_id_t *rid_)
{
    std::vector<unsigned char> payload (kPayloadSize, 0x5A);
    int sent = 0;
    const auto stable_full_window = std::chrono::milliseconds (120);
    auto no_success_since = std::chrono::steady_clock::now ();
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (kFillDeadlineMs);
    bool reached_full = false;
    for (;;) {
        const int rc =
          test_stream_send_bytes (server_, rid_, &payload[0], kPayloadSize, ZLINK_DONTWAIT);
        if (rc == static_cast<int> (kPayloadSize)) {
            ++sent;
            no_success_since = std::chrono::steady_clock::now ();
            if (std::chrono::steady_clock::now () >= deadline)
                break;
            continue;
        }

        TEST_ASSERT_EQUAL_INT (-1, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

        const auto now = std::chrono::steady_clock::now ();
        if (now - no_success_since >= stable_full_window) {
            reached_full = true;
            break;
        }
        if (now >= deadline)
            break;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    TEST_ASSERT_GREATER_THAN_INT (0, sent);
    TEST_ASSERT_TRUE (reached_full);
}

void fill_stream_send_queue_until_backpressured_once (void *server_, const zlink_routing_id_t *rid_)
{
    std::vector<unsigned char> payload (kPayloadSize, 0x5A);
    int sent = 0;
    while (true) {
        const int rc =
          test_stream_send_bytes (server_, rid_, &payload[0], kPayloadSize, ZLINK_DONTWAIT);
        if (rc == static_cast<int> (kPayloadSize)) {
            ++sent;
            continue;
        }

        TEST_ASSERT_EQUAL_INT (-1, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
        break;
    }

    TEST_ASSERT_GREATER_THAN_INT (0, sent);
}

bool drain_stream_until_send_reopens (void *server_,
                                      int raw_fd_,
                                      const zlink_routing_id_t *rid_,
                                      const unsigned char *payload_,
                                      size_t payload_size_)
{
    if (!server_ || raw_fd_ < 0 || !rid_ || !payload_ || payload_size_ == 0)
        return false;

    unsigned char drain_buf[64 * 1024];
    int drained = 0;
    const int drain_target = static_cast<int> (payload_size_ * ((kStreamHwm + 1) / 2 + 2));
    const auto drain_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (1000);
    while (std::chrono::steady_clock::now () < drain_deadline && drained < drain_target) {
        const int n = recv_raw (raw_fd_, drain_buf, sizeof (drain_buf));
        if (n > 0)
            drained += n;
    }

    const auto reopen_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (1500);
    while (std::chrono::steady_clock::now () < reopen_deadline) {
        const int send_rc =
          test_stream_send_bytes (server_, rid_, payload_, payload_size_, ZLINK_DONTWAIT);
        if (send_rc == static_cast<int> (payload_size_))
            return true;

        TEST_ASSERT_EQUAL_INT (-1, send_rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
        (void) recv_raw (raw_fd_, drain_buf, sizeof (drain_buf));
    }

    return false;
}

zlink_routed_submit_target_t select_stream_target_eventually (
  void *server_, const zlink_routing_id_t *rid_)
{
    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    for (int attempt = 0; attempt < 3000; ++attempt) {
        const zlink_submit_result_t result =
          zlink_select_routed_submit_target (server_, rid_, &target);
        if (result == ZLINK_SUBMIT_OK)
            return target;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_NOT_CONNECTED, result);
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    TEST_FAIL_MESSAGE ("STREAM route did not become an exact submit target");
    return target;
}
} // namespace

void test_stream_routed_admission_is_exact_and_initial_wake_is_lossless ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    stream_routed_ready_probe_t before_attach_probe;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (server, &capture_stream_routed_ready,
                                   &before_attach_probe));

    char endpoint[MAX_SOCKET_STRING];
    memset (endpoint, 0, sizeof (endpoint));
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 1000);

    zlink_routing_id_t rid;
    establish_stream_route (server, raw_fd, &rid);

    const zlink_routed_submit_target_t target =
      select_stream_target_eventually (server, &rid);
    //  A handler registered before the transport attached does not prevent
    //  immediate admission of the first record submitted to that exact
    //  target. Immediate admission uses op id zero and no callback.
    zlink_send_op_id_t admitted_op = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      park_stream_record (server, &target, "attach", &admitted_op));
    TEST_ASSERT_EQUAL_UINT64 (0, admitted_op);
    {
        unsigned char attach_received[6];
        TEST_ASSERT_TRUE (
          recv_raw_exact (raw_fd, attach_received, sizeof (attach_received)));
    }
    TEST_ASSERT_EQUAL_UINT64 (4, target.peer_rid.size);
    TEST_ASSERT_TRUE (target.transport_pair_id != 0);
    TEST_ASSERT_TRUE (target.transport_pair_generation != 0);

    zlink_msg_t exact;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&exact, 5));
    memcpy (zlink_msg_data (&exact), "exact", 5);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_transport_pair (
        server, &target.peer_rid, target.transport_pair_id,
        target.transport_pair_generation, &exact, ZLINK_DONTWAIT,
        ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&exact));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&exact));

    unsigned char received[5];
    TEST_ASSERT_TRUE (recv_raw_exact (raw_fd, received, sizeof (received)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY ("exact", received, sizeof (received));

    zlink_routed_submit_target_t stale = target;
    ++stale.transport_pair_generation;
    zlink_msg_t stale_message;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&stale_message, 5));
    memcpy (zlink_msg_data (&stale_message), "stale", 5);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_NOT_CONNECTED,
      zlink_send_part_transport_pair (
        server, &stale.peer_rid, stale.transport_pair_id,
        stale.transport_pair_generation, &stale_message, ZLINK_DONTWAIT,
        ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (EHOSTUNREACH, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&stale_message));

    //  Back the queue up so the next record stays reserved, then end the
    //  route: the reserved record is what reports the terminal failure.
    fill_stream_send_queue_until_backpressured_once (server, &rid);
    zlink_send_op_id_t parked_op = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      park_stream_record (server, &target, "parked", &parked_op));
    TEST_ASSERT_NOT_EQUAL (0, parked_op);

    close_raw_fd (raw_fd);
    TEST_ASSERT_TRUE_MESSAGE (
      wait_stream_routed_ready (&before_attach_probe, &rid,
                                ZLINK_SEND_TERMINAL),
      "STREAM detach did not fail the reserved record for the exact target");
    test_context_socket_close (server);

    void *late_server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (late_server);
    configure_stream_socket (late_server);
    memset (endpoint, 0, sizeof (endpoint));
    bind_loopback_ipv4 (late_server, endpoint, sizeof (endpoint));
    const int late_raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, late_raw_fd);
    set_raw_timeout (late_raw_fd, 1000);
    zlink_routing_id_t late_rid;
    establish_stream_route (late_server, late_raw_fd, &late_rid);

    stream_routed_ready_probe_t after_attach_probe;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_complete_handler (late_server, &capture_stream_routed_ready,
                                   &after_attach_probe));
    const zlink_routed_submit_target_t late_target =
      select_stream_target_eventually (late_server, &late_rid);
    zlink_send_op_id_t late_op = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      park_stream_record (late_server, &late_target, "late", &late_op));
    TEST_ASSERT_EQUAL_UINT64 (0, late_op);

    unsigned char late_received[4];
    TEST_ASSERT_TRUE (
      recv_raw_exact (late_raw_fd, late_received, sizeof (late_received)));

    close_raw_fd (late_raw_fd);
    test_context_socket_close (late_server);
#endif
}

void test_stream_queue_reopens_after_peer_reads ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_SNDTIMEO, &kWakeupSendTimeoutMs,
                                                 sizeof (kWakeupSendTimeoutMs)));

    char endpoint[MAX_SOCKET_STRING];
    memset (endpoint, 0, sizeof (endpoint));
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 200);

    zlink_routing_id_t rid;
    establish_stream_route (server, raw_fd, &rid);
    fill_stream_send_queue_until_hwm (server, &rid);

    std::vector<unsigned char> payload (kPayloadSize, 0x33);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_SNDTIMEO, &kProbeTimeoutMs, sizeof (kProbeTimeoutMs)));
    TEST_ASSERT_EQUAL_INT (-1, test_stream_send_bytes (server, &rid, &payload[0], kPayloadSize, 0));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    unsigned char drain_buf[64 * 1024];
    int drained = 0;
    const int drain_target = static_cast<int> (kPayloadSize * ((kStreamHwm + 1) / 2 + 2));
    const auto drain_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (1000);
    while (std::chrono::steady_clock::now () < drain_deadline && drained < drain_target) {
        const int n = recv_raw (raw_fd, drain_buf, sizeof (drain_buf));
        if (n > 0)
            drained += n;
    }

    bool reopened = false;
    const auto reopen_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (1500);
    while (std::chrono::steady_clock::now () < reopen_deadline) {
        const int send_rc =
          test_stream_send_bytes (server, &rid, &payload[0], kPayloadSize, ZLINK_DONTWAIT);
        if (send_rc == static_cast<int> (kPayloadSize)) {
            reopened = true;
            break;
        }
        TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
        (void) recv_raw (raw_fd, drain_buf, sizeof (drain_buf));
    }
    TEST_ASSERT_TRUE (reopened);

    close_raw_fd (raw_fd);
    test_context_socket_close (server);
#endif
}

void test_stream_pollout_tracks_send_recovery_axis ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    char endpoint[MAX_SOCKET_STRING];
    memset (endpoint, 0, sizeof (endpoint));
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 200);

    zlink_routing_id_t rid;
    establish_stream_route (server, raw_fd, &rid);

    fill_stream_send_queue_until_backpressured_once (server, &rid);

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, server, server, ZLINK_POLLOUT));

    unsigned char drain_buf[64 * 1024];
    const auto drain_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (1000);
    while (std::chrono::steady_clock::now () < drain_deadline) {
        const int n = recv_raw (raw_fd, drain_buf, sizeof (drain_buf));
        if (n <= 0)
            break;
    }

    bool pollout_ready = false;
    const auto poll_deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
    while (std::chrono::steady_clock::now () < poll_deadline) {
        zlink_poller_event_t event;
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        const int rc = zlink_poller_wait (poller, &event, 1, 50, &error);
        if (rc <= 0)
            continue;
        if (event.socket == server && (event.events & ZLINK_POLLOUT) != 0) {
            pollout_ready = true;
            break;
        }
    }
    TEST_ASSERT_TRUE (pollout_ready);

    uint32_t ready_events = 0;
    size_t ready_events_size = sizeof (ready_events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (server, ZLINK_OPT_EVENTS, &ready_events, &ready_events_size));
    TEST_ASSERT_TRUE ((ready_events & ZLINK_POLLOUT) != 0);

    std::vector<unsigned char> payload (kPayloadSize, 0x77);
    const int retry_rc =
      test_stream_send_bytes (server, &rid, &payload[0], kPayloadSize, ZLINK_DONTWAIT);
    if (retry_rc != static_cast<int> (kPayloadSize)) {
        TEST_ASSERT_EQUAL_INT (-1, retry_rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_destroy (&poller));
    close_raw_fd (raw_fd);
    test_context_socket_close (server);
#endif
}

void test_stream_pollout_only_observes_recovery_readiness ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    char endpoint[MAX_SOCKET_STRING];
    memset (endpoint, 0, sizeof (endpoint));
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 200);

    zlink_routing_id_t rid;
    establish_stream_route (server, raw_fd, &rid);

    void *poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, server, server, ZLINK_POLLOUT));

    zlink_poller_event_t event;
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    TEST_ASSERT_TRUE (wait_stream_poller_no_event (poller, &event, &error, 0));

    fill_stream_send_queue_until_hwm (server, &rid);

    std::vector<unsigned char> payload (kPayloadSize, 0x21);
    TEST_ASSERT_EQUAL_INT (
      -1, test_stream_send_bytes (server, &rid, &payload[0], kPayloadSize, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    TEST_ASSERT_TRUE (wait_stream_poller_no_event (poller, &event, &error, 0));

    unsigned char drain_buf[64 * 1024];
    const auto reopen_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (1500);
    while (std::chrono::steady_clock::now () < reopen_deadline) {
        const int n = recv_raw (raw_fd, drain_buf, sizeof (drain_buf));
        if (n > 0)
            continue;
        if (n < 0 && errno != EAGAIN)
            break;
    }

    bool pollout_ready = false;
    const auto poll_deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
    while (std::chrono::steady_clock::now () < poll_deadline) {
        const int rc = zlink_poller_wait (poller, &event, 1, 50, &error);
        if (rc <= 0)
            continue;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        if (event.socket == server && (event.events & ZLINK_POLLOUT) != 0) {
            pollout_ready = true;
            break;
        }
    }
    TEST_ASSERT_TRUE (pollout_ready);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_destroy (&poller));
    poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_add (poller, server, server, ZLINK_POLLOUT));
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, &event, 1, 0, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_PTR (server, event.socket);
    TEST_ASSERT_TRUE ((event.events & ZLINK_POLLOUT) != 0);

    std::vector<unsigned char> retry_payload (kPayloadSize, 0x29);
    const int retry_rc =
      test_stream_send_bytes (server, &rid, &retry_payload[0], kPayloadSize, ZLINK_DONTWAIT);
    if (retry_rc != static_cast<int> (kPayloadSize)) {
        TEST_ASSERT_EQUAL_INT (-1, retry_rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_poller_destroy (&poller));
    close_raw_fd (raw_fd);
    test_context_socket_close (server);
#endif
}

void test_stream_blocking_send_times_out_without_peer_reads ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    char endpoint[MAX_SOCKET_STRING];
    memset (endpoint, 0, sizeof (endpoint));
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 200);

    zlink_routing_id_t rid;
    establish_stream_route (server, raw_fd, &rid);
    fill_stream_send_queue_until_hwm (server, &rid);

    std::vector<unsigned char> payload (kPayloadSize, 0x44);
    void *stopwatch = zlink_stopwatch_start ();
    const int send_rc = test_stream_send_bytes (server, &rid, &payload[0], kPayloadSize, 0);
    const unsigned int elapsed_ms = zlink_stopwatch_stop (stopwatch) / 1000;

    TEST_ASSERT_EQUAL_INT (-1, send_rc);
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_TRUE (elapsed_ms >= static_cast<unsigned int> (kSendTimeoutMs - 150));

    close_raw_fd (raw_fd);
    test_context_socket_close (server);
#endif
}

void test_stream_nonblocking_send_preserves_message_for_retry ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    char endpoint[MAX_SOCKET_STRING];
    memset (endpoint, 0, sizeof (endpoint));
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 200);

    zlink_routing_id_t rid;
    establish_stream_route (server, raw_fd, &rid);
    fill_stream_send_queue_until_hwm (server, &rid);

    zlink_msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&msg, kPayloadSize));
    memset (zlink_msg_data (&msg), 0x6C, kPayloadSize);

    const zlink_submit_result_t send_rc = zlink_send_rid (server, &rid, &msg, 1, ZLINK_DONTWAIT);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, send_rc);
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_EQUAL_UINT64 (kPayloadSize, zlink_msg_size (&msg));

    const unsigned char *msg_data = static_cast<const unsigned char *> (zlink_msg_data (&msg));
    TEST_ASSERT_NOT_NULL (msg_data);
    for (size_t i = 0; i < kPayloadSize; ++i)
        TEST_ASSERT_EQUAL_UINT8 (0x6C, msg_data[i]);

    std::vector<unsigned char> probe_payload (kPayloadSize, 0x41);
    TEST_ASSERT_TRUE (drain_stream_until_send_reopens (server, raw_fd, &rid, &probe_payload[0],
                                                       probe_payload.size ()));

    TEST_ASSERT_EQUAL_INT (static_cast<int> (kPayloadSize),
                           test_stream_send_single_msg (server, &rid, &msg, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&msg));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));
    close_raw_fd (raw_fd);
    test_context_socket_close (server);
#endif
}

void test_stream_part_nonblocking_backpressure_preserves_message ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    char endpoint[MAX_SOCKET_STRING];
    memset (endpoint, 0, sizeof (endpoint));
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 200);

    zlink_routing_id_t rid;
    establish_stream_route (server, raw_fd, &rid);
    fill_stream_send_queue_until_hwm (server, &rid);

    zlink_msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&msg, kPayloadSize));
    memset (zlink_msg_data (&msg), 0x73, kPayloadSize);

    const zlink_submit_result_t send_rc =
      zlink_send_part_rid (server, &rid, &msg, ZLINK_DONTWAIT, ZLINK_PART_FINAL);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, send_rc);
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_EQUAL_UINT64 (kPayloadSize, zlink_msg_size (&msg));

    const unsigned char *msg_data = static_cast<const unsigned char *> (zlink_msg_data (&msg));
    TEST_ASSERT_NOT_NULL (msg_data);
    for (size_t i = 0; i < kPayloadSize; ++i)
        TEST_ASSERT_EQUAL_UINT8 (0x73, msg_data[i]);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));
    close_raw_fd (raw_fd);
    test_context_socket_close (server);
#endif
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_stream_routed_admission_is_exact_and_initial_wake_is_lossless);
    RUN_TEST (test_stream_queue_reopens_after_peer_reads);
    RUN_TEST (test_stream_pollout_tracks_send_recovery_axis);
    RUN_TEST (test_stream_pollout_only_observes_recovery_readiness);
    RUN_TEST (test_stream_blocking_send_times_out_without_peer_reads);
    RUN_TEST (test_stream_nonblocking_send_preserves_message_for_retry);
    RUN_TEST (test_stream_part_nonblocking_backpressure_preserves_message);
    return UNITY_END ();
}
