/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <array>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
namespace net = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
typedef net::ip::tcp tcp;
const int timeout_ms = 3000;

struct client_t
{
    client_t (net::io_context &io_, bool websocket_, unsigned short port_) :
        socket (io_), websocket (websocket_), failed (false)
    {
        socket.next_layer ().connect (
          tcp::endpoint (net::ip::make_address ("127.0.0.1"), port_));
        if (websocket) {
            socket.binary (true);
            socket.handshake ("127.0.0.1:" + std::to_string (port_), "/");
        }
    }

    void write (const std::string &bytes_)
    {
        if (websocket)
            socket.write (net::buffer (bytes_));
        else
            net::write (socket.next_layer (), net::buffer (bytes_));
    }

    void read ()
    {
        const auto completed = [this] (boost::system::error_code ec_, size_t n_) {
            if (ec_) {
                failed = true;
                return;
            }
            if (websocket) {
                received += beast::buffers_to_string (ws_buffer.data ());
                ws_buffer.consume (n_);
            } else
                received.append (tcp_buffer.data (), n_);
            if (received.size () < expected.size ())
                read ();
        };
        if (websocket)
            socket.async_read (ws_buffer, completed);
        else
            socket.next_layer ().async_read_some (net::buffer (tcp_buffer),
                                                  completed);
    }

    void close ()
    {
        boost::system::error_code ignored;
        socket.next_layer ().close (ignored);
    }

    ws::stream<tcp::socket> socket;
    bool websocket;
    bool failed;
    zlink_routing_id_t rid;
    uint64_t connection_id;
    beast::flat_buffer ws_buffer;
    std::array<char, 65536> tcp_buffer;
    std::string expected;
    std::string received;
};

zlink_monitor_event_t receive_edge (void *monitor_, uint64_t expected_)
{
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::milliseconds (timeout_ms);
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
          deadline - std::chrono::steady_clock::now ()).count ();
        if (remaining <= 0)
            return zlink_monitor_event_t ();
        zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
        const int poll_rc = zlink_poll (&item, 1, remaining, NULL);
        if (poll_rc == 0)
            return zlink_monitor_event_t ();
        TEST_ASSERT_EQUAL_INT (1, poll_rc);
        zlink_monitor_event_t event = {};
        const zlink_recv_result_t rc = zlink_socket_monitor_recv (
          monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA)
            continue;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
        if (event.event == ZLINK_EVENT_CONNECTION_READY
            && !(event.flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE))
            continue;
        TEST_ASSERT_EQUAL_UINT64 (expected_, event.event);
        TEST_ASSERT_EQUAL_INT (4, event.routing_id.size);
        TEST_ASSERT_NOT_EQUAL (0, event.connection_id);
        return event;
    }
}

std::string packet (unsigned char client_, unsigned char phase_,
                    unsigned char sequence_, size_t body_size_)
{
    std::string bytes (6, '\0');
    bytes[1] = 3;
    for (int i = 0; i != 4; ++i)
        bytes[2 + i] = static_cast<char> (body_size_ >> (24 - 8 * i));
    bytes += static_cast<char> (client_);
    bytes += static_cast<char> (phase_);
    bytes += static_cast<char> (sequence_);
    bytes.append (body_size_, static_cast<char> (client_ + sequence_));
    return bytes;
}

void push (void *server_, client_t &client_, const std::string &bytes_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (&part, bytes_.size ()));
    memcpy (zlink_msg_data (&part), bytes_.data (), bytes_.size ());
    zlink_completion_id_t id = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part_rid (server_, &client_.rid, &part,
                           ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, NULL, &id));
    TEST_ASSERT_EQUAL_UINT64 (0, id);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    client_.expected += bytes_;
}

void expect_delivery (net::io_context &io_,
                      std::array<std::unique_ptr<client_t>, 4> &clients_)
{
    io_.restart ();
    for (size_t i = 0; i != clients_.size (); ++i)
        if (clients_[i])
            clients_[i]->read ();
    io_.run_for (std::chrono::milliseconds (timeout_ms));
    for (size_t i = 0; i != clients_.size (); ++i) {
        if (!clients_[i])
            continue;
        TEST_ASSERT_FALSE (clients_[i]->failed);
        TEST_ASSERT_EQUAL_UINT64 (clients_[i]->expected.size (),
                                  clients_[i]->received.size ());
        TEST_ASSERT_EQUAL_MEMORY (clients_[i]->expected.data (),
                                  clients_[i]->received.data (),
                                  clients_[i]->expected.size ());
        clients_[i]->expected.clear ();
        clients_[i]->received.clear ();
    }
}

void run_delivery (bool websocket_, bool local_close_)
{
    if (websocket_ && !zlink_has ("ws"))
        TEST_IGNORE_MESSAGE ("WebSocket transport unavailable");
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    const zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
    const uint64_t hwm = 4u * 1024u * 1024u;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_stream_option (
      server, ZLINK_STREAM_OPT_RECV_MODE, &mode, sizeof (mode)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_option (
      server, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_option (
      server, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));
    zlink_socket_monitor_open_options_t options = {};
    options.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
    void *monitor = zlink_socket_monitor_open (server, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (
      server, websocket_ ? "ws://127.0.0.1:*" : "tcp://127.0.0.1:*"));
    char endpoint[256];
    size_t size = sizeof (endpoint);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_get_option (
      server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &size));
    unsigned int port = 0;
    TEST_ASSERT_EQUAL_INT (1, sscanf (strrchr (endpoint, ':') + 1, "%u", &port));

    net::io_context io;
    std::array<std::unique_ptr<client_t>, 4> clients;
    std::set<uint64_t> connections;
    std::set<std::string> rids;
    for (unsigned int phase = 0; phase != 3; ++phase) {
        for (size_t i = 0; i != clients.size (); ++i) {
            if (clients[i])
                continue;
            clients[i].reset (new client_t (io, websocket_, port));
            const zlink_monitor_event_t event = receive_edge (
              monitor, ZLINK_EVENT_CONNECTION_READY);
            clients[i]->rid = event.routing_id;
            clients[i]->connection_id = event.connection_id;
            TEST_ASSERT_TRUE (connections.insert (event.connection_id).second);
            TEST_ASSERT_TRUE (rids.insert (std::string (
              reinterpret_cast<const char *> (event.routing_id.data), 4)).second);
        }

        // No application recv/poll drives the server between these pushes.
        // Mix small records with the raw encoder's large-message gather path.
        for (unsigned int sequence = 0; sequence != 24; ++sequence) {
            const size_t body_size = sequence % 8 == 0 ? 131072 : 31 + sequence;
            for (size_t i = 0; i != clients.size (); ++i)
                push (server, *clients[i], packet (i, phase, sequence, body_size));
        }
        expect_delivery (io, clients);

        // The client identifies itself in the packet, independently of RID.
        for (size_t i = 0; i != clients.size (); ++i)
            clients[i]->write (packet (i, phase, 0, 1));
        std::set<unsigned char> received;
        for (size_t i = 0; i != clients.size (); ++i) {
            zlink_msg_t header, body;
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&header));
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&body));
            const zlink_routing_id_t *rid = NULL;
            TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_stream_recv_packet (
              server, &rid, &header, &body, ZLINK_RECV_FLAGS_NONE));
            TEST_ASSERT_EQUAL_UINT64 (3, zlink_msg_size (&header));
            const unsigned char index = *static_cast<unsigned char *> (
              zlink_msg_data (&header));
            TEST_ASSERT_LESS_THAN (clients.size (), index);
            TEST_ASSERT_TRUE (received.insert (index).second);
            TEST_ASSERT_EQUAL_MEMORY (clients[index]->rid.data, rid->data, 4);
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&header));
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&body));
        }

        const size_t retired = phase % clients.size ();
        if (local_close_)
            TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_disconnect_rid (
              server, &clients[retired]->rid));
        else
            clients[retired]->close ();
        const zlink_monitor_event_t disconnected = receive_edge (
          monitor, ZLINK_EVENT_DISCONNECTED);
        if (disconnected.event == 0) {
            // Release live peers before reporting a missing edge so the
            // diagnostic failure cannot turn into an unrelated ctx-term hang.
            for (size_t i = 0; i != clients.size (); ++i)
                if (clients[i])
                    clients[i]->close ();
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor));
            test_context_socket_close_zero_linger (server);
            TEST_FAIL_MESSAGE ("STREAM disconnect_rid did not publish DISCONNECTED");
        }
        TEST_ASSERT_EQUAL_UINT64 (clients[retired]->connection_id,
                                  disconnected.connection_id);
        TEST_ASSERT_EQUAL_MEMORY (clients[retired]->rid.data,
                                  disconnected.routing_id.data, 4);
        clients[retired]->close ();
        clients[retired].reset ();
        // A new accept or application recv must not be needed to flush a
        // surviving connection after another peer has terminated.
        for (size_t i = 0; i != clients.size (); ++i)
            if (clients[i])
                push (server, *clients[i], packet (i, phase, 24, 113));
        expect_delivery (io, clients);
    }
    for (size_t i = 0; i != clients.size (); ++i) {
        if (clients[i]) {
            clients[i]->close ();
            const zlink_monitor_event_t disconnected = receive_edge (
              monitor, ZLINK_EVENT_DISCONNECTED);
            TEST_ASSERT_EQUAL_UINT64 (clients[i]->connection_id,
                                      disconnected.connection_id);
        }
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (server);
}

void run_connector_reconnect ()
{
    net::io_context io;
    tcp::acceptor listener (io, tcp::endpoint (tcp::v4 (), 0));
    void *connector = test_context_socket (ZLINK_SOCKET_STREAM);
    const zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_stream_option (
      connector, ZLINK_STREAM_OPT_RECV_MODE, &mode, sizeof (mode)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_option (
      connector, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));
    zlink_socket_monitor_open_options_t options = {};
    options.events = ZLINK_EVENT_DISCONNECTED;
    void *monitor = zlink_socket_monitor_open (connector, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    const std::string endpoint = "tcp://127.0.0.1:"
                                 + std::to_string (listener.local_endpoint ().port ());
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (connector, endpoint.c_str ()));
    std::set<uint64_t> disconnected_connections;
    for (unsigned int cycle = 0; cycle != 3; ++cycle) {
        tcp::socket peer (io);
        bool accepted = false;
        listener.async_accept (peer, [&accepted] (boost::system::error_code ec_) {
            accepted = !ec_;
        });
        io.restart ();
        io.run_for (std::chrono::milliseconds (timeout_ms));
        TEST_ASSERT_TRUE (accepted);
        // Receive a packet through each new physical connection before closing
        // it. The connector's default IMMEDIATE=0 keeps its pipe on reconnect.
        net::write (peer, net::buffer (packet (0, cycle, 0, 1)));
        zlink_msg_t header, body;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&header));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&body));
        const zlink_routing_id_t *source_rid = NULL;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_stream_recv_packet (
          connector, &source_rid, &header, &body, ZLINK_RECV_FLAGS_NONE));
        const zlink_routing_id_t rid = *source_rid;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&header));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&body));
        peer.close ();
        const zlink_monitor_event_t event = receive_edge (
          monitor, ZLINK_EVENT_DISCONNECTED);
        if (event.event == 0) {
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor));
            test_context_socket_close_zero_linger (connector);
            TEST_FAIL_MESSAGE ("Reconnected STREAM transport lost DISCONNECTED");
        }
        TEST_ASSERT_EQUAL_MEMORY (rid.data, event.routing_id.data, 4);
        TEST_ASSERT_TRUE (disconnected_connections.insert (event.connection_id).second);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (connector);
}
}

void test_stream_tcp_peer_close_delivery () { run_delivery (false, false); }
void test_stream_tcp_disconnect_delivery () { run_delivery (false, true); }
void test_stream_ws_peer_close_delivery () { run_delivery (true, false); }
void test_stream_ws_disconnect_delivery () { run_delivery (true, true); }
void test_stream_tcp_connector_reconnect () { run_connector_reconnect (); }

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    const char *selected = getenv ("ZLINK_TEST_CASE");
#define RUN_SELECTED(name) \
    if (!selected || strcmp (selected, #name) == 0) RUN_TEST (name)
    RUN_SELECTED (test_stream_tcp_peer_close_delivery);
    RUN_SELECTED (test_stream_tcp_disconnect_delivery);
    RUN_SELECTED (test_stream_ws_peer_close_delivery);
    RUN_SELECTED (test_stream_ws_disconnect_delivery);
    RUN_SELECTED (test_stream_tcp_connector_reconnect);
#undef RUN_SELECTED
    return UNITY_END ();
}
