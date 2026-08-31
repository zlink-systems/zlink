/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "transports/tls/wss_transport.hpp"

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS                           \
  && defined ZLINK_HAVE_ASIO_SSL

#include "engine/asio/asio_debug.hpp"
#include "core/address.hpp"
#include "utils/env.hpp"

#include <openssl/ssl.h>
#include <cerrno>

//  Debug logging for WSS transport
#define ASIO_DBG_WSS(fmt, ...) ASIO_DBG_THIS ("WSS", fmt, ##__VA_ARGS__)

namespace zlink
{
namespace
{
boost::asio::ip::tcp protocol_for_fd (fd_t fd_)
{
    sockaddr_storage ss;
    const zlink_socklen_t sl = get_socket_address (fd_, socket_end_local, &ss);
    if (sl != 0 && ss.ss_family == AF_INET6)
        return boost::asio::ip::tcp::v6 ();
    return boost::asio::ip::tcp::v4 ();
}

size_t wss_write_buffer_bytes ()
{
    static const size_t value =
      env::positive_size ("ZLINK_WS_WRITE_BUFFER_BYTES", 64 * 1024);
    return value;
}

size_t wss_read_message_max ()
{
    static const size_t value =
      env::positive_size ("ZLINK_WS_READ_MESSAGE_MAX", 64 * 1024 * 1024);
    return value;
}
}

#ifdef ZLINK_BUILD_TESTS
size_t test_wss_write_buffer_bytes ()
{
    return wss_write_buffer_bytes ();
}

size_t test_wss_read_message_max ()
{
    return wss_read_message_max ();
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
    boost::asio::ip::tcp::socket socket (io_context);
    boost::system::error_code ec;

    //  Assign the file descriptor to the socket
    socket.assign (protocol_for_fd (fd), fd, ec);
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

    //  Configure WebSocket options
    //  Use binary mode for ZLINK messages
    _connection->stream.binary (true);
    _connection->stream.auto_fragment (false);
    _connection->stream.write_buffer_bytes (wss_write_buffer_bytes ());
    _connection->stream.read_message_max (wss_read_message_max ());

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
    std::shared_ptr<connection_generation_t> connection = _connection;
    if (!connection || !connection->ws_handshake_complete) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    connection->read_message_state.reset ();
    if (buffer_size == 0) {
        if (handler) {
            boost::asio::post (connection->stream.get_executor (),
                               [handler = std::move (handler)] () {
                                   handler (boost::system::error_code (), 0);
                               });
        }
        return;
    }

    //  The engine owns this writable input buffer (normally the decoder buffer)
    //  until completion. Beast can therefore place WebSocket payload directly
    //  into it while retaining fragmented-message state inside the stream.
    connection_generation_t *const generation = connection.get ();
    generation->stream.async_read_some (
      boost::asio::buffer (buffer, buffer_size),
      [connection = std::move (connection), handler = std::move (handler)] (
        const boost::system::error_code &ec, std::size_t bytes_transferred) {
          connection->read_message_state.finish (
            !ec ? connection->stream.got_binary () : true);
          if (ec)
              ASIO_DBG ("WSS", "read failed: %s", ec.message ().c_str ());
          if (handler)
              handler (ec, bytes_transferred);
      });
}

std::size_t wss_transport_t::read_some (std::uint8_t *buffer, std::size_t len)
{
    connection_generation_t *const connection = _connection.get ();
    if (connection)
        connection->read_message_state.reset ();
    if (len == 0) {
        errno = 0;
        return 0;
    }

    if (!connection || !connection->ssl_handshake_complete
        || !connection->ws_handshake_complete) {
        errno = ENOTCONN;
        return 0;
    }

    boost::system::error_code ec;
    const std::size_t bytes_read =
      connection->stream.read_some (boost::asio::buffer (buffer, len), ec);

    if (ec) {
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
            errno = EAGAIN;
            return 0;
        }
        if (ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset
            || ec == boost::asio::error::broken_pipe
            || ec == boost::beast::websocket::error::closed) {
            errno = EPIPE;
            return 0;
        }
        if (ec.category () == boost::asio::error::get_ssl_category ()) {
            errno = EIO;
            return 0;
        }
        if (ec == boost::asio::error::not_connected) {
            errno = ENOTCONN;
        } else if (ec == boost::asio::error::bad_descriptor) {
            errno = EBADF;
        } else {
            errno = EIO;
        }
        return 0;
    }

    connection->read_message_state.finish (connection->stream.got_binary ());
    errno = 0;
    return bytes_read;
}

void wss_transport_t::async_write_some (const unsigned char *buffer,
                                        std::size_t buffer_size,
                                        completion_handler_t handler)
{
    std::shared_ptr<connection_generation_t> connection = _connection;
    if (!connection || !connection->ws_handshake_complete) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    //  WebSocket writes are frame-based
    connection_generation_t *const generation = connection.get ();
    generation->stream.async_write (
      boost::asio::buffer (buffer, buffer_size),
      [connection = std::move (connection), handler = std::move (handler)] (
        const boost::system::error_code &ec,
        std::size_t bytes_transferred) {
          ASIO_DBG ("WSS", "write complete: ec=%s, bytes=%zu", ec.message ().c_str (),
                    bytes_transferred);
          if (handler) {
              handler (ec, bytes_transferred);
          }
      });
}

void wss_transport_t::async_writev (const unsigned char *header,
                                    std::size_t header_size,
                                    const unsigned char *body,
                                    std::size_t body_size,
                                    completion_handler_t handler)
{
    std::shared_ptr<connection_generation_t> connection = _connection;
    if (!connection || !connection->ws_handshake_complete) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    std::array<boost::asio::const_buffer, 2> buffers = {boost::asio::buffer (header, header_size),
                                                        boost::asio::buffer (body, body_size)};

    connection_generation_t *const generation = connection.get ();
    generation->stream.async_write (
      buffers,
      [connection = std::move (connection), handler = std::move (handler)] (
        const boost::system::error_code &ec,
        std::size_t bytes_transferred) {
        ASIO_DBG ("WSS", "writev complete: ec=%s, bytes=%zu", ec.message ().c_str (),
                  bytes_transferred);
        if (handler) {
            handler (ec, bytes_transferred);
        }
      });
}

std::size_t wss_transport_t::write_some (const std::uint8_t *data, std::size_t len)
{
    if (len == 0) {
        return 0;
    }

    //  Check transport state - both SSL and WebSocket handshakes must be complete
    connection_generation_t *const connection = _connection.get ();
    if (!connection || !connection->ws_handshake_complete) {
        errno = ENOTCONN;
        return 0;
    }

    if (!connection->stream.next_layer ().next_layer ().is_open ()) {
        errno = EBADF;
        return 0;
    }

    //  WebSocket over SSL is frame-based protocol.
    //  We must write the complete frame atomically to maintain protocol integrity.
    //  Unlike TCP/TLS, partial writes are not meaningful for WebSocket.
    //
    //  Beast's write() sends a complete frame synchronously over the SSL layer.
    //  On would_block, we return 0 with errno = EAGAIN, and the caller
    //  should retry via async path.
    //
    //  Important: This blocks until the entire frame is sent or an error occurs.
    //  For speculative write optimization, this is acceptable because:
    //  1. Small messages (typical ZLINK use case) fit in socket buffer
    //  2. If buffer is full, we get would_block immediately
    //  3. Large messages should use async path anyway

    boost::system::error_code ec;
    std::size_t bytes_written = 0;

    //  Use write() to send complete frame (not write_some which is not
    //  available for WebSocket in Beast)
    bytes_written =
      connection->stream.write (boost::asio::buffer (data, len), ec);

    if (ec) {
        //  Handle would_block case
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
            errno = EAGAIN;
            return 0;
        }

        //  Handle SSL-specific errors
        if (ec.category () == boost::asio::error::get_ssl_category ()) {
            errno = EIO;
            ASIO_DBG_WSS ("write_some SSL error: %s", ec.message ().c_str ());
            return 0;
        }

        //  Map boost error codes to errno
        if (ec == boost::asio::error::broken_pipe || ec == boost::asio::error::connection_reset) {
            errno = EPIPE;
        } else if (ec == boost::asio::error::not_connected) {
            errno = ENOTCONN;
        } else if (ec == boost::asio::error::bad_descriptor) {
            errno = EBADF;
        } else if (ec == boost::asio::error::eof || ec == boost::beast::websocket::error::closed) {
            errno = ECONNRESET;
        } else {
            errno = EIO;
        }

        ASIO_DBG_WSS ("write_some error: %s", ec.message ().c_str ());
        return 0;
    }

    ASIO_DBG_WSS ("write_some: wrote %zu bytes as frame", bytes_written);
    errno = 0;
    return bytes_written;
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
