/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_WS_TRANSPORT_COMMON_INTERNAL_HPP_INCLUDED__
#define __ZLINK_WS_TRANSPORT_COMMON_INTERNAL_HPP_INCLUDED__

#include "core/poller.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_ASIO_WS

#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>
#include <array>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <utility>

#include "core/address.hpp"
#include "engine/asio/i_asio_transport.hpp"
#include "transports/ws/ws_batch_policy.hpp"
#include "utils/env.hpp"

namespace zlink
{
namespace ws_transport_common_internal
{
// open() always assigns the descriptor to its owning io_context. Keep that
// executor type through Beast's composed operations instead of erasing and
// reconstructing the same executor at every continuation.
typedef boost::asio::basic_stream_socket<
  boost::asio::ip::tcp, boost::asio::io_context::executor_type> socket_t;

inline boost::asio::ip::tcp protocol_for_fd (fd_t fd_)
{
    sockaddr_storage ss;
    const zlink_socklen_t sl = get_socket_address (fd_, socket_end_local, &ss);
    if (sl != 0 && ss.ss_family == AF_INET6)
        return boost::asio::ip::tcp::v6 ();
    return boost::asio::ip::tcp::v4 ();
}

inline size_t write_buffer_bytes ()
{
    static const size_t value =
      env::positive_size ("ZLINK_WS_WRITE_BUFFER_BYTES",
                          ws_batch_policy::zmp_send_batch_max_size ());
    return std::min (value, ws_batch_policy::write_buffer_initial_size ());
}

inline size_t write_buffer_bytes_for_payload (size_t payload_bytes_)
{
    static const size_t maximum =
      env::positive_size ("ZLINK_WS_WRITE_BUFFER_BYTES",
                          ws_batch_policy::zmp_send_batch_max_size ());
    const size_t initial =
      std::min (maximum, ws_batch_policy::write_buffer_initial_size ());
    return payload_bytes_ > initial ? maximum : initial;
}

inline size_t read_message_max ()
{
    static const size_t value =
      env::positive_size ("ZLINK_WS_READ_MESSAGE_MAX", 64 * 1024 * 1024);
    return value;
}

inline void debug_read_failure (const char *category_,
                                const boost::system::error_code &ec_)
{
#ifdef ZLINK_ASIO_DEBUG
    fprintf (stderr, "[ASIO:%s] read failed: %s\n", category_, ec_.message ().c_str ());
#else
    (void) category_;
    (void) ec_;
#endif
}

inline void debug_write_completion (const char *category_,
                                    const char *operation_,
                                    const boost::system::error_code &ec_,
                                    std::size_t bytes_transferred_)
{
#ifdef ZLINK_ASIO_DEBUG
    fprintf (stderr, "[ASIO:%s] %s complete: ec=%s, bytes=%zu\n", category_, operation_,
             ec_.message ().c_str (), bytes_transferred_);
#else
    (void) category_;
    (void) operation_;
    (void) ec_;
    (void) bytes_transferred_;
#endif
}

inline void debug_write_failure (const char *category_,
                                 bool secure_,
                                 const boost::system::error_code &ec_)
{
#ifdef ZLINK_ASIO_DEBUG
    fprintf (stderr, "[ASIO:%s] write_some%s error: %s\n", category_,
             secure_ ? " SSL" : "", ec_.message ().c_str ());
#else
    (void) category_;
    (void) secure_;
    (void) ec_;
#endif
}

template <typename connection_t>
inline void configure_stream (connection_t *connection_)
{
    connection_->stream.binary (true);
    connection_->stream.auto_fragment (false);
    connection_->stream.write_buffer_bytes (write_buffer_bytes ());
    connection_->stream.read_message_max (read_message_max ());
}

template <typename connection_t>
inline bool read_message_binary (const std::shared_ptr<connection_t> &connection_)
{
    return !connection_ || connection_->read_message_state.is_binary ();
}

template <typename connection_t>
void async_read_some (std::shared_ptr<connection_t> connection_,
                      bool ready_,
                      unsigned char *buffer_,
                      std::size_t buffer_size_,
                      i_asio_transport::completion_handler_t handler_,
                      const char *debug_category_)
{
    if (!connection_ || !ready_) {
        if (handler_)
            handler_ (boost::asio::error::not_connected, 0);
        return;
    }

    connection_->read_message_state.reset ();
    if (buffer_size_ == 0) {
        if (handler_) {
            boost::asio::post (connection_->stream.get_executor (),
                               [handler = std::move (handler_)] () {
                                   handler (boost::system::error_code (), 0);
                               });
        }
        return;
    }

    connection_->stream.async_read_some (
      boost::asio::buffer (buffer_, buffer_size_),
      [connection = std::move (connection_), handler = std::move (handler_),
       debug_category_] (const boost::system::error_code &ec,
                         std::size_t bytes_transferred) {
          connection->read_message_state.finish (
            !ec ? connection->stream.got_binary () : true);
          if (ec)
              debug_read_failure (debug_category_, ec);
          if (handler)
              handler (ec, bytes_transferred);
      });
}

template <typename connection_t>
std::size_t read_some (connection_t *connection_,
                       bool ready_,
                       const boost::system::error_category *secure_error_category_,
                       std::uint8_t *buffer_,
                       std::size_t len_)
{
    if (connection_)
        connection_->read_message_state.reset ();
    if (len_ == 0) {
        errno = 0;
        return 0;
    }

    if (!connection_ || !ready_) {
        errno = ENOTCONN;
        return 0;
    }

    boost::system::error_code ec;
    const std::size_t bytes_read =
      connection_->stream.read_some (boost::asio::buffer (buffer_, len_), ec);
    if (ec) {
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
            errno = EAGAIN;
        } else if (ec == boost::asio::error::eof
                   || ec == boost::asio::error::connection_reset
                   || ec == boost::asio::error::broken_pipe
                   || ec == boost::beast::websocket::error::closed) {
            errno = EPIPE;
        } else if (secure_error_category_
                   && ec.category () == *secure_error_category_) {
            errno = EIO;
        } else if (ec == boost::asio::error::not_connected) {
            errno = ENOTCONN;
        } else if (ec == boost::asio::error::bad_descriptor) {
            errno = EBADF;
        } else {
            errno = EIO;
        }
        return 0;
    }

    connection_->read_message_state.finish (connection_->stream.got_binary ());
    errno = 0;
    return bytes_read;
}

template <typename connection_t>
void async_write_some (std::shared_ptr<connection_t> connection_,
                       bool ready_,
                       const unsigned char *buffer_,
                       std::size_t buffer_size_,
                       i_asio_transport::completion_handler_t handler_,
                       const char *debug_category_)
{
    if (!connection_ || !ready_) {
        if (handler_)
            handler_ (boost::asio::error::not_connected, 0);
        return;
    }

    connection_->stream.write_buffer_bytes (
      write_buffer_bytes_for_payload (buffer_size_));
    connection_->stream.async_write (
      boost::asio::buffer (buffer_, buffer_size_),
      [connection = std::move (connection_), handler = std::move (handler_),
       debug_category_] (const boost::system::error_code &ec,
                         std::size_t bytes_transferred) {
          debug_write_completion (debug_category_, "write", ec, bytes_transferred);
          if (handler)
              handler (ec, bytes_transferred);
      });
}

template <typename connection_t>
void async_writev (std::shared_ptr<connection_t> connection_,
                   bool ready_,
                   const unsigned char *header_,
                   std::size_t header_size_,
                   const unsigned char *body_,
                   std::size_t body_size_,
                   i_asio_transport::completion_handler_t handler_,
                   const char *debug_category_)
{
    if (!connection_ || !ready_) {
        if (handler_)
            handler_ (boost::asio::error::not_connected, 0);
        return;
    }

    const std::array<boost::asio::const_buffer, 2> buffers = {
      boost::asio::buffer (header_, header_size_),
      boost::asio::buffer (body_, body_size_)};
    connection_->stream.write_buffer_bytes (
      write_buffer_bytes_for_payload (header_size_ + body_size_));
    connection_->stream.async_write (
      buffers,
      [connection = std::move (connection_), handler = std::move (handler_),
       debug_category_] (const boost::system::error_code &ec,
                         std::size_t bytes_transferred) {
          debug_write_completion (debug_category_, "writev", ec, bytes_transferred);
          if (handler)
              handler (ec, bytes_transferred);
      });
}

template <typename connection_t>
std::size_t write_some (connection_t *connection_,
                        bool ready_,
                        bool stream_open_,
                        const boost::system::error_category *secure_error_category_,
                        const std::uint8_t *data_,
                        std::size_t len_,
                        const char *debug_category_)
{
    if (len_ == 0)
        return 0;
    if (!connection_ || !ready_) {
        errno = ENOTCONN;
        return 0;
    }
    if (!stream_open_) {
        errno = EBADF;
        return 0;
    }

    connection_->stream.write_buffer_bytes (
      write_buffer_bytes_for_payload (len_));
    boost::system::error_code ec;
    const std::size_t bytes_written =
      connection_->stream.write (boost::asio::buffer (data_, len_), ec);
    if (ec) {
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
            errno = EAGAIN;
        } else if (secure_error_category_
                   && ec.category () == *secure_error_category_) {
            errno = EIO;
            debug_write_failure (debug_category_, true, ec);
        } else if (ec == boost::asio::error::broken_pipe
                   || ec == boost::asio::error::connection_reset) {
            errno = EPIPE;
        } else if (ec == boost::asio::error::not_connected) {
            errno = ENOTCONN;
        } else if (ec == boost::asio::error::bad_descriptor) {
            errno = EBADF;
        } else if (ec == boost::asio::error::eof
                   || ec == boost::beast::websocket::error::closed) {
            errno = ECONNRESET;
        } else {
            errno = EIO;
        }
        if (!(secure_error_category_
              && ec.category () == *secure_error_category_)) {
            debug_write_failure (debug_category_, false, ec);
        }
        return 0;
    }

#ifdef ZLINK_ASIO_DEBUG
    fprintf (stderr, "[ASIO:%s] write_some: wrote %zu bytes as frame\n", debug_category_,
             bytes_written);
#else
    (void) debug_category_;
#endif
    errno = 0;
    return bytes_written;
}
}
}

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_ASIO_WS

#endif // __ZLINK_WS_TRANSPORT_COMMON_INTERNAL_HPP_INCLUDED__
