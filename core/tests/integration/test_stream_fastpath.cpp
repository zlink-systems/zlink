/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#if defined(ZLINK_HAVE_WINDOWS)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

SETUP_TEARDOWN_TESTCONTEXT

static const size_t stream_routing_id_size = 4;

static bool wait_monitor_event (void *monitor_,
                                void *activity_socket_,
                                uint64_t expected_event_,
                                unsigned char routing_id_[stream_routing_id_size],
                                int timeout_ms_)
{
    return wait_monitor_event_routing_id (monitor_, activity_socket_, expected_event_, routing_id_,
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

static int recv_stream_msg (void *socket_,
                            unsigned char routing_id_[stream_routing_id_size],
                            void *buf_,
                            size_t buf_size_)
{
    zlink_msg_t rid_msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&rid_msg));
    int rc = test_recv_single_msg (&rid_msg, socket_, 0);
    if (rc != static_cast<int> (stream_routing_id_size)) {
        zlink_msg_close (&rid_msg);
        return -1;
    }
    memcpy (routing_id_, zlink_msg_data (&rid_msg), stream_routing_id_size);
    if (!test_msg_has_more (&rid_msg)) {
        zlink_msg_close (&rid_msg);
        return -1;
    }
    zlink_msg_close (&rid_msg);

    zlink_msg_t payload_msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&payload_msg));
    rc = test_recv_single_msg (&payload_msg, socket_, 0);
    if (rc < 0) {
        zlink_msg_close (&payload_msg);
        return -1;
    }
    const size_t copy_size = std::min (buf_size_, zlink_msg_size (&payload_msg));
    if (buf_ && copy_size > 0)
        memcpy (buf_, zlink_msg_data (&payload_msg), copy_size);
    const int result = static_cast<int> (zlink_msg_size (&payload_msg));
    zlink_msg_close (&payload_msg);
    return result;
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

static void close_raw_fd (int fd_)
{
    if (fd_ >= 0)
        close (fd_);
}
#endif

void test_stream_fastpath_tcp_basic ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    char recv_buf[64];
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, zlink_recv (server, recv_buf, sizeof (recv_buf), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    zlink_msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1, test_recv_single_msg (&msg, server, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));

    test_context_socket_close_zero_linger (server);
}

int main (void)
{
    UNITY_BEGIN ();

    setup_test_environment ();

    RUN_TEST (test_stream_fastpath_tcp_basic);

    return UNITY_END ();
}
