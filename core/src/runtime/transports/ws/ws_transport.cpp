/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "transports/ws/ws_transport.hpp"

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS

#include "engine/asio/asio_debug.hpp"

//  Debug logging for WebSocket transport
#define ASIO_DBG_WS(fmt, ...) ASIO_DBG_THIS ("WS", fmt, ##__VA_ARGS__)

namespace zlink
{
#ifdef ZLINK_BUILD_TESTS
size_t test_ws_write_buffer_bytes ()
{
    return ws_transport_common_internal::write_buffer_bytes ();
}

size_t test_ws_read_message_max ()
{
    return ws_transport_common_internal::read_message_max ();
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
    ws_transport_common_internal::socket_t socket (io_context);
    boost::system::error_code ec;

    //  Assign the file descriptor to the socket
    socket.assign (ws_transport_common_internal::protocol_for_fd (fd), fd, ec);
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

    ws_transport_common_internal::configure_stream (_connection.get ());

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
    const std::shared_ptr<connection_generation_t> connection = _connection;
    ws_transport_common_internal::async_read_some (
      connection, connection && connection->handshake_complete, buffer, buffer_size,
      std::move (handler), "WS");
}

std::size_t ws_transport_t::read_some (std::uint8_t *buffer, std::size_t len)
{
    connection_generation_t *const connection = _connection.get ();
    return ws_transport_common_internal::read_some (
      connection, connection && connection->handshake_complete, NULL, buffer, len);
}

void ws_transport_t::async_write_some (const unsigned char *buffer,
                                       std::size_t buffer_size,
                                       completion_handler_t handler)
{
    const std::shared_ptr<connection_generation_t> connection = _connection;
    ws_transport_common_internal::async_write_some (
      connection, connection && connection->handshake_complete, buffer, buffer_size,
      std::move (handler), "WS");
}

void ws_transport_t::async_writev (const unsigned char *header,
                                   std::size_t header_size,
                                   const unsigned char *body,
                                   std::size_t body_size,
                                   completion_handler_t handler)
{
    const std::shared_ptr<connection_generation_t> connection = _connection;
    ws_transport_common_internal::async_writev (
      connection, connection && connection->handshake_complete, header, header_size, body,
      body_size, std::move (handler), "WS");
}

std::size_t ws_transport_t::write_some (const std::uint8_t *data, std::size_t len)
{
    connection_generation_t *const connection = _connection.get ();
    return ws_transport_common_internal::write_some (
      connection, connection && connection->handshake_complete,
      connection && connection->stream.next_layer ().is_open (), NULL, data, len, "WS");
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
