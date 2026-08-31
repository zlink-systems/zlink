/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_WSS_TRANSPORT_HPP_INCLUDED__
#define __ZLINK_WSS_TRANSPORT_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS                           \
  && defined ZLINK_HAVE_ASIO_SSL

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <array>
#include <memory>
#include <string>
#include <utility>

#include "engine/asio/i_asio_transport.hpp"

namespace zlink
{

#ifdef ZLINK_BUILD_TESTS
size_t test_wss_write_buffer_bytes ();
size_t test_wss_read_message_max ();
#endif

//  Secure WebSocket (WSS) transport implementation using Boost.Beast
//
//  This transport wraps a TCP socket with SSL and then WebSocket framing.
//  It implements the i_asio_transport interface for use with asio_engine_t.
//
//  WSS specifics:
//  - First performs SSL/TLS handshake
//  - Then performs WebSocket HTTP upgrade handshake
//  - Uses binary frames for ZLINK messages
//  - Encrypted transport (is_encrypted() returns true)
//
//  Usage:
//    1. Create wss_transport_t with SSL context and optional path/host
//    2. Call open() to wrap an existing TCP socket
//    3. Call async_handshake() to complete SSL + WebSocket handshake
//    4. Use async_read_some/async_write_some for encrypted WebSocket I/O

class wss_transport_t : public i_asio_transport
{
  public:
    //  Handshake types
    enum handshake_type
    {
        client = 0,
        server = 1
    };

    //  Create Secure WebSocket transport
    //  ssl_ctx: SSL context with certificate/key configuration
    //  path: URL path for WebSocket endpoint (e.g., "/zlink")
    //  host: Host name for client handshake (e.g., "127.0.0.1:9443")
    wss_transport_t (boost::asio::ssl::context &ssl_ctx,
                     const std::string &path = "/",
                     const std::string &host = "localhost");
    ~wss_transport_t () ZLINK_OVERRIDE;

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

    //  WSS-specific overrides
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
    bool is_encrypted () const ZLINK_OVERRIDE { return true; }
    const char *name () const ZLINK_OVERRIDE { return "wss"; }

    void set_tls_hostname (const std::string &hostname) { _tls_hostname = hostname; }

    //  Set the host for client handshake
    void set_host (const std::string &host) { _host = host; }

    //  Set the path for WebSocket endpoint
    void set_path (const std::string &path) { _path = path; }

  private:
    //  SSL stream type
    typedef boost::asio::ssl::stream<boost::asio::ip::tcp::socket> ssl_stream_t;

    //  WebSocket stream over SSL
    typedef boost::beast::websocket::stream<ssl_stream_t> wss_stream_t;

    struct connection_generation_t
    {
        explicit connection_generation_t (ssl_stream_t &&stream_) :
            stream (std::move (stream_)),
            ssl_handshake_complete (false),
            ws_handshake_complete (false),
            handshake_type (client)
        {
        }

        wss_stream_t stream;
        asio_transport_read_opcode_state_t read_message_state;
        bool ssl_handshake_complete;
        bool ws_handshake_complete;
        int handshake_type;
    };

    boost::asio::ssl::context &_ssl_ctx;
    std::string _path;
    std::string _host;
    //  TLS, Beast, handshake, and boundary state share one connection
    //  generation. Async callbacks retain this aggregate after close().
    std::shared_ptr<connection_generation_t> _connection;
    std::string _tls_hostname;

    //  Internal handshake continuation
    static void continue_ws_handshake (
      std::shared_ptr<connection_generation_t> connection_,
      const std::string &host_,
      const std::string &path_,
      completion_handler_t handler_);

    ZLINK_NON_COPYABLE_NOR_MOVABLE (wss_transport_t)
};

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_WS && ZLINK_HAVE_ASIO_SSL

#endif // __ZLINK_WSS_TRANSPORT_HPP_INCLUDED__
