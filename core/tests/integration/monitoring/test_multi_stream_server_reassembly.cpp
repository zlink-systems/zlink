/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "test_multi_stream_session_support.hpp"

#include <unity.h>

#if !defined(ZLINK_HAVE_WINDOWS)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

void setup_test_context ();
void teardown_test_context ();
void *test_context_socket (int type_);
void test_context_socket_close_zero_linger (void *socket_);
void bind_loopback_ipv4 (void *socket_, char *my_endpoint_, size_t len_);

void setUp ()
{
    setup_test_context ();
}

void tearDown ()
{
    teardown_test_context ();
}

#if defined(ZLINK_HAVE_WINDOWS)

int main ()
{
    TEST_IGNORE_MESSAGE ("POSIX raw tcp regression test");
    return 77;
}

#else

namespace
{

static const size_t kEndpointSize = 256;

bool parse_tcp_endpoint (const char *endpoint_, char host_[64], int *port_)
{
    if (!endpoint_ || !host_ || !port_)
        return false;

    char proto[8] = {0};
    int port = 0;
    if (std::sscanf (endpoint_, "%7[^:]://%63[^:]:%d", proto, host_, &port) != 3) {
        return false;
    }
    if (std::strcmp (proto, "tcp") != 0 || port <= 0 || port > 65535)
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
    std::memset (&addr, 0, sizeof (addr));
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

void close_raw_fd (int fd_)
{
    if (fd_ >= 0)
        close (fd_);
}

int set_raw_fd_timeout (int fd_, int timeout_ms_)
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

int recv_exact (int fd_, unsigned char *buf_, size_t size_)
{
    size_t off = 0;
    while (off < size_) {
        const ssize_t n = recv (fd_, buf_ + off, size_ - off, 0);
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

bool recv_should_timeout (int fd_)
{
    unsigned char byte = 0;
    const ssize_t n = recv (fd_, &byte, 1, 0);
    if (n >= 0)
        return false;

    return errno == EAGAIN || errno == EWOULDBLOCK;
}

std::vector<unsigned char> make_frame (const char *payload_)
{
    const size_t payload_size = std::strlen (payload_);
    std::vector<unsigned char> frame (6 + payload_size);
    frame[0] = 0;
    frame[1] = 0;
    test_stream_common::store_u32_be (&frame[2], static_cast<uint32_t> (payload_size));
    std::memcpy (&frame[6], payload_, payload_size);
    return frame;
}

} // namespace

void test_multi_stream_server_reassembles_complete_frames_per_connection ()
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);

    char endpoint[kEndpointSize];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    test_multi_stream::session_t session;
    test_multi_stream::packet_handler_context_t packet_handler_ctx;
    test_multi_stream::stop_requested ().store (false, std::memory_order_release);
    test_multi_stream::reset_session (&session, server);
    packet_handler_ctx.session = &session;
    packet_handler_ctx.stop_token = "STOP";
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_stream_packet_handler (server, &test_multi_stream::stream_packet_handler_callback,
                                   &packet_handler_ctx));
    int server_rc = -1;

    std::thread server_thread ([&session, server, &server_rc] () {
        server_rc = test_multi_stream::run_server_event_loop (&session, server, "STOP", NULL, NULL);
    });

    const int client_a = connect_raw_tcp (endpoint);
    const int client_b = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_a >= 0);
    TEST_ASSERT_TRUE (client_b >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_a, 200));
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_b, 1000));
    msleep (SETTLE_TIME);

    const std::vector<unsigned char> frame_a = make_frame ("client-a-frame");
    const std::vector<unsigned char> frame_b = make_frame ("client-b-frame");

    TEST_ASSERT_EQUAL_INT (0, send_all (client_a, &frame_a[0], 2));
    TEST_ASSERT_TRUE (recv_should_timeout (client_a));

    TEST_ASSERT_EQUAL_INT (0, send_all (client_b, &frame_b[0], 4));
    TEST_ASSERT_TRUE (recv_should_timeout (client_b));

    TEST_ASSERT_EQUAL_INT (0, send_all (client_a, &frame_a[2], static_cast<size_t> (4)));
    TEST_ASSERT_TRUE (recv_should_timeout (client_a));

    TEST_ASSERT_EQUAL_INT (0, send_all (client_b, &frame_b[4], frame_b.size () - 4));

    std::vector<unsigned char> echo_b (frame_b.size ());
    TEST_ASSERT_EQUAL_INT (0, recv_exact (client_b, &echo_b[0], echo_b.size ()));
    TEST_ASSERT_EQUAL_MEMORY (&frame_b[0], &echo_b[0], frame_b.size ());
    TEST_ASSERT_TRUE (recv_should_timeout (client_a));

    TEST_ASSERT_EQUAL_INT (0, send_all (client_a, &frame_a[6], frame_a.size () - 6));

    std::vector<unsigned char> echo_a (frame_a.size ());
    TEST_ASSERT_EQUAL_INT (0, recv_exact (client_a, &echo_a[0], echo_a.size ()));
    TEST_ASSERT_EQUAL_MEMORY (&frame_a[0], &echo_a[0], frame_a.size ());

    test_multi_stream::stop_requested ().store (true, std::memory_order_release);
    server_thread.join ();
    TEST_ASSERT_EQUAL_INT (0, server_rc);

    test_multi_stream::clear_session (&session);
    close_raw_fd (client_a);
    close_raw_fd (client_b);
    test_context_socket_close_zero_linger (server);
}

int main (int, char **)
{
    UNITY_BEGIN ();
    RUN_TEST (test_multi_stream_server_reassembles_complete_frames_per_connection);
    return UNITY_END ();
}

#endif
