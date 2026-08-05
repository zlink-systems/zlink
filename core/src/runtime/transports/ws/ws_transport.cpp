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
    static size_t value = 0;
    if (value == 0)
        value = env::positive_size ("ZLINK_WS_WRITE_BUFFER_BYTES", 64 * 1024);
    return value;
}

size_t ws_read_message_max ()
{
    static size_t value = 0;
    if (value == 0)
        value = env::positive_size ("ZLINK_WS_READ_MESSAGE_MAX", 64 * 1024 * 1024);
    return value;
}
}

ws_transport_t::ws_transport_t (const std::string &path, const std::string &host) :
    _path (path), _host (host), _handshake_complete (false), _read_state (new read_state_t)
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

    //  Create WebSocket stream wrapping the socket
    try {
        _ws_stream = std::shared_ptr<ws_stream_t> (new ws_stream_t (std::move (socket)));
    }
    catch (const std::bad_alloc &) {
        ASIO_GLOBAL_ERROR ("ws_transport stream allocation failed");
        return false;
    }

    //  Configure WebSocket options
    //  Use binary mode for ZLINK messages
    _ws_stream->binary (true);
    _ws_stream->auto_fragment (false);
    _ws_stream->write_buffer_bytes (ws_write_buffer_bytes ());
    _ws_stream->read_message_max (ws_read_message_max ());

    _handshake_complete = false;
    _read_state.reset (new read_state_t);

    ASIO_DBG_WS ("opened with path=%s, host=%s", _path.c_str (), _host.c_str ());
    return true;
}

bool ws_transport_t::is_open () const
{
    return _ws_stream && _ws_stream->next_layer ().is_open ();
}

void ws_transport_t::close ()
{
    std::shared_ptr<ws_stream_t> stream = _ws_stream;
    _ws_stream.reset ();

    if (stream) {
        boost::system::error_code ec;

        //  Close the underlying socket first - this cancels all pending async ops
        //  The socket close will cause pending async_read/async_write to complete
        //  with operation_aborted error
        if (stream->next_layer ().is_open ()) {
            stream->next_layer ().shutdown (boost::asio::ip::tcp::socket::shutdown_both, ec);
            stream->next_layer ().close (ec);
        }

        _handshake_complete = false;
        if (_read_state) {
            _read_state->closed = true;
            _read_state->clear ();
        }
    }
}

void ws_transport_t::async_read_some (unsigned char *buffer,
                                      std::size_t buffer_size,
                                      completion_handler_t handler)
{
    std::shared_ptr<ws_stream_t> stream = _ws_stream;
    std::shared_ptr<read_state_t> read_state = _read_state;
    if (!stream || !read_state || !_handshake_complete) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    if (buffer_size == 0) {
        if (handler) {
            boost::asio::post (stream->get_executor (),
                               [handler] () { handler (boost::system::error_code (), 0); });
        }
        return;
    }

    const std::size_t buffered_size = read_state->message_buffer.size ();
    if (buffered_size > 0) {
        const std::size_t deliver = std::min<std::size_t> (buffer_size, buffered_size);
        boost::asio::buffer_copy (boost::asio::buffer (buffer, deliver),
                                  read_state->message_buffer.data ());
        read_state->message_buffer.consume (deliver);
        if (handler) {
            boost::asio::post (stream->get_executor (), [handler, deliver] () {
                handler (boost::system::error_code (), deliver);
            });
        }
        return;
    }

    stream->async_read (read_state->message_buffer, [stream, read_state, buffer, buffer_size,
                                                     handler] (const boost::system::error_code &ec,
                                                               std::size_t) {
        if (read_state->closed) {
            if (handler) {
                handler (boost::asio::error::operation_aborted, 0);
            }
            return;
        }

        if (ec) {
            ASIO_DBG ("WS", "read failed: %s", ec.message ().c_str ());
            if (handler) {
                handler (ec, 0);
            }
            return;
        }

        const std::size_t available = boost::asio::buffer_size (read_state->message_buffer.data ());
        if (available == 0) {
            if (handler) {
                handler (boost::system::error_code (), 0);
            }
            return;
        }

        const std::size_t deliver = std::min<std::size_t> (buffer_size, available);
        boost::asio::buffer_copy (boost::asio::buffer (buffer, deliver),
                                  read_state->message_buffer.data ());
        read_state->message_buffer.consume (deliver);

        if (handler) {
            handler (boost::system::error_code (), deliver);
        }
    });
}

std::size_t ws_transport_t::read_some (std::uint8_t *buffer, std::size_t len)
{
    if (len == 0) {
        errno = 0;
        return 0;
    }

    if (!_ws_stream || !_handshake_complete) {
        errno = ENOTCONN;
        return 0;
    }

    boost::system::error_code ec;
    const std::size_t bytes_read = _ws_stream->read_some (boost::asio::buffer (buffer, len), ec);

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

    errno = 0;
    return bytes_read;
}

void ws_transport_t::async_write_some (const unsigned char *buffer,
                                       std::size_t buffer_size,
                                       completion_handler_t handler)
{
    std::shared_ptr<ws_stream_t> stream = _ws_stream;
    if (!stream || !_handshake_complete) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    //  WebSocket writes are frame-based, so we write the entire buffer
    //  as a single binary frame
    stream->async_write (
      boost::asio::buffer (buffer, buffer_size),
      [stream, handler] (const boost::system::error_code &ec, std::size_t bytes_transferred) {
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
    std::shared_ptr<ws_stream_t> stream = _ws_stream;
    if (!stream || !_handshake_complete) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    std::array<boost::asio::const_buffer, 2> buffers = {boost::asio::buffer (header, header_size),
                                                        boost::asio::buffer (body, body_size)};

    stream->async_write (buffers, [stream, handler] (const boost::system::error_code &ec,
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
    if (!_ws_stream || !_handshake_complete) {
        errno = ENOTCONN;
        return 0;
    }

    if (!_ws_stream->next_layer ().is_open ()) {
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
    bytes_written = _ws_stream->write (boost::asio::buffer (data, len), ec);

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
    if (!_ws_stream) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    ASIO_DBG_WS ("starting handshake, type=%s", handshake_type == 0 ? "client" : "server");

    if (handshake_type == client) {
        //  Client-side WebSocket handshake
        _ws_stream->async_handshake (
          _host, _path, [this, handler] (const boost::system::error_code &ec) {
              if (!ec) {
                  _handshake_complete = true;
                  ASIO_DBG_WS ("client handshake complete");
              } else {
                  ASIO_DBG_WS ("client handshake failed: %s", ec.message ().c_str ());
              }
              if (handler) {
                  handler (ec, 0);
              }
          });
    } else {
        //  Server-side WebSocket handshake
        _ws_stream->async_accept ([this, handler] (const boost::system::error_code &ec) {
            if (!ec) {
                _handshake_complete = true;
                ASIO_DBG_WS ("server handshake complete");
            } else {
                ASIO_DBG_WS ("server handshake failed: %s", ec.message ().c_str ());
            }
            if (handler) {
                handler (ec, 0);
            }
        });
    }
}

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_WS
