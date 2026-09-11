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

static fd_t connect_raw_tcp (const char *endpoint_)
{
    return connect_socket (endpoint_, AF_INET, IPPROTO_TCP);
}

static int send_all (fd_t fd_, const unsigned char *buf_, size_t size_)
{
    size_t off = 0;
    while (off < size_) {
#if defined(ZLINK_HAVE_WINDOWS)
        const int n = send (fd_, reinterpret_cast<const char *> (buf_ + off),
                            static_cast<int> (size_ - off), 0);
        if (n == SOCKET_ERROR && WSAGetLastError () == WSAEINTR)
            continue;
#else
        const ssize_t n = send (fd_, buf_ + off, size_ - off, 0);
        if (n < 0 && errno == EINTR)
            continue;
#endif
        if (n > 0) {
            off += static_cast<size_t> (n);
            continue;
        }
        return -1;
    }
    return 0;
}

static int send_stream_packet (fd_t fd_, const void *data_, size_t size_)
{
    return send_all (fd_, static_cast<const unsigned char *> (data_), size_);
}

static void close_raw_fd (fd_t fd_)
{
    if (fd_ != retired_fd)
        close (fd_);
}

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

    const fd_t client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd != retired_fd);

    const char payload[] = "x";
    TEST_ASSERT_EQUAL_INT (0, send_stream_packet (client_fd, payload, sizeof (payload) - 1));

    const zlink_routing_id_t *rid = NULL;
    zlink_msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    size_t has_more = 0;
    zlink_recv_result_t recv_result = ZLINK_RECV_NO_DATA;
    for (int i = 0; i < 200 && recv_result == ZLINK_RECV_NO_DATA; ++i) {
        recv_result = zlink_recv (server, &rid, &received, 1, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
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
    TEST_ASSERT_EQUAL_INT (1, has_more);
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
