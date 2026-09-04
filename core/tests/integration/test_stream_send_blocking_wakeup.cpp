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
const size_t payload_size = 4096;
const uint64_t stream_hwm =
  10u * (payload_size + static_cast<uint64_t> (sizeof (zlink_msg_t)));
const int socket_buffer_bytes = 4096;
const int send_timeout_ms = 5000;
const int linger_ms = 0;

#if !defined(ZLINK_HAVE_WINDOWS)
bool parse_tcp_endpoint (const char *endpoint_, char host_[64], int *port_)
{
    char proto[8] = {0};
    return endpoint_ && host_ && port_
           && sscanf (endpoint_, "%7[^:]://%63[^:]:%d", proto, host_, port_)
                == 3
           && strcmp (proto, "tcp") == 0 && *port_ > 0 && *port_ <= 65535;
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

    TEST_ASSERT_EQUAL_INT (
      0, setsockopt (fd, SOL_SOCKET, SO_RCVBUF, &socket_buffer_bytes,
                     sizeof (socket_buffer_bytes)));

    struct sockaddr_in address;
    memset (&address, 0, sizeof (address));
    address.sin_family = AF_INET;
    address.sin_port = htons (static_cast<uint16_t> (port));
    if (inet_pton (AF_INET, host, &address.sin_addr) != 1
        || connect (fd, reinterpret_cast<const struct sockaddr *> (&address),
                    sizeof (address)) != 0) {
        const int err = errno;
        close (fd);
        errno = err;
        return -1;
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    TEST_ASSERT_EQUAL_INT (
      0, setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof (timeout)));
    return fd;
}

int recv_raw (int fd_, unsigned char *buffer_, size_t capacity_)
{
    for (;;) {
        const ssize_t received = recv (fd_, buffer_, capacity_, 0);
        if (received > 0)
            return static_cast<int> (received);
        if (received < 0 && errno == EINTR)
            continue;
        return -1;
    }
}
#endif

void configure_stream (void *stream_)
{
    const zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_RAW;
    const int notify = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (stream_, ZLINK_OPT_LINGER, &linger_ms,
                        sizeof (linger_ms)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (stream_, ZLINK_OPT_SNDHWM, &stream_hwm,
                        sizeof (stream_hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (stream_, ZLINK_OPT_SNDBUF, &socket_buffer_bytes,
                        sizeof (socket_buffer_bytes)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (stream_, ZLINK_OPT_SNDTIMEO, &send_timeout_ms,
                        sizeof (send_timeout_ms)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (stream_, ZLINK_OPT_RCVTIMEO, &send_timeout_ms,
                        sizeof (send_timeout_ms)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (stream_, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (stream_, ZLINK_STREAM_OPT_NOTIFY, &notify,
                               sizeof (notify)));
}

zlink_routing_id_t receive_connected_rid (void *stream_)
{
    zlink_msg_t notification;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&notification));
    const zlink_routing_id_t *borrowed_rid = NULL;
    zlink_part_flag_t part_flag = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv_part (stream_, &borrowed_rid, &notification, &part_flag,
                       ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (borrowed_rid);
    TEST_ASSERT_EQUAL_UINT (4, borrowed_rid->size);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&notification));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, part_flag);

    zlink_routing_id_t rid = *borrowed_rid;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&notification));
    return rid;
}

void assert_no_completion (void *stream_)
{
    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (stream_, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    zlink_completion_close (&completion);
}

void receive_writable_completion (void *stream_,
                                  zlink_completion_id_t expected_id_,
                                  void *expected_context_,
                                  const zlink_routing_id_t *expected_rid_)
{
    zlink_pollitem_t writable = {stream_, 0, ZLINK_POLLOUT, 0};
    zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&writable, 1, 5000, &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLOUT, writable.revents);

    writable.revents = 0;
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&writable, 1, 0, &poll_error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLOUT, writable.revents);

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (stream_, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (expected_id_, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (expected_context_, completion.user_context);
    TEST_ASSERT_EQUAL_UINT (expected_rid_->size, completion.peer_rid.size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY (expected_rid_->data,
                                   completion.peer_rid.data,
                                   expected_rid_->size);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
    TEST_ASSERT_EQUAL_INT (0, completion.send_terminal_errno);
    zlink_completion_close (&completion);
}

size_t fill_until_backpressured (
  void *stream_, const zlink_routing_id_t *rid_,
  const std::vector<unsigned char> &payload_, void *user_context_,
  zlink_completion_id_t *wait_token_out_)
{
    TEST_ASSERT_NOT_NULL (wait_token_out_);
    *wait_token_out_ = 0;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (5);
    size_t admitted = 0;

    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_msg_init_size (&part, payload_.size ()));
        memcpy (zlink_msg_data (&part), payload_.data (), payload_.size ());
        zlink_completion_id_t id = UINT64_MAX;
        const zlink_submit_result_t result = zlink_send_part_rid (
          stream_, rid_, &part, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
          user_context_, &id);
        const int submit_errno = zlink_errno ();
        TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, submit_errno);
            TEST_ASSERT_NOT_EQUAL (0, id);

            // A raw TCP peer can free kernel credit without an application
            // recv. Consume such an already-ready token and keep filling until
            // POLLOUT stays quiet for a bounded interval; that is the stable
            // pressure point needed by the blocking-wakeup half of this test.
            zlink_pollitem_t writable = {stream_, 0, ZLINK_POLLOUT, 0};
            zlink_config_result_t poll_error = ZLINK_CONFIG_INTERNAL_ERROR;
            const int poll_rc =
              zlink_poll (&writable, 1, 20, &poll_error);
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, poll_error);
            if (poll_rc == 0) {
                *wait_token_out_ = id;
                assert_no_completion (stream_);
                return admitted;
            }
            TEST_ASSERT_EQUAL_INT (1, poll_rc);
            TEST_ASSERT_BITS_HIGH (ZLINK_POLLOUT, writable.revents);
            receive_writable_completion (stream_, id, user_context_, rid_);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
        TEST_ASSERT_EQUAL_UINT64 (0, id);
        ++admitted;
    }

    TEST_FAIL_MESSAGE ("STREAM send did not reach backpressure at the HWM");
    return 0;
}

struct blocking_send_result_t
{
    blocking_send_result_t () : done (false), result (ZLINK_SUBMIT_INTERNAL_ERROR),
                                completion_id (UINT64_MAX),
                                remaining_size (UINT64_MAX)
    {
    }

    std::atomic<bool> done;
    zlink_submit_result_t result;
    zlink_completion_id_t completion_id;
    uint64_t remaining_size;
};

struct send_start_latch_t
{
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
};
} // namespace

void test_stream_blocking_send_wakes_after_peer_reads ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream);
    configure_stream (stream);

    char endpoint[MAX_SOCKET_STRING];
    memset (endpoint, 0, sizeof (endpoint));
    bind_loopback_ipv4 (stream, endpoint, sizeof (endpoint));
    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    const zlink_routing_id_t rid = receive_connected_rid (stream);
    const std::vector<unsigned char> rejected_payload (payload_size, 0x5a);
    int wait_context = 71;
    zlink_completion_id_t wait_token = 0;
    TEST_ASSERT_GREATER_THAN (
      0, fill_until_backpressured (stream, &rid, rejected_payload,
                                   &wait_context, &wait_token));
    TEST_ASSERT_NOT_EQUAL (0, wait_token);

    blocking_send_result_t observed;
    send_start_latch_t start;
    std::thread sender ([&] {
        zlink_msg_t part;
        observed.result =
          zlink_msg_init_size (&part, payload_size) == ZLINK_CONFIG_OK
            ? ZLINK_SUBMIT_OK
            : ZLINK_SUBMIT_INTERNAL_ERROR;
        if (observed.result == ZLINK_SUBMIT_OK) {
            memset (zlink_msg_data (&part), 0x33, payload_size);
            {
                std::lock_guard<std::mutex> lock (start.mutex);
                start.started = true;
            }
            start.condition.notify_one ();
            errno = 0;
            observed.result = zlink_send_part_rid (
              stream, &rid, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
              NULL, &observed.completion_id);
            observed.remaining_size = zlink_msg_size (&part);
            zlink_msg_close (&part);
        }
        observed.done.store (true, std::memory_order_release);
    });

    {
        std::unique_lock<std::mutex> lock (start.mutex);
        TEST_ASSERT_TRUE (start.condition.wait_for (
          lock, std::chrono::seconds (5), [&] { return start.started; }));
    }
    unsigned char drain_buffer[64 * 1024];
    const std::chrono::steady_clock::time_point drain_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (!observed.done.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < drain_deadline)
        (void) recv_raw (raw_fd, drain_buffer, sizeof (drain_buffer));

    sender.join ();
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, observed.result);
    TEST_ASSERT_EQUAL_UINT64 (0, observed.completion_id);
    TEST_ASSERT_EQUAL_UINT64 (0, observed.remaining_size);

    receive_writable_completion (stream, wait_token, &wait_context, &rid);
    assert_no_completion (stream);

    close (raw_fd);
    test_context_socket_close_zero_linger (stream);
#endif
}

void test_stream_backpressure_wakes_pollout_and_retry_succeeds ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *stream = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (stream);
    configure_stream (stream);

    char endpoint[MAX_SOCKET_STRING];
    memset (endpoint, 0, sizeof (endpoint));
    bind_loopback_ipv4 (stream, endpoint, sizeof (endpoint));
    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    const zlink_routing_id_t rid = receive_connected_rid (stream);
    const std::vector<unsigned char> rejected_payload (payload_size, 0x5a);
    int wait_context = 72;
    zlink_completion_id_t wait_token = 0;
    TEST_ASSERT_GREATER_THAN (
      0, fill_until_backpressured (stream, &rid, rejected_payload,
                                   &wait_context, &wait_token));
    TEST_ASSERT_NOT_EQUAL (0, wait_token);

    unsigned char drain_buffer[64 * 1024];
    TEST_ASSERT_GREATER_THAN (0,
                              recv_raw (raw_fd, drain_buffer,
                                        sizeof (drain_buffer)));

    receive_writable_completion (stream, wait_token, &wait_context, &rid);

    zlink_msg_t retry;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (&retry,
                                                rejected_payload.size ()));
    memcpy (zlink_msg_data (&retry), rejected_payload.data (),
            rejected_payload.size ());
    zlink_completion_id_t completion_id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (stream, &rid, &retry,
                           ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, NULL,
                           &completion_id));
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&retry));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&retry));
    assert_no_completion (stream);

    close (raw_fd);
    test_context_socket_close_zero_linger (stream);
#endif
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_stream_blocking_send_wakes_after_peer_reads);
    RUN_TEST (test_stream_backpressure_wakes_pollout_and_retry_succeeds);
    return UNITY_END ();
}
