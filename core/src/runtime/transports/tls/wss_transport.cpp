/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "transports/tls/wss_transport.hpp"

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS                           \
  && defined ZLINK_HAVE_ASIO_SSL

#include "engine/asio/asio_debug.hpp"

#include <openssl/ssl.h>

//  Debug logging for WSS transport
#define ASIO_DBG_WSS(fmt, ...) ASIO_DBG_THIS ("WSS", fmt, ##__VA_ARGS__)

namespace zlink
{
#ifdef ZLINK_BUILD_TESTS
size_t test_wss_write_buffer_bytes ()
{
    return ws_transport_common_internal::write_buffer_bytes ();
}

size_t test_wss_read_message_max ()
{
    return ws_transport_common_internal::read_message_max ();
}
#endif

wss_transport_t::wss_transport_t (boost::asio::ssl::context &ssl_ctx,
                                  const std::string &path,
                                  const std::string &host) :
    _ssl_ctx (ssl_ctx),
    _path (path),
    _host (host)
{
}

wss_transport_t::~wss_transport_t ()
{
    close ();
}

bool wss_transport_t::open (boost::asio::io_context &io_context, fd_t fd)
{
    //  Close any existing stream
    close ();

    //  Create the underlying TCP socket
    ws_transport_common_internal::socket_t socket (io_context);
    boost::system::error_code ec;

    //  Assign the file descriptor to the socket
    socket.assign (ws_transport_common_internal::protocol_for_fd (fd), fd, ec);
    if (ec) {
        ASIO_GLOBAL_ERROR ("wss_transport assign failed: %s", ec.message ().c_str ());
        return false;
    }

    //  Keep synchronous read_some/write paths non-blocking.
    socket.native_non_blocking (true, ec);
    if (ec) {
        ASIO_GLOBAL_ERROR ("wss_transport non-blocking failed: %s", ec.message ().c_str ());
        return false;
    }

    //  Create SSL stream wrapping the socket
    ssl_stream_t ssl_stream (std::move (socket), _ssl_ctx);

    //  Create one owner for the TLS/WebSocket stream and its authoritative
    //  handshake and read-boundary state.
    try {
        _connection = std::make_shared<connection_generation_t> (
          std::move (ssl_stream));
    }
    catch (const std::bad_alloc &) {
        ASIO_GLOBAL_ERROR ("wss_transport stream allocation failed");
        return false;
    }

    ws_transport_common_internal::configure_stream (_connection.get ());

    ASIO_DBG_WSS ("opened with path=%s, host=%s", _path.c_str (), _host.c_str ());
    return true;
}

bool wss_transport_t::is_open () const
{
    return _connection
           && _connection->stream.next_layer ().next_layer ().is_open ();
}

void wss_transport_t::close ()
{
    std::shared_ptr<connection_generation_t> connection =
      std::move (_connection);

    if (connection) {
        boost::system::error_code ec;
        connection->stream.next_layer ().next_layer ().cancel (ec);

        //  Avoid blocking WebSocket/SSL shutdown; just close the TCP layer.
        connection->stream.next_layer ().next_layer ().shutdown (
          boost::asio::ip::tcp::socket::shutdown_both, ec);
        connection->stream.next_layer ().next_layer ().close (ec);
        connection->read_message_state.reset ();
    }
}

void wss_transport_t::async_read_some (unsigned char *buffer,
                                       std::size_t buffer_size,
                                       completion_handler_t handler)
{
    const std::shared_ptr<connection_generation_t> connection = _connection;
    ws_transport_common_internal::async_read_some (
      connection, connection && connection->ws_handshake_complete, buffer, buffer_size,
      std::move (handler), "WSS");
}

std::size_t wss_transport_t::read_some (std::uint8_t *buffer, std::size_t len)
{
    connection_generation_t *const connection = _connection.get ();
    return ws_transport_common_internal::read_some (
      connection,
      connection && connection->ssl_handshake_complete
        && connection->ws_handshake_complete,
      &boost::asio::error::get_ssl_category (), buffer, len);
}

void wss_transport_t::async_write_some (const unsigned char *buffer,
                                        std::size_t buffer_size,
                                        completion_handler_t handler)
{
    const std::shared_ptr<connection_generation_t> connection = _connection;
    ws_transport_common_internal::async_write_some (
      connection, connection && connection->ws_handshake_complete, buffer, buffer_size,
      std::move (handler), "WSS");
}

void wss_transport_t::async_writev (const unsigned char *header,
                                    std::size_t header_size,
                                    const unsigned char *body,
                                    std::size_t body_size,
                                    completion_handler_t handler)
{
    const std::shared_ptr<connection_generation_t> connection = _connection;
    ws_transport_common_internal::async_writev (
      connection, connection && connection->ws_handshake_complete, header, header_size, body,
      body_size, std::move (handler), "WSS");
}

std::size_t wss_transport_t::write_some (const std::uint8_t *data, std::size_t len)
{
    connection_generation_t *const connection = _connection.get ();
    return ws_transport_common_internal::write_some (
      connection, connection && connection->ws_handshake_complete,
      connection && connection->stream.next_layer ().next_layer ().is_open (),
      &boost::asio::error::get_ssl_category (), data, len, "WSS");
}

void wss_transport_t::async_handshake (int handshake_type, completion_handler_t handler)
{
    std::shared_ptr<connection_generation_t> connection = _connection;
    if (!connection) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    connection->handshake_type = handshake_type;

    ASIO_DBG_WSS ("starting SSL handshake, type=%s", handshake_type == 0 ? "client" : "server");

    //  First do SSL handshake
    auto ssl_hs_type = (handshake_type == client) ? boost::asio::ssl::stream_base::client
                                                  : boost::asio::ssl::stream_base::server;

    if (handshake_type == client && !_tls_hostname.empty ()) {
        if (!SSL_set_tlsext_host_name (
              connection->stream.next_layer ().native_handle (),
              _tls_hostname.c_str ())) {
            if (handler) {
                handler (boost::asio::error::invalid_argument, 0);
            }
            return;
        }
    }

    const std::string host = _host;
    const std::string path = _path;
    connection_generation_t *const generation = connection.get ();
    generation->stream.next_layer ().async_handshake (
      ssl_hs_type,
      [connection = std::move (connection), host, path,
       handler = std::move (handler)] (
        const boost::system::error_code &ec) mutable {
          if (ec) {
              ASIO_DBG ("WSS", "SSL handshake failed: %s",
                        ec.message ().c_str ());
              if (handler) {
                  handler (ec, 0);
              }
              return;
          }

          connection->ssl_handshake_complete = true;
          ASIO_DBG ("WSS", "SSL handshake complete, continuing with WebSocket");

          //  Now do WebSocket handshake
          wss_transport_t::continue_ws_handshake (
            std::move (connection), host, path, std::move (handler));
      });
}

void wss_transport_t::continue_ws_handshake (
  std::shared_ptr<connection_generation_t> connection,
  const std::string &host,
  const std::string &path,
  completion_handler_t handler)
{
    if (!connection) {
        if (handler)
            handler (boost::asio::error::not_connected, 0);
        return;
    }

    if (connection->handshake_type == client) {
        //  Client-side WebSocket handshake
        connection_generation_t *const generation = connection.get ();
        generation->stream.async_handshake (
          host, path,
          [connection = std::move (connection), handler = std::move (handler)] (
            const boost::system::error_code &ec) {
              if (!ec) {
                  connection->ws_handshake_complete = true;
                  ASIO_DBG ("WSS", "WebSocket client handshake complete");
              } else {
                  ASIO_DBG ("WSS", "WebSocket client handshake failed: %s",
                            ec.message ().c_str ());
              }
              if (handler) {
                  handler (ec, 0);
              }
          });
    } else {
        //  Server-side WebSocket handshake
        connection_generation_t *const generation = connection.get ();
        generation->stream.async_accept (
          [connection = std::move (connection), handler = std::move (handler)] (
            const boost::system::error_code &ec) {
            if (!ec) {
                connection->ws_handshake_complete = true;
                ASIO_DBG ("WSS", "WebSocket server handshake complete");
            } else {
                ASIO_DBG ("WSS", "WebSocket server handshake failed: %s",
                          ec.message ().c_str ());
            }
            if (handler) {
                handler (ec, 0);
            }
        });
    }
}

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_WS && ZLINK_HAVE_ASIO_SSL
