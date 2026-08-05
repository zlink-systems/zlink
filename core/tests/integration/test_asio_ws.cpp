/* SPDX-License-Identifier: MPL-2.0 */

/*
 * Test suite for the ASIO WebSocket infrastructure
 *
 * These tests verify that the Boost.Beast WebSocket layer works correctly
 * for WebSocket connections. The actual ZLINK WebSocket integration will
 * use ws_transport_t which wraps these primitives.
 *
 * NOTE: These tests require Boost.Beast and are only compiled when
 * ZLINK_HAVE_ASIO_WS is defined.
 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <unity.h>

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <string>
#include <string.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <thread>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

void setUp ()
{
}

void tearDown ()
{
}

//  Test 1: WebSocket stream creation
void test_ws_stream_creation ()
{
    net::io_context io_context;

    //  Create WebSocket stream over TCP socket
    websocket::stream<tcp::socket> ws_stream (io_context);

    //  Check that the lowest layer (TCP socket) exists
    TEST_ASSERT_FALSE (ws_stream.next_layer ().is_open ());

    //  Configure stream for binary mode
    ws_stream.binary (true);
}

//  Test 2: WebSocket stream configuration options
void test_ws_stream_options ()
{
    net::io_context io_context;
    websocket::stream<tcp::socket> ws_stream (io_context);

    //  Test binary mode setting
    ws_stream.binary (true);

    //  Test auto-fragmentation
    ws_stream.auto_fragment (false);

    //  Test message size limit
    ws_stream.read_message_max (16 * 1024 * 1024); // 16 MB
}

//  Test 3: TCP connection baseline (without WebSocket upgrade)
void test_tcp_connection_baseline ()
{
    net::io_context io_context;

    //  Create acceptor on ephemeral port
    tcp::acceptor acceptor (io_context, tcp::endpoint (net::ip::make_address ("127.0.0.1"), 0));
    auto endpoint = acceptor.local_endpoint ();

    //  Server socket
    tcp::socket server_socket (io_context);

    //  Client socket
    tcp::socket client_socket (io_context);

    //  State tracking
    std::atomic<bool> all_done{false};
    std::atomic<int> completed{0};
    boost::system::error_code accept_ec, connect_ec;

    //  Async accept
    acceptor.async_accept (server_socket, [&] (const boost::system::error_code &ec) {
        accept_ec = ec;
        if (++completed >= 2)
            all_done = true;
    });

    //  Async connect
    client_socket.async_connect (endpoint, [&] (const boost::system::error_code &ec) {
        connect_ec = ec;
        if (++completed >= 2)
            all_done = true;
    });

    //  Run io_context with timeout using thread
    std::thread io_thread ([&] () { io_context.run (); });

    //  Wait for completion with timeout (5 seconds)
    for (int i = 0; i < 50 && !all_done; ++i) {
        msleep (100);
    }

    if (!all_done) {
        io_context.stop ();
    }
    io_thread.join ();

    TEST_ASSERT_TRUE_MESSAGE (all_done.load (), "TCP connection timed out");
    TEST_ASSERT_FALSE_MESSAGE (accept_ec.failed (), accept_ec.message ().c_str ());
    TEST_ASSERT_FALSE_MESSAGE (connect_ec.failed (), connect_ec.message ().c_str ());
    TEST_ASSERT_TRUE (server_socket.is_open ());
    TEST_ASSERT_TRUE (client_socket.is_open ());
}

//  Test 4: WebSocket handshake (client/server)
void test_ws_handshake ()
{
    net::io_context io_context;

    //  Create work guard to keep io_context running
    auto work = net::make_work_guard (io_context);

    //  Create acceptor on ephemeral port
    tcp::acceptor acceptor (io_context, tcp::endpoint (net::ip::make_address ("127.0.0.1"), 0));
    auto endpoint = acceptor.local_endpoint ();

    //  Build host string for handshake
    std::string host = "127.0.0.1:" + std::to_string (endpoint.port ());

    //  Server WebSocket stream
    websocket::stream<tcp::socket> server_ws (io_context);

    //  Client WebSocket stream
    websocket::stream<tcp::socket> client_ws (io_context);

    //  State tracking
    std::atomic<bool> all_done{false};
    std::atomic<int> stage{0};
    boost::system::error_code server_hs_ec, client_hs_ec;

    //  Set up the chain of operations
    acceptor.async_accept (server_ws.next_layer (), [&] (const boost::system::error_code &ec) {
        if (ec) {
            all_done = true;
            work.reset ();
            return;
        }

        //  Server: accept WebSocket upgrade
        server_ws.async_accept ([&] (const boost::system::error_code &ec) {
            server_hs_ec = ec;
            if (++stage >= 2) {
                all_done = true;
                work.reset ();
            }
        });
    });

    client_ws.next_layer ().async_connect (endpoint, [&] (const boost::system::error_code &ec) {
        if (ec) {
            all_done = true;
            work.reset ();
            return;
        }

        //  Client: perform WebSocket handshake
        client_ws.async_handshake (host, "/zlink", [&] (const boost::system::error_code &ec) {
            client_hs_ec = ec;
            if (++stage >= 2) {
                all_done = true;
                work.reset ();
            }
        });
    });

    //  Run io_context in a separate thread
    std::thread io_thread ([&] () { io_context.run (); });

    //  Wait for completion with timeout (5 seconds)
    for (int i = 0; i < 50 && !all_done; ++i) {
        msleep (100);
    }

    if (!all_done) {
        work.reset ();
        io_context.stop ();
    }
    io_thread.join ();

    TEST_ASSERT_TRUE_MESSAGE (all_done.load (), "WebSocket handshake timed out");
    TEST_ASSERT_FALSE_MESSAGE (server_hs_ec.failed (), server_hs_ec.message ().c_str ());
    TEST_ASSERT_FALSE_MESSAGE (client_hs_ec.failed (), client_hs_ec.message ().c_str ());

    //  Note: Don't call sync close - it blocks when io_context is stopped
    //  Let the streams destruct naturally
}

//  Test 5: WebSocket binary message exchange
void test_ws_binary_message ()
{
    net::io_context io_context;

    //  Create work guard to keep io_context running
    auto work = net::make_work_guard (io_context);

    //  Create acceptor on ephemeral port
    tcp::acceptor acceptor (io_context, tcp::endpoint (net::ip::make_address ("127.0.0.1"), 0));
    auto endpoint = acceptor.local_endpoint ();

    std::string host = "127.0.0.1:" + std::to_string (endpoint.port ());

    //  Server and client WebSocket streams
    websocket::stream<tcp::socket> server_ws (io_context);
    websocket::stream<tcp::socket> client_ws (io_context);

    //  Enable binary mode for both
    server_ws.binary (true);
    client_ws.binary (true);

    //  Test data
    const unsigned char test_data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0xFF, 0xFE, 0xFD};
    const std::size_t test_data_size = sizeof (test_data);

    //  State tracking
    std::atomic<bool> all_done{false};
    beast::flat_buffer recv_buffer;
    boost::system::error_code write_ec, read_ec;
    std::size_t bytes_written = 0;
    std::size_t bytes_read = 0;

    //  Set up the chain of operations
    acceptor.async_accept (server_ws.next_layer (), [&] (const boost::system::error_code &ec) {
        if (ec) {
            all_done = true;
            work.reset ();
            return;
        }

        //  Server: accept WebSocket upgrade, then read
        server_ws.async_accept ([&] (const boost::system::error_code &ec) {
            if (ec) {
                all_done = true;
                work.reset ();
                return;
            }

            //  Server reads binary data
            server_ws.async_read (recv_buffer,
                                  [&] (const boost::system::error_code &ec, std::size_t n) {
                                      read_ec = ec;
                                      bytes_read = n;
                                      all_done = true;
                                      work.reset ();
                                  });
        });
    });

    client_ws.next_layer ().async_connect (endpoint, [&] (const boost::system::error_code &ec) {
        if (ec) {
            all_done = true;
            work.reset ();
            return;
        }

        //  Client: perform WebSocket handshake, then write
        client_ws.async_handshake (host, "/zlink", [&] (const boost::system::error_code &ec) {
            if (ec) {
                all_done = true;
                work.reset ();
                return;
            }

            //  Client writes binary data
            client_ws.async_write (net::buffer (test_data, test_data_size),
                                   [&] (const boost::system::error_code &ec, std::size_t n) {
                                       write_ec = ec;
                                       bytes_written = n;
                                   });
        });
    });

    //  Run io_context in a separate thread
    std::thread io_thread ([&] () { io_context.run (); });

    //  Wait for completion with timeout (5 seconds)
    for (int i = 0; i < 50 && !all_done; ++i) {
        msleep (100);
    }

    if (!all_done) {
        work.reset ();
        io_context.stop ();
    }
    io_thread.join ();

    TEST_ASSERT_TRUE_MESSAGE (all_done.load (), "WebSocket message exchange timed out");
    TEST_ASSERT_FALSE_MESSAGE (write_ec.failed (), write_ec.message ().c_str ());
    TEST_ASSERT_FALSE_MESSAGE (read_ec.failed (), read_ec.message ().c_str ());
    TEST_ASSERT_EQUAL_UINT64 (test_data_size, bytes_written);
    TEST_ASSERT_EQUAL_UINT64 (test_data_size, bytes_read);

    //  Verify received data
    const auto &data = recv_buffer.data ();
    TEST_ASSERT_EQUAL_MEMORY (test_data, data.data (), test_data_size);

    //  Verify binary mode
    TEST_ASSERT_TRUE (server_ws.got_binary ());

    //  Note: Don't call sync close - it blocks when io_context is stopped
    //  Let the streams destruct naturally
}

//  ============================================================================
//  ZLINK WebSocket Integration Tests
//  ============================================================================
//
//  The following tests verify that ZLINK API works with WebSocket transport:
//  - zlink_bind("ws://...") should work without errno 93 (Protocol not supported)
//  - zlink_connect("ws://...") should work and establish connection
//  - Message exchange should work over WebSocket transport

#if defined ZLINK_HAVE_WS

//  Global ZLINK context for WebSocket integration tests
static void *g_ctx = NULL;

static void setup_zlink_ctx ()
{
    g_ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (g_ctx);
}

static void teardown_zlink_ctx ()
{
    if (g_ctx) {
        int rc = zlink_ctx_term (g_ctx);
        TEST_ASSERT_EQUAL_INT (0, rc);
        g_ctx = NULL;
    }
}

namespace
{
std::mutex g_ws_stream_probe_mutex;
std::atomic<bool> g_ws_stream_probe_ready (false);
zlink_routing_id_t g_ws_stream_probe_rid;

void ws_stream_packet_handler (void *,
                               const zlink_routing_id_t *rid_,
                               zlink_msg_t *header_,
                               zlink_msg_t *body_,
                               void *)
{
    if (rid_) {
        std::lock_guard<std::mutex> lock (g_ws_stream_probe_mutex);
        g_ws_stream_probe_rid = *rid_;
        g_ws_stream_probe_ready.store (true, std::memory_order_release);
    }
    if (header_)
        zlink_msg_close (header_);
    if (body_)
        zlink_msg_close (body_);
}

std::vector<unsigned char> ws_stream_packet (const char *header_, const char *body_)
{
    const size_t header_size = strlen (header_);
    const size_t body_size = strlen (body_);
    std::vector<unsigned char> frame (6 + header_size + body_size);
    frame[0] = static_cast<unsigned char> (header_size >> 8);
    frame[1] = static_cast<unsigned char> (header_size);
    frame[2] = static_cast<unsigned char> (body_size >> 24);
    frame[3] = static_cast<unsigned char> (body_size >> 16);
    frame[4] = static_cast<unsigned char> (body_size >> 8);
    frame[5] = static_cast<unsigned char> (body_size);
    memcpy (&frame[6], header_, header_size);
    memcpy (&frame[6 + header_size], body_, body_size);
    return frame;
}
}

//  Test 6: ZLINK WebSocket bind should not return EPROTONOSUPPORT
void test_zlink_ws_bind ()
{
    setup_zlink_ctx ();

    void *socket = zlink_socket (g_ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);

    //  Bind to WebSocket endpoint - this should NOT return -1 with errno 93
    int rc = zlink_bind (socket, "ws://127.0.0.1:*");
    if (rc == -1) {
        int err = zlink_errno ();
        char msg[256];
        snprintf (msg, sizeof (msg), "zlink_bind(ws://) failed with errno %d: %s", err,
                  zlink_strerror (err));
        TEST_FAIL_MESSAGE (msg);
    }
    TEST_ASSERT_EQUAL_INT (0, rc);

    //  Get the actual bound endpoint
    char endpoint[256];
    size_t endpoint_len = sizeof (endpoint);
    rc = zlink_get_option (socket, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_len);
    TEST_ASSERT_EQUAL_INT (0, rc);

    //  Verify it's a ws:// endpoint
    TEST_ASSERT_TRUE (strncmp (endpoint, "ws://", 5) == 0);

    zlink_close (socket);
    teardown_zlink_ctx ();
}

//  Test 7: ZLINK WebSocket connect should not return EPROTONOSUPPORT
void test_zlink_ws_connect ()
{
    setup_zlink_ctx ();

    //  First create a bind socket
    void *bind_socket = zlink_socket (g_ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (bind_socket);

    int rc = zlink_bind (bind_socket, "ws://127.0.0.1:*");
    TEST_ASSERT_EQUAL_INT (0, rc);

    //  Get the actual bound endpoint
    char endpoint[256];
    size_t endpoint_len = sizeof (endpoint);
    rc = zlink_get_option (bind_socket, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_len);
    TEST_ASSERT_EQUAL_INT (0, rc);

    //  Create connect socket
    void *connect_socket = zlink_socket (g_ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (connect_socket);

    //  Connect to the WebSocket endpoint
    rc = zlink_connect (connect_socket, endpoint);
    if (rc == -1) {
        int err = zlink_errno ();
        char msg[256];
        snprintf (msg, sizeof (msg), "zlink_connect(ws://) failed with errno %d: %s", err,
                  zlink_strerror (err));
        TEST_FAIL_MESSAGE (msg);
    }
    TEST_ASSERT_EQUAL_INT (0, rc);

    zlink_close (connect_socket);
    zlink_close (bind_socket);
    teardown_zlink_ctx ();
}

//  Test 8: ZLINK WebSocket PAIR message exchange
void test_zlink_ws_pair_message ()
{
    setup_zlink_ctx ();

    //  Create bind socket
    void *bind_socket = zlink_socket (g_ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (bind_socket);

    int rc = zlink_bind (bind_socket, "ws://127.0.0.1:*");
    TEST_ASSERT_EQUAL_INT (0, rc);

    //  Get the actual bound endpoint
    char endpoint[256];
    size_t endpoint_len = sizeof (endpoint);
    rc = zlink_get_option (bind_socket, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_len);
    TEST_ASSERT_EQUAL_INT (0, rc);

    //  Create connect socket
    void *connect_socket = zlink_socket (g_ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (connect_socket);

    rc = zlink_connect (connect_socket, endpoint);
    TEST_ASSERT_EQUAL_INT (0, rc);

    //  Give time for connection to establish (including WS handshake)
    msleep (200);

    //  Send a message from connect to bind
    const char *test_msg = "Hello WebSocket";
    rc = zlink_send (connect_socket, test_msg, strlen (test_msg), 0);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (strlen (test_msg)), rc);

    //  Receive the message
    char recv_buf[256];
    rc = zlink_recv (bind_socket, recv_buf, sizeof (recv_buf), 0);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (strlen (test_msg)), rc);
    recv_buf[rc] = '\0';
    TEST_ASSERT_EQUAL_STRING (test_msg, recv_buf);

    //  Send a message back from bind to connect
    const char *reply_msg = "Hello from bind";
    rc = zlink_send (bind_socket, reply_msg, strlen (reply_msg), 0);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (strlen (reply_msg)), rc);

    //  Receive the reply
    rc = zlink_recv (connect_socket, recv_buf, sizeof (recv_buf), 0);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (strlen (reply_msg)), rc);
    recv_buf[rc] = '\0';
    TEST_ASSERT_EQUAL_STRING (reply_msg, recv_buf);

    zlink_close (connect_socket);
    zlink_close (bind_socket);
    teardown_zlink_ctx ();
}

//  A STREAM socket with packet dispatch remains writable by routing id from
//  socket threads. This is the path used when bound-session ingress
//  forwards a complete framework frame to a WebSocket client.
void test_zlink_ws_stream_packet_handler_routed_send ()
{
    setup_zlink_ctx ();
    g_ws_stream_probe_ready.store (false, std::memory_order_release);
    memset (&g_ws_stream_probe_rid, 0, sizeof (g_ws_stream_probe_rid));

    void *server = zlink_socket (g_ctx, ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "ws://127.0.0.1:*"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_stream_packet_handler (server, &ws_stream_packet_handler, NULL));

    char endpoint[256];
    size_t endpoint_size = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_size));
    unsigned int port = 0;
    TEST_ASSERT_EQUAL_INT (
      1, sscanf (endpoint, "ws://127.0.0.1:%u", &port));

    net::io_context client_io;
    tcp::resolver resolver (client_io);
    websocket::stream<tcp::socket> client (client_io);
    const std::string port_text = std::to_string (port);
    net::connect (client.next_layer (), resolver.resolve ("127.0.0.1", port_text));
    client.binary (true);
    client.handshake ("127.0.0.1:" + port_text, "/");

    const std::vector<unsigned char> request = ws_stream_packet ("request", "body");
    client.write (net::buffer (request));
    for (int waited = 0;
         waited < 5000 && !g_ws_stream_probe_ready.load (std::memory_order_acquire);
         waited += 10)
        msleep (10);
    TEST_ASSERT_TRUE (g_ws_stream_probe_ready.load (std::memory_order_acquire));

    zlink_routing_id_t rid;
    {
        std::lock_guard<std::mutex> lock (g_ws_stream_probe_mutex);
        rid = g_ws_stream_probe_rid;
    }
    const std::string first_body (192, 'n');
    const std::string second_body (192, 'r');
    const std::vector<unsigned char> first = ws_stream_packet ("notify", first_body.c_str ());
    const std::vector<unsigned char> second =
      ws_stream_packet ("response", second_body.c_str ());
    const std::vector<unsigned char> *messages[] = {&first, &second};
    for (size_t i = 0; i < 2; ++i) {
        std::atomic<int> send_result (ZLINK_SUBMIT_INTERNAL_ERROR);
        std::thread service_thread ([&, i] () {
            zlink_msg_t response_message;
            if (zlink_msg_init_size (&response_message, messages[i]->size ()) != 0)
                return;
            memcpy (zlink_msg_data (&response_message), &(*messages[i])[0],
                    messages[i]->size ());
            send_result.store (
              zlink_send_part_rid (
                server, &rid, &response_message, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL),
              std::memory_order_release);
        });
        service_thread.join ();
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK, send_result.load (std::memory_order_acquire));
    }

    std::string received_bytes;
    const size_t expected_size = first.size () + second.size ();
    while (received_bytes.size () < expected_size) {
        beast::flat_buffer received;
        client.read (received);
        received_bytes += beast::buffers_to_string (received.data ());
    }
    TEST_ASSERT_EQUAL_UINT64 (expected_size, received_bytes.size ());
    TEST_ASSERT_EQUAL_MEMORY (&first[0], received_bytes.data (), first.size ());
    TEST_ASSERT_EQUAL_MEMORY (&second[0], received_bytes.data () + first.size (),
                              second.size ());

    boost::system::error_code ignored;
    client.close (websocket::close_code::normal, ignored);
    zlink_close (server);
    teardown_zlink_ctx ();
}

//  Test 9: ZLINK WebSocket PUB/SUB pattern
void test_zlink_ws_pubsub ()
{
    setup_zlink_ctx ();

    //  Create PUB socket
    void *pub_socket = zlink_socket (g_ctx, ZLINK_SOCKET_PUB);
    TEST_ASSERT_NOT_NULL (pub_socket);

    int rc = zlink_bind (pub_socket, "ws://127.0.0.1:*");
    TEST_ASSERT_EQUAL_INT (0, rc);

    //  Get the actual bound endpoint
    char endpoint[256];
    size_t endpoint_len = sizeof (endpoint);
    rc = zlink_get_option (pub_socket, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_len);
    TEST_ASSERT_EQUAL_INT (0, rc);

    //  Create SUB socket
    void *sub_socket = zlink_socket (g_ctx, ZLINK_SOCKET_SUB);
    TEST_ASSERT_NOT_NULL (sub_socket);

    //  Subscribe to all messages
    rc = zlink_set_subscription (sub_socket, "");
    TEST_ASSERT_EQUAL_INT (0, rc);

    rc = zlink_connect (sub_socket, endpoint);
    TEST_ASSERT_EQUAL_INT (0, rc);

    //  Give time for connection and subscription to propagate
    msleep (300);

    //  Send a message from PUB
    const char *test_msg = "WebSocket PubSub Test";
    rc = zlink_send (pub_socket, test_msg, strlen (test_msg), 0);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (strlen (test_msg)), rc);

    //  Receive the message on SUB
    char recv_buf[256];
    rc = zlink_recv (sub_socket, recv_buf, sizeof (recv_buf), 0);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (strlen (test_msg)), rc);
    recv_buf[rc] = '\0';
    TEST_ASSERT_EQUAL_STRING (test_msg, recv_buf);

    zlink_close (sub_socket);
    zlink_close (pub_socket);
    teardown_zlink_ctx ();
}

//  Test 10: ZLINK WebSocket with path
void test_zlink_ws_with_path ()
{
    setup_zlink_ctx ();

    void *socket = zlink_socket (g_ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);

    //  Bind to WebSocket endpoint with custom path
    int rc = zlink_bind (socket, "ws://127.0.0.1:*/my/custom/path");
    TEST_ASSERT_EQUAL_INT (0, rc);

    //  Get the actual bound endpoint
    char endpoint[256];
    size_t endpoint_len = sizeof (endpoint);
    rc = zlink_get_option (socket, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_len);
    TEST_ASSERT_EQUAL_INT (0, rc);

    //  Verify the path is included in the endpoint
    TEST_ASSERT_NOT_NULL (strstr (endpoint, "/my/custom/path"));

    zlink_close (socket);
    teardown_zlink_ctx ();
}

#if defined ZLINK_HAVE_WSS
void test_zlink_wss_pair_message ()
{
    setup_zlink_ctx ();

    const tls_test_files_t files = make_tls_test_files ();

    void *server = zlink_socket (g_ctx, ZLINK_SOCKET_PAIR);
    void *client = zlink_socket (g_ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    const int trust_system = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system, sizeof (trust_system)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server, ZLINK_OPT_TLS_CERT, files.server_cert.c_str (), files.server_cert.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      server, ZLINK_OPT_TLS_KEY, files.server_key.c_str (), files.server_key.size ()));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_CA, files.ca_cert.c_str (), files.ca_cert.size ()));

    const char hostname[] = "localhost";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_TLS_HOSTNAME, hostname, strlen (hostname)));

    //  Bind server to WSS endpoint
    int rc = zlink_bind (server, "wss://127.0.0.1:*");
    TEST_ASSERT_SUCCESS_ERRNO (rc);

    char endpoint[256];
    size_t endpoint_len = sizeof (endpoint);
    rc = zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_len);
    TEST_ASSERT_SUCCESS_ERRNO (rc);

    //  Connect client
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    send_string_expect_success (client, "wss-hello", 0);
    recv_string_expect_success (server, "wss-hello", 0);

    zlink_close (client);
    zlink_close (server);
    cleanup_tls_test_files (files);
    teardown_zlink_ctx ();
}
#endif // ZLINK_HAVE_WSS

#endif // ZLINK_HAVE_WS

#else // !ZLINK_IOTHREAD_POLLER_USE_ASIO || !ZLINK_HAVE_ASIO_WS

void setUp ()
{
}

void tearDown ()
{
}

void test_asio_ws_not_enabled ()
{
    //  Skip tests when Asio WS is not enabled
    TEST_IGNORE_MESSAGE ("Asio WebSocket not enabled, skipping tests");
}

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_WS

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS
    //  Beast WebSocket infrastructure tests
    RUN_TEST (test_ws_stream_creation);
    RUN_TEST (test_ws_stream_options);
    //  Network-based Beast tests
    RUN_TEST (test_tcp_connection_baseline);
    RUN_TEST (test_ws_handshake);
    RUN_TEST (test_ws_binary_message);

#if defined ZLINK_HAVE_WS
    //  ZLINK WebSocket API integration tests
    RUN_TEST (test_zlink_ws_bind);
    RUN_TEST (test_zlink_ws_connect);
    RUN_TEST (test_zlink_ws_pair_message);
    RUN_TEST (test_zlink_ws_stream_packet_handler_routed_send);
    RUN_TEST (test_zlink_ws_pubsub);
    RUN_TEST (test_zlink_ws_with_path);
#if defined ZLINK_HAVE_WSS
    RUN_TEST (test_zlink_wss_pair_message);
#endif
#endif // ZLINK_HAVE_WS

#else
    RUN_TEST (test_asio_ws_not_enabled);
#endif

    return UNITY_END ();
}
