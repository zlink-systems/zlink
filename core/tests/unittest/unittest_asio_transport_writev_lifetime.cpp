/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"

#include "platform.hpp"
#include "transports/tcp/tcp_transport.hpp"
#if defined ZLINK_HAVE_IPC
#include "transports/ipc/ipc_transport.hpp"
#endif

#include <unity.h>

#if !defined ZLINK_HAVE_WINDOWS
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
struct connected_fds_t
{
    int writer;
    int reader;
};

connected_fds_t make_tcp_connection ()
{
    const int listener = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, listener);

    sockaddr_in address;
    std::memset (&address, 0, sizeof (address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    address.sin_port = 0;
    TEST_ASSERT_EQUAL_INT (
      0, bind (listener, reinterpret_cast<const sockaddr *> (&address), sizeof (address)));
    TEST_ASSERT_EQUAL_INT (0, listen (listener, 1));

    socklen_t address_size = sizeof (address);
    TEST_ASSERT_EQUAL_INT (
      0, getsockname (listener, reinterpret_cast<sockaddr *> (&address), &address_size));

    const int reader = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, reader);
    TEST_ASSERT_EQUAL_INT (
      0, connect (reader, reinterpret_cast<const sockaddr *> (&address), sizeof (address)));

    const int writer = accept (listener, NULL, NULL);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, writer);
    TEST_ASSERT_EQUAL_INT (0, close (listener));

    const connected_fds_t connection = {writer, reader};
    return connection;
}

#if defined ZLINK_HAVE_IPC
connected_fds_t make_ipc_connection ()
{
    int sockets[2] = {-1, -1};
    TEST_ASSERT_EQUAL_INT (0, socketpair (AF_UNIX, SOCK_STREAM, 0, sockets));
    const connected_fds_t connection = {sockets[0], sockets[1]};
    return connection;
}
#endif

void fill_send_buffer (int fd_)
{
    const int send_buffer_size = 4096;
    TEST_ASSERT_EQUAL_INT (
      0, setsockopt (fd_, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof (send_buffer_size)));

    unsigned char data[4096];
    std::memset (data, 0xA5, sizeof (data));
    size_t total = 0;
    for (;;) {
        const ssize_t rc = send (fd_, data, sizeof (data), MSG_DONTWAIT);
        if (rc > 0) {
            total += static_cast<size_t> (rc);
            TEST_ASSERT_LESS_THAN_UINT64 (64U * 1024U * 1024U, total);
            continue;
        }
        if (rc < 0 && errno == EINTR)
            continue;
        TEST_ASSERT_EQUAL_INT (-1, rc);
        TEST_ASSERT_TRUE (errno == EAGAIN || errno == EWOULDBLOCK);
        return;
    }
}

template <typename Transport>
void assert_immediate_writev_releases_completion (Transport &transport_,
                                                  const connected_fds_t &connection_)
{
    boost::asio::io_context io_context;
    TEST_ASSERT_TRUE (transport_.open (io_context, connection_.writer));

    int completion_count = 0;
    std::size_t completion_bytes = 0;
    boost::system::error_code completion_error;
    bool call_returned = false;
    bool completed_inline = false;
    std::shared_ptr<int> lifetime (new int (1));
    const std::weak_ptr<int> weak_lifetime = lifetime;
    const unsigned char header = 0x01;
    const unsigned char body = 0x02;

    auto completion = [lifetime, &completion_count, &completion_bytes, &completion_error,
                       &call_returned,
                       &completed_inline] (const boost::system::error_code &ec,
                                           std::size_t bytes) {
        ++completion_count;
        completion_bytes = bytes;
        completion_error = ec;
        completed_inline = !call_returned;
    };
    lifetime.reset ();

    transport_.async_writev (&header, sizeof (header), &body, sizeof (body),
                             std::move (completion));
    call_returned = true;

    TEST_ASSERT_EQUAL_INT (1, completion_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (header) + sizeof (body), completion_bytes);
    TEST_ASSERT_FALSE (completion_error);
    TEST_ASSERT_TRUE (completed_inline);
    TEST_ASSERT_TRUE (weak_lifetime.expired ());

    io_context.poll ();
    TEST_ASSERT_EQUAL_INT (1, completion_count);

    transport_.close ();
    TEST_ASSERT_EQUAL_INT (0, close (connection_.reader));
}

template <typename Transport>
void assert_pending_writev_releases_completion (Transport &transport_,
                                                const connected_fds_t &connection_)
{
    boost::asio::io_context io_context;
    TEST_ASSERT_TRUE (transport_.open (io_context, connection_.writer));
    fill_send_buffer (connection_.writer);

    int completion_count = 0;
    std::size_t completion_bytes = 0;
    boost::system::error_code completion_error;
    std::shared_ptr<int> lifetime (new int (1));
    const std::weak_ptr<int> weak_lifetime = lifetime;
    const unsigned char header = 0x01;
    const unsigned char body = 0x02;

    transport_.async_writev (
      &header, sizeof (header), &body, sizeof (body),
      [lifetime, &completion_count, &completion_bytes,
       &completion_error] (const boost::system::error_code &ec, std::size_t bytes) {
          ++completion_count;
          completion_bytes = bytes;
          completion_error = ec;
      });
    lifetime.reset ();

    TEST_ASSERT_EQUAL_INT (0, completion_count);
    TEST_ASSERT_FALSE (weak_lifetime.expired ());

    transport_.close ();
    io_context.run ();

    TEST_ASSERT_EQUAL_INT (1, completion_count);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_bytes);
    TEST_ASSERT_TRUE (completion_error == boost::asio::error::operation_aborted
                      || completion_error == boost::asio::error::bad_descriptor);
    TEST_ASSERT_TRUE (weak_lifetime.expired ());
    TEST_ASSERT_EQUAL_INT (0, close (connection_.reader));
}
}
#endif

void setUp ()
{
}

void tearDown ()
{
}

void test_tcp_pending_writev_releases_completion_after_close ()
{
#if defined ZLINK_HAVE_WINDOWS
    TEST_IGNORE_MESSAGE ("native writev continuation is Unix-specific");
#else
    zlink::tcp_transport_t transport;
    assert_pending_writev_releases_completion (transport, make_tcp_connection ());
#endif
}

void test_tcp_immediate_writev_completes_inline_and_releases_completion ()
{
#if defined ZLINK_HAVE_WINDOWS
    TEST_IGNORE_MESSAGE ("native writev fast path test is Unix-specific");
#else
    zlink::tcp_transport_t transport;
    assert_immediate_writev_releases_completion (transport, make_tcp_connection ());
#endif
}

void test_ipc_pending_writev_releases_completion_after_close ()
{
#if defined ZLINK_HAVE_IPC && !defined ZLINK_HAVE_WINDOWS
    zlink::ipc_transport_t transport;
    assert_pending_writev_releases_completion (transport, make_ipc_connection ());
#else
    TEST_IGNORE_MESSAGE ("IPC native writev continuation is unavailable");
#endif
}

void test_ipc_immediate_writev_completes_inline_and_releases_completion ()
{
#if defined ZLINK_HAVE_IPC && !defined ZLINK_HAVE_WINDOWS
    zlink::ipc_transport_t transport;
    assert_immediate_writev_releases_completion (transport, make_ipc_connection ());
#else
    TEST_IGNORE_MESSAGE ("IPC native writev fast path is unavailable");
#endif
}

int main ()
{
    UNITY_BEGIN ();
    RUN_TEST (test_tcp_immediate_writev_completes_inline_and_releases_completion);
    RUN_TEST (test_tcp_pending_writev_releases_completion_after_close);
    RUN_TEST (test_ipc_immediate_writev_completes_inline_and_releases_completion);
    RUN_TEST (test_ipc_pending_writev_releases_completion_after_close);
    return UNITY_END ();
}
