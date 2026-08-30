/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "transports/ws/ws_transport.hpp"

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS

#include "engine/asio/asio_debug.hpp"
#include "core/address.hpp"
#include "utils/env.hpp"

#include <cerrno>

//  Debug logging for WebSocket transport
#define ASIO_DBG_WS(fmt, ...) ASIO_DBG_THIS ("WS", fmt, ##__VA_ARGS__)

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

size_t ws_write_buffer_bytes ()
{
    static const size_t value =
      env::positive_size ("ZLINK_WS_WRITE_BUFFER_BYTES", 64 * 1024);
    return value;
}

size_t ws_read_message_max ()
{
    static const size_t value =
      env::positive_size ("ZLINK_WS_READ_MESSAGE_MAX", 64 * 1024 * 1024);
    return value;
}
}

#ifdef ZLINK_BUILD_TESTS
size_t test_ws_write_buffer_bytes ()
{
    return ws_write_buffer_bytes ();
}

size_t test_ws_read_message_max ()
{
    return ws_read_message_max ();
}
#endif

ws_transport_t::ws_transport_t (const std::string &path, const std::string &host) :
    _path (path),
    _host (host)
{
}

ws_transport_t::~ws_transport_t ()
{
    close ();
}

bool ws_transport_t::open (boost::asio::io_context &io_context, fd_t fd)
{
    //  Close any existing stream
    close ();

    //  Create the underlying TCP socket
    boost::asio::ip::tcp::socket socket (io_context);
    boost::system::error_code ec;

    //  Assign the file descriptor to the socket
    socket.assign (protocol_for_fd (fd), fd, ec);
    if (ec) {
        ASIO_GLOBAL_ERROR ("ws_transport assign failed: %s", ec.message ().c_str ());
        return false;
    }

    //  Keep synchronous read_some/write paths non-blocking.
    socket.native_non_blocking (true, ec);
    if (ec) {
        ASIO_GLOBAL_ERROR ("ws_transport non-blocking failed: %s", ec.message ().c_str ());
        return false;
    }

    //  Create one owner for the stream and its authoritative read-boundary
    //  snapshot. Async callbacks retain this exact connection generation.
    try {
        _connection =
          std::make_shared<connection_generation_t> (std::move (socket));
    }
    catch (const std::bad_alloc &) {
        ASIO_GLOBAL_ERROR ("ws_transport stream allocation failed");
        return false;
    }

    //  Configure WebSocket options
    //  Use binary mode for ZLINK messages
    _connection->stream.binary (true);
    _connection->stream.auto_fragment (false);
    _connection->stream.write_buffer_bytes (ws_write_buffer_bytes ());
    _connection->stream.read_message_max (ws_read_message_max ());

    ASIO_DBG_WS ("opened with path=%s, host=%s", _path.c_str (), _host.c_str ());
    return true;
}

bool ws_transport_t::is_open () const
{
    return _connection && _connection->stream.next_layer ().is_open ();
}

void ws_transport_t::close ()
{
    std::shared_ptr<connection_generation_t> connection =
      std::move (_connection);

    if (connection) {
        boost::system::error_code ec;

        //  Close the underlying socket first - this cancels all pending async ops
        //  The socket close will cause pending async_read/async_write to complete
        //  with operation_aborted error
        if (connection->stream.next_layer ().is_open ()) {
            connection->stream.next_layer ().shutdown (
              boost::asio::ip::tcp::socket::shutdown_both, ec);
            connection->stream.next_layer ().close (ec);
        }
        connection->read_message_state.reset ();
    }
}

void ws_transport_t::async_read_some (unsigned char *buffer,
                                      std::size_t buffer_size,
                                      completion_handler_t handler)
{
    std::shared_ptr<connection_generation_t> connection = _connection;
    if (!connection || !connection->handshake_complete) {
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
            !ec && connection->stream.is_message_done (),
            !ec ? connection->stream.got_binary () : true);
          if (ec)
              ASIO_DBG ("WS", "read failed: %s", ec.message ().c_str ());
          if (handler)
              handler (ec, bytes_transferred);
      });
}

std::size_t ws_transport_t::read_some (std::uint8_t *buffer, std::size_t len)
{
    connection_generation_t *const connection = _connection.get ();
    if (connection)
        connection->read_message_state.reset ();
    if (len == 0) {
        errno = 0;
        return 0;
    }

    if (!connection || !connection->handshake_complete) {
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
        if (ec == boost::asio::error::not_connected) {
            errno = ENOTCONN;
        } else if (ec == boost::asio::error::bad_descriptor) {
            errno = EBADF;
        } else {
            errno = EIO;
        }
        return 0;
    }

    connection->read_message_state.finish (
      connection->stream.is_message_done (), connection->stream.got_binary ());
    errno = 0;
    return bytes_read;
}

void ws_transport_t::async_write_some (const unsigned char *buffer,
                                       std::size_t buffer_size,
                                       completion_handler_t handler)
{
    std::shared_ptr<connection_generation_t> connection = _connection;
    if (!connection || !connection->handshake_complete) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    //  WebSocket writes are frame-based, so we write the entire buffer
    //  as a single binary frame
    connection_generation_t *const generation = connection.get ();
    generation->stream.async_write (
      boost::asio::buffer (buffer, buffer_size),
      [connection = std::move (connection), handler = std::move (handler)] (
        const boost::system::error_code &ec,
        std::size_t bytes_transferred) {
          ASIO_DBG ("WS", "write complete: ec=%s, bytes=%zu", ec.message ().c_str (),
                    bytes_transferred);
          if (handler) {
              handler (ec, bytes_transferred);
          }
      });
}

void ws_transport_t::async_writev (const unsigned char *header,
                                   std::size_t header_size,
                                   const unsigned char *body,
                                   std::size_t body_size,
                                   completion_handler_t handler)
{
    std::shared_ptr<connection_generation_t> connection = _connection;
    if (!connection || !connection->handshake_complete) {
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
        ASIO_DBG ("WS", "writev complete: ec=%s, bytes=%zu", ec.message ().c_str (),
                  bytes_transferred);
        if (handler) {
            handler (ec, bytes_transferred);
        }
      });
}

std::size_t ws_transport_t::write_some (const std::uint8_t *data, std::size_t len)
{
    if (len == 0) {
        return 0;
    }

    //  Check transport state
    connection_generation_t *const connection = _connection.get ();
    if (!connection || !connection->handshake_complete) {
        errno = ENOTCONN;
        return 0;
    }

    if (!connection->stream.next_layer ().is_open ()) {
        errno = EBADF;
        return 0;
    }

    //  WebSocket is frame-based protocol.
    //  We must write the complete frame atomically to maintain protocol integrity.
    //  Unlike TCP/TLS, partial writes are not meaningful for WebSocket.
    //
    //  Beast's write() sends a complete frame synchronously.
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

        ASIO_DBG_WS ("write_some error: %s", ec.message ().c_str ());
        return 0;
    }

    ASIO_DBG_WS ("write_some: wrote %zu bytes as frame", bytes_written);
    errno = 0;
    return bytes_written;
}

void ws_transport_t::async_handshake (int handshake_type, completion_handler_t handler)
{
    std::shared_ptr<connection_generation_t> connection = _connection;
    if (!connection) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    ASIO_DBG_WS ("starting handshake, type=%s", handshake_type == 0 ? "client" : "server");

    if (handshake_type == client) {
        //  Client-side WebSocket handshake
        connection_generation_t *const generation = connection.get ();
        generation->stream.async_handshake (
          _host, _path,
          [connection = std::move (connection), handler = std::move (handler)] (
            const boost::system::error_code &ec) {
              if (!ec) {
                  connection->handshake_complete = true;
                  ASIO_DBG ("WS", "client handshake complete");
              } else {
                  ASIO_DBG ("WS", "client handshake failed: %s",
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
                connection->handshake_complete = true;
                ASIO_DBG ("WS", "server handshake complete");
            } else {
                ASIO_DBG ("WS", "server handshake failed: %s",
                          ec.message ().c_str ());
            }
            if (handler) {
                handler (ec, 0);
            }
        });
    }
}

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_WS
