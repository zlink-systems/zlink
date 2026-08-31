/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_WS_TRANSPORT_HPP_INCLUDED__
#define __ZLINK_WS_TRANSPORT_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <array>
#include <memory>
#include <string>
#include <utility>

#include "engine/asio/i_asio_transport.hpp"

namespace zlink
{

#ifdef ZLINK_BUILD_TESTS
size_t test_ws_write_buffer_bytes ();
size_t test_ws_read_message_max ();
#endif

//  WebSocket transport implementation using Boost.Beast
//
//  This transport wraps a TCP socket with WebSocket framing.
//  It implements the i_asio_transport interface for use with asio_engine_t.
//
//  WebSocket specifics:
//  - Requires HTTP upgrade handshake before data transfer
//  - Uses binary frames for ZLINK messages (not text frames)
//  - Frame-based protocol (read_some returns message data chunks)
//
//  Usage:
//    1. Create ws_transport_t with optional path/protocol
//    2. Call open() to wrap an existing TCP socket
//    3. Call async_handshake() to complete WebSocket handshake
//    4. Use async_read_some/async_write_some for WebSocket I/O

class ws_transport_t : public i_asio_transport
{
  public:
    //  Handshake types
    enum handshake_type
    {
        client = 0,
        server = 1
    };

    //  Create WebSocket transport
    //  path: URL path for WebSocket endpoint (e.g., "/zlink")
    //  host: Host name for client handshake (e.g., "127.0.0.1:9000")
    ws_transport_t (const std::string &path = "/", const std::string &host = "localhost");
    ~ws_transport_t () ZLINK_OVERRIDE;

    //  i_asio_transport interface
    bool open (boost::asio::io_context &io_context, fd_t fd) ZLINK_OVERRIDE;
    bool is_open () const ZLINK_OVERRIDE;
    void close () ZLINK_OVERRIDE;

    void async_read_some (unsigned char *buffer,
                          std::size_t buffer_size,
                          completion_handler_t handler) ZLINK_OVERRIDE;

    std::size_t read_some (std::uint8_t *buffer, std::size_t len) ZLINK_OVERRIDE;
    bool has_message_boundaries () const ZLINK_OVERRIDE { return true; }
    bool read_message_binary () const ZLINK_OVERRIDE
    {
        return !_connection
               || _connection->read_message_state.is_binary ();
    }

    void async_write_some (const unsigned char *buffer,
                           std::size_t buffer_size,
                           completion_handler_t handler) ZLINK_OVERRIDE;

    std::size_t write_some (const std::uint8_t *data, std::size_t len) ZLINK_OVERRIDE;

    //  WebSocket-specific overrides
    bool requires_handshake () const ZLINK_OVERRIDE { return true; }
    void async_handshake (int handshake_type, completion_handler_t handler) ZLINK_OVERRIDE;
    bool supports_speculative_write () const ZLINK_OVERRIDE { return false; }
    // STREAM 64 KiB echo relies on this capability to avoid copying the body
    // through the encoder batch buffer. Keep it aligned with async_writev().
    bool supports_gather_write () const ZLINK_OVERRIDE { return true; }
    void async_writev (const unsigned char *header,
                       std::size_t header_size,
                       const unsigned char *body,
                       std::size_t body_size,
                       completion_handler_t handler) ZLINK_OVERRIDE;
    bool is_encrypted () const ZLINK_OVERRIDE { return false; }
    const char *name () const ZLINK_OVERRIDE { return "ws"; }

    //  Set the host for client handshake
    void set_host (const std::string &host) { _host = host; }

    //  Set the path for WebSocket endpoint
    void set_path (const std::string &path) { _path = path; }

  private:
    //  WebSocket stream type (over TCP socket, no compression for simplicity)
    typedef boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws_stream_t;

    struct connection_generation_t
    {
        explicit connection_generation_t (
          boost::asio::ip::tcp::socket &&socket_) :
            stream (std::move (socket_)), handshake_complete (false)
        {
        }

        ws_stream_t stream;
        asio_transport_read_opcode_state_t read_message_state;
        bool handshake_complete;
    };

    std::string _path;
    std::string _host;
    //  The Beast stream and boundary snapshot are one connection generation.
    //  Async callbacks retain this aggregate after close() without a second
    //  allocation or reference-count operation.
    std::shared_ptr<connection_generation_t> _connection;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (ws_transport_t)
};

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_WS

#endif // __ZLINK_WS_TRANSPORT_HPP_INCLUDED__
