/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink.h>
#include <unity.h>

#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

void setUp () {}
void tearDown () {}

namespace
{
namespace net = boost::asio;
namespace ws = boost::beast::websocket;
typedef net::ip::tcp tcp;
const int timeout_ms = 3000;

struct fixture_t
{
    fixture_t () : context (zlink_ctx_new ()), socket (NULL), monitor (NULL),
                   poller (zlink_poller_new ())
    {
        TEST_ASSERT_NOT_NULL (context);
        socket = zlink_socket (context, ZLINK_SOCKET_STREAM);
        TEST_ASSERT_NOT_NULL (socket);
        const zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
        const uint64_t hwm = 0;
        const int linger = 0;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_stream_option (
          socket, ZLINK_STREAM_OPT_RECV_MODE, &mode, sizeof (mode)));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_option (
          socket, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_option (
          socket, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_option (
          socket, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
        zlink_socket_monitor_open_options_t options = {};
        options.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
        monitor = zlink_socket_monitor_open (socket, &options);
        TEST_ASSERT_NOT_NULL (monitor);
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                               zlink_bind (socket, "ws://127.0.0.1:*"));
        char endpoint[256];
        size_t size = sizeof (endpoint);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_get_option (
          socket, ZLINK_OPT_LAST_ENDPOINT, endpoint, &size));
        port = static_cast<unsigned short> (
          std::strtoul (std::strrchr (endpoint, ':') + 1, NULL, 10));
        TEST_ASSERT_NOT_NULL (poller);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_poller_add (poller, socket, socket, ZLINK_POLLIN));
    }

    ~fixture_t ()
    {
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
        if (monitor)
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (context));
    }

    uint64_t pending ()
    {
        zlink_monitor_status_t status = {};
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_monitor_status (monitor, &status));
        return status.rcv_pending_msgs;
    }

    void await_input (uint64_t chunks_)
    {
        const auto end = std::chrono::steady_clock::now ()
                         + std::chrono::milliseconds (timeout_ms);
        while (pending () < chunks_ && std::chrono::steady_clock::now () < end)
            std::this_thread::yield ();
        TEST_ASSERT_TRUE_MESSAGE (pending () >= chunks_, "transport did not queue all fragments");
    }

    void expect_packet (const std::string &header_, const std::string &body_, bool blocking_ = false)
    {
        if (!blocking_) {
            zlink_poller_event_t event = {};
            TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (poller, &event, 1, timeout_ms, NULL));
            TEST_ASSERT_EQUAL_PTR (socket, event.socket);
            TEST_ASSERT_TRUE (event.events & ZLINK_POLLIN);
        }
        zlink_msg_t header, body;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&header));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&body));
        const zlink_routing_id_t *rid = NULL;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_stream_recv_packet (
          socket, &rid, &header, &body,
          blocking_ ? ZLINK_RECV_FLAGS_NONE : ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_NOT_NULL (rid);
        TEST_ASSERT_EQUAL_UINT64 (header_.size (), zlink_msg_size (&header));
        TEST_ASSERT_EQUAL_UINT64 (body_.size (), zlink_msg_size (&body));
        if (!header_.empty ())
            TEST_ASSERT_EQUAL_MEMORY (header_.data (), zlink_msg_data (&header), header_.size ());
        if (!body_.empty ())
            TEST_ASSERT_EQUAL_MEMORY (body_.data (), zlink_msg_data (&body), body_.size ());
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&header));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&body));
    }

    void *context, *socket, *monitor, *poller;
    unsigned short port;
};

struct client_t
{
    explicit client_t (unsigned short port_) : socket (io)
    {
        socket.next_layer ().connect (tcp::endpoint (net::ip::make_address ("127.0.0.1"), port_));
        socket.handshake ("127.0.0.1", "/");
        socket.binary (true);
    }
    ~client_t ()
    {
        boost::system::error_code ignored;
        socket.next_layer ().close (ignored);
    }
    void write (const std::string &bytes_) { socket.write (net::buffer (bytes_)); }
    void fragment (const std::string &bytes_)
    {
        // Separate WS messages give public transport boundaries; no private
        // pipe injection or assumptions about TCP write/read coalescing.
        for (size_t i = 0; i < bytes_.size (); ++i)
            socket.write (net::buffer (bytes_.data () + i, 1));
    }
    net::io_context io;
    ws::stream<tcp::socket> socket;
};

std::string packet (const std::string &header_, const std::string &body_)
{
    std::string bytes (6, '\0');
    bytes[0] = static_cast<char> (header_.size () >> 8);
    bytes[1] = static_cast<char> (header_.size ());
    for (int i = 0; i < 4; ++i)
        bytes[2 + i] = static_cast<char> (body_.size () >> (24 - 8 * i));
    return bytes + header_ + body_;
}

void buffered_fragments (bool blocking_, bool async_owner_)
{
    fixture_t f;
    client_t client (f.port);
    const std::string body (512, 'b');
    const std::string bytes = packet ("h", body);
    client.fragment (bytes);
    f.await_input (bytes.size ());
    if (!async_owner_)
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&f.monitor));
    f.expect_packet ("h", body, blocking_);
    zlink_poller_event_t event = {};
    TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (f.poller, &event, 1, 0, NULL));
}
}

void test_buffered_poll () { buffered_fragments (false, true); }
void test_buffered_recv () { buffered_fragments (true, true); }
void test_buffered_poll_without_monitor () { buffered_fragments (false, false); }
void test_buffered_recv_without_monitor () { buffered_fragments (true, false); }

void test_two_sessions ()
{
    fixture_t f;
    client_t large (f.port), small (f.port);
    const std::string body (4096, 'L');
    const std::string bytes = packet ("large", body);
    large.fragment (bytes.substr (0, bytes.size () - 1));
    for (int i = 0; i < 16; ++i)
        small.write (packet (std::to_string (i), "small"));
    f.await_input (bytes.size () - 1 + 16);
    for (int i = 0; i < 16; ++i)
        f.expect_packet (std::to_string (i), "small");
    large.write (bytes.substr (bytes.size () - 1));
    large.write (packet ("next", "ordered"));
    f.expect_packet ("large", body);
    f.expect_packet ("next", "ordered");
}

void test_idle_final_fragment ()
{
    fixture_t f;
    client_t client (f.port);
    const std::string bytes = packet ("gap", "body");
    client.fragment (bytes.substr (0, bytes.size () - 1));
    f.await_input (bytes.size () - 1);
    zlink_poller_event_t event = {};
    // An actual idle poll establishes false packet readiness and consumes the
    // old activation before a later transport input supplies the last byte.
    TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (f.poller, &event, 1, 10, NULL));
    client.write (bytes.substr (bytes.size () - 1));
    f.expect_packet ("gap", "body");
    TEST_ASSERT_EQUAL_INT (0, zlink_poller_wait (f.poller, &event, 1, 0, NULL));
}

void test_control_progress ()
{
    fixture_t f;
    client_t client (f.port);
    void *timer = zlink_timer_new ();
    TEST_ASSERT_NOT_NULL (timer);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_add_timer (
      f.poller, timer, timer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_timer_start (timer, 1, 1));
    zlink_poller_event_t event = {};
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (f.poller, &event, 1, timeout_ms, NULL));
    TEST_ASSERT_EQUAL_PTR (timer, event.user_data);
    // Leave the fire count unread so control readiness is already true when
    // STREAM starts decoding the subsequently buffered partial packet.
    const std::string bytes = packet ("long", std::string (8192, 'p'));
    client.fragment (bytes.substr (0, bytes.size () - 1));
    f.await_input (bytes.size () - 1);
    const auto begin = std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (1, zlink_poller_wait (f.poller, &event, 1, timeout_ms, NULL));
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds> (
      std::chrono::steady_clock::now () - begin).count ();
    TEST_ASSERT_EQUAL_PTR (timer, event.user_data);
    const uint64_t remaining = f.pending ();
    std::printf ("control poll: %lld us, %llu partial chunks remain\n",
                 static_cast<long long> (elapsed), static_cast<unsigned long long> (remaining));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove_timer (f.poller, timer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_timer_destroy (&timer));
    // Other event-loop work must get a turn without exhausting an unrelated
    // peer's incomplete packet. This is a progress assertion, not a latency SLA.
    TEST_ASSERT_TRUE_MESSAGE (remaining > 0, "partial-packet drain exhausted input before control got a turn");
    client.write (bytes.substr (bytes.size () - 1));
    f.expect_packet ("long", std::string (8192, 'p'));
}

void test_shutdown_during_drain ()
{
    fixture_t f;
    client_t client (f.port);
    const std::string bytes = packet ("stop", std::string (262144, 'p'));
    client.fragment (bytes.substr (0, bytes.size () - 1));
    f.await_input (bytes.size () - 1);
    std::atomic<bool> observing (false);
    int shutdown_rc = -1;
    int monitor_rc = ZLINK_CONFIG_OK;
    uint64_t at_shutdown = 0;
    std::thread control ([&] {
        const auto end = std::chrono::steady_clock::now ()
                         + std::chrono::milliseconds (timeout_ms);
        zlink_monitor_status_t status = {};
        do {
            monitor_rc = zlink_monitor_status (f.monitor, &status);
            if (monitor_rc != ZLINK_CONFIG_OK) {
                observing.store (true, std::memory_order_release);
                break;
            }
            observing.store (true, std::memory_order_release);
        } while (status.rcv_pending_msgs == bytes.size () - 1
                 && std::chrono::steady_clock::now () < end);
        at_shutdown = status.rcv_pending_msgs;
        shutdown_rc = zlink_ctx_shutdown (f.context);
    });
    while (!observing.load (std::memory_order_acquire))
        std::this_thread::yield ();
    zlink_msg_t header, body;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&header));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&body));
    const zlink_recv_result_t rc = zlink_stream_recv_packet (
      f.socket, NULL, &header, &body, ZLINK_RECV_FLAGS_NONE);
    control.join ();
    const uint64_t remaining = f.pending ();
    std::printf ("shutdown: %llu chunks at request, %llu at receive return\n",
                 static_cast<unsigned long long> (at_shutdown),
                 static_cast<unsigned long long> (remaining));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, shutdown_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, monitor_rc);
    TEST_ASSERT_TRUE (at_shutdown < bytes.size () - 1);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_TERMINATED, rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&header));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&body));
    TEST_ASSERT_TRUE_MESSAGE (remaining > 0, "receive ignored shutdown until all partial input was drained");
}

int main ()
{
    if (!zlink_has ("ws"))
        return 77;
    UNITY_BEGIN ();
    const char *selected = std::getenv ("ZLINK_TEST_CASE");
#define RUN_SELECTED(name) \
    if (!selected || std::strcmp (selected, #name) == 0) RUN_TEST (name)
    RUN_SELECTED (test_buffered_poll);
    RUN_SELECTED (test_buffered_recv);
    RUN_SELECTED (test_buffered_poll_without_monitor);
    RUN_SELECTED (test_buffered_recv_without_monitor);
    RUN_SELECTED (test_two_sessions);
    RUN_SELECTED (test_idle_final_fragment);
    RUN_SELECTED (test_control_progress);
    RUN_SELECTED (test_shutdown_during_drain);
    return UNITY_END ();
}
