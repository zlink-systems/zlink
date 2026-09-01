/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
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

zlink_completion_id_t create_stably_pending_send (
  void *stream_, const zlink_routing_id_t *rid_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (5);
    std::vector<unsigned char> payload (payload_size, 0x5a);

    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_msg_init_size (&part, payload.size ()));
        memcpy (zlink_msg_data (&part), &payload[0], payload.size ());
        zlink_completion_id_t id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part_rid (stream_, rid_, &part,
                               ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
                               NULL, &id));
        TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        if (id == 0)
            continue;

        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        zlink_completion_t completion;
        memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t receive_result = zlink_completion_recv (
          stream_, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (receive_result == ZLINK_RECV_NO_DATA) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            return id;
        }

        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, receive_result);
        TEST_ASSERT_EQUAL_UINT64 (id, completion.completion_id);
        TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
        zlink_completion_close (&completion);
    }

    TEST_FAIL_MESSAGE ("STREAM send did not remain pending at the HWM");
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
    const zlink_completion_id_t pending_id =
      create_stably_pending_send (stream, &rid);
    TEST_ASSERT_NOT_EQUAL (0, pending_id);

    blocking_send_result_t observed;
    std::thread sender ([&] {
        zlink_msg_t part;
        observed.result =
          zlink_msg_init_size (&part, payload_size) == ZLINK_CONFIG_OK
            ? ZLINK_SUBMIT_OK
            : ZLINK_SUBMIT_INTERNAL_ERROR;
        if (observed.result == ZLINK_SUBMIT_OK) {
            memset (zlink_msg_data (&part), 0x33, payload_size);
            errno = 0;
            observed.result = zlink_send_part_rid (
              stream, &rid, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
              NULL, &observed.completion_id);
            observed.remaining_size = zlink_msg_size (&part);
            zlink_msg_close (&part);
        }
        observed.done.store (true, std::memory_order_release);
    });

    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    TEST_ASSERT_FALSE (observed.done.load (std::memory_order_acquire));

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

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (stream, &completion, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (pending_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, completion.kind);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
    zlink_completion_close (&completion);

    close (raw_fd);
    test_context_socket_close_zero_linger (stream);
#endif
}

void test_stream_pending_send_detach_completes_once ()
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
    const zlink_completion_id_t pending_id =
      create_stably_pending_send (stream, &rid);
    TEST_ASSERT_NOT_EQUAL (0, pending_id);

    close (raw_fd);

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (stream, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    zlink_completion_close (&completion);

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect_rid (stream, &rid));

    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (stream, &completion, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_SEND, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (pending_id, completion.completion_id);
    TEST_ASSERT_EQUAL_UINT (rid.size, completion.peer_rid.size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY (rid.data, completion.peer_rid.data, rid.size);
    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_TERMINAL, completion.send_result);
    TEST_ASSERT_EQUAL_INT (ENOENT, completion.send_terminal_errno);
    zlink_completion_close (&completion);

    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_completion_recv (stream, &completion,
                             ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    zlink_completion_close (&completion);

    test_context_socket_close_zero_linger (stream);
#endif
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_stream_blocking_send_wakes_after_peer_reads);
    RUN_TEST (test_stream_pending_send_detach_completes_once);
    return UNITY_END ();
}
