/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "transports/tls/ssl_transport.hpp"

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_SSL

#include "engine/asio/asio_debug.hpp"
#include "core/address.hpp"

#include <openssl/ssl.h>

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
}

ssl_transport_t::ssl_transport_t (boost::asio::ssl::context &ssl_ctx) :
    _ssl_ctx (ssl_ctx), _handshake_complete (false)
{
}

ssl_transport_t::~ssl_transport_t ()
{
    close ();
}

bool ssl_transport_t::open (boost::asio::io_context &io_context, fd_t fd)
{
    //  Close any existing stream
    close ();

    //  Create the underlying TCP socket
    boost::asio::ip::tcp::socket socket (io_context);
    boost::system::error_code ec;

    //  Assign the file descriptor to the socket
    socket.assign (protocol_for_fd (fd), fd, ec);
    if (ec) {
        ASIO_GLOBAL_ERROR ("ssl_transport assign failed: %s", ec.message ().c_str ());
        return false;
    }

    //  Keep synchronous read_some/write_some paths non-blocking.
    socket.native_non_blocking (true, ec);
    if (ec) {
        ASIO_GLOBAL_ERROR ("ssl_transport non-blocking failed: %s", ec.message ().c_str ());
        return false;
    }

    //  Create SSL stream wrapping the socket
    try {
        _ssl_stream = std::make_shared<ssl_stream_t> (std::move (socket), _ssl_ctx);
    }
    catch (const std::bad_alloc &) {
        ASIO_GLOBAL_ERROR ("ssl_transport stream allocation failed");
        return false;
    }

    _handshake_complete = false;
    return true;
}

bool ssl_transport_t::is_open () const
{
    return _ssl_stream && _ssl_stream->lowest_layer ().is_open ();
}

void ssl_transport_t::close ()
{
    std::shared_ptr<ssl_stream_t> stream = _ssl_stream;
    _ssl_stream.reset ();

    if (stream) {
        boost::system::error_code ec;

        stream->lowest_layer ().cancel (ec);

        //  Avoid blocking SSL shutdown on close; just close the TCP layer.
        stream->lowest_layer ().shutdown (boost::asio::ip::tcp::socket::shutdown_both, ec);
        stream->lowest_layer ().close (ec);
    }

    _handshake_complete = false;
}

void ssl_transport_t::async_read_some (unsigned char *buffer,
                                       std::size_t buffer_size,
                                       completion_handler_t handler)
{
    std::shared_ptr<ssl_stream_t> stream = _ssl_stream;
    if (!stream || !_handshake_complete) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    stream->async_read_some (
      boost::asio::buffer (buffer, buffer_size),
      [stream,
       handler = std::move (handler)] (const boost::system::error_code &ec,
                                       std::size_t bytes_transferred) {
          if (handler) {
              handler (ec, bytes_transferred);
          }
      });
}

std::size_t ssl_transport_t::read_some (std::uint8_t *buffer, std::size_t len)
{
    if (len == 0) {
        errno = 0;
        return 0;
    }

    std::shared_ptr<ssl_stream_t> stream = _ssl_stream;
    if (!stream || !_handshake_complete) {
        errno = ENOTCONN;
        return 0;
    }

    if (!stream->lowest_layer ().is_open ()) {
        errno = EBADF;
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
            || ec == boost::asio::error::broken_pipe) {
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

void ssl_transport_t::async_write_some (const unsigned char *buffer,
                                        std::size_t buffer_size,
                                        completion_handler_t handler)
{
    std::shared_ptr<ssl_stream_t> stream = _ssl_stream;
    if (!stream || !_handshake_complete) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    boost::asio::async_write (
      *stream, boost::asio::buffer (buffer, buffer_size),
      [stream,
       handler = std::move (handler)] (const boost::system::error_code &ec,
                                       std::size_t bytes_transferred) {
          if (handler) {
              handler (ec, bytes_transferred);
          }
      });
}

std::size_t ssl_transport_t::write_some (const std::uint8_t *data, std::size_t len)
{
    if (len == 0) {
        return 0;
    }

    //  Check transport state
    std::shared_ptr<ssl_stream_t> stream = _ssl_stream;
    if (!stream || !_handshake_complete) {
        errno = ENOTCONN;
        return 0;
    }

    if (!stream->lowest_layer ().is_open ()) {
        errno = EBADF;
        return 0;
    }

    boost::system::error_code ec;
    std::size_t bytes_written = 0;

    //  Perform synchronous write_some on SSL stream
    //  Note: SSL write_some may write fewer bytes than requested
    bytes_written = stream->write_some (boost::asio::buffer (data, len), ec);

    if (ec) {
        //  Handle would_block case
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
            errno = EAGAIN;
            return 0;
        }

        //  Handle SSL-specific errors
        if (ec.category () == boost::asio::error::get_ssl_category ()) {
            //  SSL errors - treat as generic I/O error
            errno = EIO;
            return 0;
        }

        //  Map boost error codes to errno
        if (ec == boost::asio::error::broken_pipe || ec == boost::asio::error::connection_reset) {
            errno = EPIPE;
        } else if (ec == boost::asio::error::not_connected) {
            errno = ENOTCONN;
        } else if (ec == boost::asio::error::bad_descriptor) {
            errno = EBADF;
        } else if (ec == boost::asio::error::eof) {
            errno = ECONNRESET;
        } else {
            errno = EIO;
        }
        return 0;
    }

    errno = 0;
    return bytes_written;
}

void ssl_transport_t::async_handshake (int handshake_type, completion_handler_t handler)
{
    std::shared_ptr<ssl_stream_t> stream = _ssl_stream;
    if (!stream) {
        if (handler) {
            handler (boost::asio::error::not_connected, 0);
        }
        return;
    }

    auto hs_type = (handshake_type == 0) ? boost::asio::ssl::stream_base::client
                                         : boost::asio::ssl::stream_base::server;

    if (handshake_type == client && !_hostname.empty ()) {
        if (!SSL_set_tlsext_host_name (stream->native_handle (), _hostname.c_str ())) {
            if (handler) {
                handler (boost::asio::error::invalid_argument, 0);
            }
            return;
        }
    }

    stream->async_handshake (
      hs_type,
      [this, stream, handler = std::move (handler)] (const boost::system::error_code &ec) {
          if (!ec) {
              _handshake_complete = true;
          }
          if (handler) {
              handler (ec, 0);
          }
      });
}

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_SSL
