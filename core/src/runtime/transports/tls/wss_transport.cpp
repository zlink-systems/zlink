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
    static size_t value = 0;
    if (value == 0)
        value = env::positive_size ("ZLINK_WS_WRITE_BUFFER_BYTES", 64 * 1024);
    return value;
}

size_t wss_read_message_max ()
{
    static size_t value = 0;
    if (value == 0)
        value = env::positive_size ("ZLINK_WS_READ_MESSAGE_MAX", 64 * 1024 * 1024);
    return value;
}
}

wss_transport_t::wss_transport_t (boost::asio::ssl::context &ssl_ctx,
                                  const std::string &path,
                                  const std::string &host) :
    _ssl_ctx (ssl_ctx),
    _path (path),
    _host (host),
    _ssl_handshake_complete (false),
    _ws_handshake_complete (false),
    _handshake_type (client),
    _read_state (new read_state_t)
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

    //  Create WebSocket stream wrapping the SSL stream
    try {
        _wss_stream = std::shared_ptr<wss_stream_t> (new wss_stream_t (std::move (ssl_stream)));
    }
    catch (const std::bad_alloc &) {
        ASIO_GLOBAL_ERROR ("wss_transport stream allocation failed");
        return false;
    }

    //  Configure WebSocket options
    //  Use binary mode for ZLINK messages
    _wss_stream->binary (true);
    _wss_stream->auto_fragment (false);
    _wss_stream->write_buffer_bytes (wss_write_buffer_bytes ());
    _wss_stream->read_message_max (wss_read_message_max ());

    _ssl_handshake_complete = false;
    _ws_handshake_complete = false;
    _read_state.reset (new read_state_t);

    ASIO_DBG_WSS ("opened with path=%s, host=%s", _path.c_str (), _host.c_str ());
    return true;
}

bool wss_transport_t::is_open () const
{
    return _wss_stream && _wss_stream->next_layer ().next_layer ().is_open ();
}

void wss_transport_t::close ()
{
    std::shared_ptr<wss_stream_t> stream = _wss_stream;
    _wss_stream.reset ();

    if (stream) {
        boost::system::error_code ec;
        stream->next_layer ().next_layer ().cancel (ec);

        //  Avoid blocking WebSocket/SSL shutdown; just close the TCP layer.
        stream->next_layer ().next_layer ().shutdown (boost::asio::ip::tcp::socket::shutdown_both,
                                                      ec);
        stream->next_layer ().next_layer ().close (ec);
    }

    _ssl_handshake_complete = false;
    _ws_handshake_complete = false;
    if (_read_state) {
        _read_state->closed = true;
        _read_state->clear ();
    }
}

void wss_transport_t::async_read_some (unsigned char *buffer,
                                       std::size_t buffer_size,
                                       completion_handler_t handler)
{
    std::shared_ptr<wss_stream_t> stream = _wss_stream;
    std::shared_ptr<read_state_t> read_state = _read_state;
    if (!stream || !read_state || !_ws_handshake_complete) {
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
            ASIO_DBG ("WSS", "read failed: %s", ec.message ().c_str ());
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

std::size_t wss_transport_t::read_some (std::uint8_t *buffer, std::size_t len)
{
    if (len == 0) {
        errno = 0;
        return 0;
    }

    std::shared_ptr<wss_stream_t> stream = _wss_stream;
    if (!stream || !_ssl_handshake_complete || !_ws_handshake_complete) {
        errno = ENOTCONN;
        return 0;
    }

    boost::system::error_code ec;
    const std::size_t bytes_read = stream->read_some (boost::asio::buffer (buffer, len), ec);

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

    errno = 0;
    return bytes_read;
}

void wss_transport_t::async_write_some (const unsigned char *buffer,
                                        std::size_t buffer_size,
                                        completion_handler_t handler)
{
    std::shared_ptr<wss_stream_t> stream = _wss_stream;
    if (!stream || !_ws_handshake_complete) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    //  WebSocket writes are frame-based
    stream->async_write (
      boost::asio::buffer (buffer, buffer_size),
      [stream, handler] (const boost::system::error_code &ec, std::size_t bytes_transferred) {
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
    std::shared_ptr<wss_stream_t> stream = _wss_stream;
    if (!stream || !_ws_handshake_complete) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    std::array<boost::asio::const_buffer, 2> buffers = {boost::asio::buffer (header, header_size),
                                                        boost::asio::buffer (body, body_size)};

    stream->async_write (buffers, [stream, handler] (const boost::system::error_code &ec,
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
    std::shared_ptr<wss_stream_t> stream = _wss_stream;
    if (!stream || !_ws_handshake_complete) {
        errno = ENOTCONN;
        return 0;
    }

    if (!stream->next_layer ().next_layer ().is_open ()) {
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
    bytes_written = stream->write (boost::asio::buffer (data, len), ec);

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
    std::shared_ptr<wss_stream_t> stream = _wss_stream;
    if (!stream) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    _handshake_type = handshake_type;

    ASIO_DBG_WSS ("starting SSL handshake, type=%s", handshake_type == 0 ? "client" : "server");

    //  First do SSL handshake
    auto ssl_hs_type = (handshake_type == client) ? boost::asio::ssl::stream_base::client
                                                  : boost::asio::ssl::stream_base::server;

    if (handshake_type == client && !_tls_hostname.empty ()) {
        if (!SSL_set_tlsext_host_name (stream->next_layer ().native_handle (),
                                       _tls_hostname.c_str ())) {
            if (handler) {
                handler (boost::asio::error::invalid_argument, 0);
            }
            return;
        }
    }

    stream->next_layer ().async_handshake (
      ssl_hs_type, [this, stream, handler] (const boost::system::error_code &ec) {
          if (ec) {
              ASIO_DBG_WSS ("SSL handshake failed: %s", ec.message ().c_str ());
              if (handler) {
                  handler (ec, 0);
              }
              return;
          }

          _ssl_handshake_complete = true;
          ASIO_DBG_WSS ("SSL handshake complete, continuing with WebSocket");

          //  Now do WebSocket handshake
          continue_ws_handshake (stream, handler);
      });
}

void wss_transport_t::continue_ws_handshake (const std::shared_ptr<wss_stream_t> &stream,
                                             completion_handler_t handler)
{
    if (!stream) {
        if (handler)
            handler (boost::asio::error::not_connected, 0);
        return;
    }

    if (_handshake_type == client) {
        //  Client-side WebSocket handshake
        stream->async_handshake (
          _host, _path, [this, stream, handler] (const boost::system::error_code &ec) {
              if (!ec) {
                  _ws_handshake_complete = true;
                  ASIO_DBG_WSS ("WebSocket client handshake complete");
              } else {
                  ASIO_DBG_WSS ("WebSocket client handshake failed: %s", ec.message ().c_str ());
              }
              if (handler) {
                  handler (ec, 0);
              }
          });
    } else {
        //  Server-side WebSocket handshake
        stream->async_accept ([this, stream, handler] (const boost::system::error_code &ec) {
            if (!ec) {
                _ws_handshake_complete = true;
                ASIO_DBG_WSS ("WebSocket server handshake complete");
            } else {
                ASIO_DBG_WSS ("WebSocket server handshake failed: %s", ec.message ().c_str ());
            }
            if (handler) {
                handler (ec, 0);
            }
        });
    }
}

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_WS && ZLINK_HAVE_ASIO_SSL
