/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
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

static void close_raw_fd (int fd_)
{
    if (fd_ >= 0)
        close (fd_);
}
#endif

void test_stream_auto_routing_id_size ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    const zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_RAW;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof endpoint);

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);

    const char payload[] = "x";
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet (client_fd, payload, sizeof (payload) - 1));

    const zlink_routing_id_t *rid = NULL;
    zlink_msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_recv_result_t recv_result = ZLINK_RECV_NO_DATA;
    for (int i = 0; i < 200 && recv_result == ZLINK_RECV_NO_DATA; ++i) {
        recv_result = zlink_recv_part (server, &rid, &received, &has_more,
                                       ZLINK_RECV_FLAGS_DONTWAIT);
        if (recv_result != ZLINK_RECV_NO_DATA)
            break;
#if defined(ZLINK_HAVE_WINDOWS)
        Sleep (10);
#else
        usleep (10000);
#endif
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, recv_result);
    TEST_ASSERT_NOT_NULL (rid);
    TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, rid->size);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_UINT64 (1, zlink_msg_size (&received));
    TEST_ASSERT_EQUAL_UINT8 ('x',
                             *static_cast<unsigned char *> (
                               zlink_msg_data (&received)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    close_raw_fd (client_fd);
    test_context_socket_close_zero_linger (server);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_stream_auto_routing_id_size);
    return UNITY_END ();
}
