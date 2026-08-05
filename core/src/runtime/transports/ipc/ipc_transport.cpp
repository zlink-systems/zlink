/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO && defined ZLINK_HAVE_IPC

#include "transports/ipc/ipc_transport.hpp"

#include "utils/env.hpp"
#include "utils/err.hpp"
#include <atomic>
#include <algorithm>
#include <array>
#ifndef ZLINK_HAVE_WINDOWS
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace zlink
{
namespace
{
std::atomic<uint64_t> ipc_async_read_calls (0);
std::atomic<uint64_t> ipc_async_read_bytes (0);
std::atomic<uint64_t> ipc_async_read_errors (0);
std::atomic<uint64_t> ipc_read_some_calls (0);
std::atomic<uint64_t> ipc_read_some_bytes (0);
std::atomic<uint64_t> ipc_read_some_eagain (0);
std::atomic<uint64_t> ipc_read_some_errors (0);
std::atomic<uint64_t> ipc_async_write_calls (0);
std::atomic<uint64_t> ipc_async_write_bytes (0);
std::atomic<uint64_t> ipc_async_write_errors (0);
std::atomic<uint64_t> ipc_write_some_calls (0);
std::atomic<uint64_t> ipc_write_some_bytes (0);
std::atomic<uint64_t> ipc_write_some_eagain (0);
std::atomic<uint64_t> ipc_write_some_errors (0);
std::atomic<bool> ipc_stats_registered (false);

const bool ipc_stats_on = env::flag_enabled ("ZLINK_ASIO_IPC_STATS");

const bool ipc_force_async_on = env::flag_enabled ("ZLINK_ASIO_IPC_FORCE_ASYNC");

const bool ipc_allow_sync_write_on = env::flag_enabled ("ZLINK_ASIO_IPC_SYNC_WRITE");

const bool ipc_use_async_write_some_on = env::flag_enabled ("ZLINK_ASIO_IPC_ASYNC_WRITE_SOME");

const bool ipc_writev_single_shot_on = env::flag_enabled ("ZLINK_ASIO_WRITEV_SINGLE_SHOT");

const bool ipc_use_asio_writev_on = env::flag_enabled ("ZLINK_ASIO_WRITEV_USE_ASIO");

void ipc_stats_dump ()
{
    std::fprintf (stderr,
                  "[ASIO_IPC_STATS] async_read calls=%llu bytes=%llu errors=%llu\n"
                  "[ASIO_IPC_STATS] read_some calls=%llu bytes=%llu eagain=%llu "
                  "errors=%llu\n"
                  "[ASIO_IPC_STATS] async_write calls=%llu bytes=%llu errors=%llu\n"
                  "[ASIO_IPC_STATS] write_some calls=%llu bytes=%llu eagain=%llu errors=%llu\n",
                  static_cast<unsigned long long> (ipc_async_read_calls.load ()),
                  static_cast<unsigned long long> (ipc_async_read_bytes.load ()),
                  static_cast<unsigned long long> (ipc_async_read_errors.load ()),
                  static_cast<unsigned long long> (ipc_read_some_calls.load ()),
                  static_cast<unsigned long long> (ipc_read_some_bytes.load ()),
                  static_cast<unsigned long long> (ipc_read_some_eagain.load ()),
                  static_cast<unsigned long long> (ipc_read_some_errors.load ()),
                  static_cast<unsigned long long> (ipc_async_write_calls.load ()),
                  static_cast<unsigned long long> (ipc_async_write_bytes.load ()),
                  static_cast<unsigned long long> (ipc_async_write_errors.load ()),
                  static_cast<unsigned long long> (ipc_write_some_calls.load ()),
                  static_cast<unsigned long long> (ipc_write_some_bytes.load ()),
                  static_cast<unsigned long long> (ipc_write_some_eagain.load ()),
                  static_cast<unsigned long long> (ipc_write_some_errors.load ()));
}

void ipc_stats_maybe_register ()
{
    bool expected = false;
    if (ipc_stats_registered.compare_exchange_strong (expected, true)) {
        std::atexit (ipc_stats_dump);
    }
}
}

ipc_transport_t::ipc_transport_t ()
{
}

ipc_transport_t::~ipc_transport_t ()
{
    close ();
}

bool ipc_transport_t::open (boost::asio::io_context &io_context, fd_t fd)
{
    try {
        _socket = std::shared_ptr<boost::asio::local::stream_protocol::socket> (
          new boost::asio::local::stream_protocol::socket (io_context));
    }
    catch (const std::bad_alloc &) {
        return false;
    }

    boost::asio::local::stream_protocol protocol;
    boost::system::error_code ec;
    _socket->assign (protocol, fd, ec);
    if (ec) {
        const int tmp_errno = ec.value ();
        errno = tmp_errno;
        _socket.reset ();
        return false;
    }

    //  The fd is already set to non-blocking; inform Asio to avoid blocking
    //  synchronous write_some/read_some paths.
    _socket->native_non_blocking (true, ec);
    if (ec) {
        const int tmp_errno = ec.value ();
        errno = tmp_errno;
        _socket.reset ();
        return false;
    }

    return true;
}

bool ipc_transport_t::is_open () const
{
    return _socket && _socket->is_open ();
}

void ipc_transport_t::close ()
{
    if (_socket) {
        boost::system::error_code ec;
        _socket->close (ec);
        _socket.reset ();
    }
}

void ipc_transport_t::async_read_some (unsigned char *buffer,
                                       std::size_t buffer_size,
                                       completion_handler_t handler)
{
    if (ipc_stats_on) {
        ipc_stats_maybe_register ();
        ++ipc_async_read_calls;
    }

    if (_socket) {
        const std::shared_ptr<boost::asio::local::stream_protocol::socket> socket = _socket;
        if (ipc_stats_on) {
            socket->async_read_some (
              boost::asio::buffer (buffer, buffer_size),
              [handler, socket] (const boost::system::error_code &ec, std::size_t bytes) {
                  LIBZLINK_UNUSED (socket);
                  if (ec)
                      ++ipc_async_read_errors;
                  else
                      ipc_async_read_bytes += bytes;
                  if (handler)
                      handler (ec, bytes);
              });
        } else {
            const auto wrapped_handler = [handler, socket] (const boost::system::error_code &ec,
                                                            std::size_t bytes) {
                LIBZLINK_UNUSED (socket);
                if (handler)
                    handler (ec, bytes);
            };
            socket->async_read_some (boost::asio::buffer (buffer, buffer_size), wrapped_handler);
        }
    } else if (handler) {
        handler (boost::asio::error::bad_descriptor, 0);
    }
}

std::size_t ipc_transport_t::read_some (std::uint8_t *buffer, std::size_t len)
{
    if (len == 0) {
        errno = 0;
        return 0;
    }

    if (!_socket || !_socket->is_open ()) {
        errno = EBADF;
        return 0;
    }

    if (ipc_stats_on) {
        ipc_stats_maybe_register ();
        ++ipc_read_some_calls;
    }

    const fd_t fd = _socket->native_handle ();
    ssize_t rc = 0;
    do {
#ifdef MSG_DONTWAIT
        rc = ::recv (fd, buffer, len, MSG_DONTWAIT);
#else
        rc = ::recv (fd, buffer, len, 0);
#endif
    } while (rc == -1 && errno == EINTR);

    if (rc > 0) {
        if (ipc_stats_on)
            ipc_read_some_bytes += static_cast<std::size_t> (rc);
        return static_cast<std::size_t> (rc);
    }

    if (rc == 0) {
        errno = EPIPE;
        if (ipc_stats_on)
            ++ipc_read_some_errors;
        return 0;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        errno = EAGAIN;
        if (ipc_stats_on)
            ++ipc_read_some_eagain;
        return 0;
    }

    if (ipc_stats_on)
        ++ipc_read_some_errors;
    return 0;
}

void ipc_transport_t::async_write_some (const unsigned char *buffer,
                                        std::size_t buffer_size,
                                        completion_handler_t handler)
{
    if (ipc_stats_on) {
        ipc_stats_maybe_register ();
        ++ipc_async_write_calls;
    }

    if (_socket) {
        const std::shared_ptr<boost::asio::local::stream_protocol::socket> socket = _socket;
        if (ipc_stats_on) {
            const auto stats_handler = [handler, socket] (const boost::system::error_code &ec,
                                                          std::size_t bytes) {
                LIBZLINK_UNUSED (socket);
                if (ec)
                    ++ipc_async_write_errors;
                else
                    ipc_async_write_bytes += bytes;
                if (handler)
                    handler (ec, bytes);
            };
            if (ipc_use_async_write_some_on) {
                socket->async_write_some (boost::asio::buffer (buffer, buffer_size), stats_handler);
            } else {
                boost::asio::async_write (*socket, boost::asio::buffer (buffer, buffer_size),
                                          stats_handler);
            }
        } else {
            const auto wrapped_handler = [handler, socket] (const boost::system::error_code &ec,
                                                            std::size_t bytes) {
                LIBZLINK_UNUSED (socket);
                if (handler)
                    handler (ec, bytes);
            };
            if (ipc_use_async_write_some_on) {
                socket->async_write_some (boost::asio::buffer (buffer, buffer_size),
                                          wrapped_handler);
            } else {
                boost::asio::async_write (*socket, boost::asio::buffer (buffer, buffer_size),
                                          wrapped_handler);
            }
        }
    } else if (handler) {
        handler (boost::asio::error::bad_descriptor, 0);
    }
}

void ipc_transport_t::async_writev (const unsigned char *header,
                                    std::size_t header_size,
                                    const unsigned char *body,
                                    std::size_t body_size,
                                    completion_handler_t handler)
{
    if (ipc_stats_on) {
        ipc_stats_maybe_register ();
        ++ipc_async_write_calls;
    }

    if (!_socket) {
        if (handler)
            handler (boost::asio::error::bad_descriptor, 0);
        return;
    }
    const std::shared_ptr<boost::asio::local::stream_protocol::socket> socket = _socket;

    if (body_size == 0) {
        async_write_some (header, header_size, handler);
        return;
    }

    if (header_size == 0) {
        async_write_some (body, body_size, handler);
        return;
    }

#if defined(ZLINK_HAVE_WINDOWS)
    if (ipc_stats_on) {
        const auto stats_handler = [handler] (const boost::system::error_code &ec,
                                              std::size_t bytes) {
            if (ec)
                ++ipc_async_write_errors;
            else
                ipc_async_write_bytes += bytes;
            if (handler)
                handler (ec, bytes);
        };
        std::array<boost::asio::const_buffer, 2> buffers = {
          boost::asio::buffer (header, header_size), boost::asio::buffer (body, body_size)};
        boost::asio::async_write (*socket, buffers, stats_handler);
    } else {
        const auto wrapped_handler = [handler, socket] (const boost::system::error_code &ec,
                                                        std::size_t bytes) {
            LIBZLINK_UNUSED (socket);
            if (handler)
                handler (ec, bytes);
        };
        std::array<boost::asio::const_buffer, 2> buffers = {
          boost::asio::buffer (header, header_size), boost::asio::buffer (body, body_size)};
        boost::asio::async_write (*socket, buffers, wrapped_handler);
    }
#else
    if (ipc_use_asio_writev_on) {
        if (ipc_stats_on) {
            const auto stats_handler = [handler, socket] (const boost::system::error_code &ec,
                                                          std::size_t bytes) {
                LIBZLINK_UNUSED (socket);
                if (ec)
                    ++ipc_async_write_errors;
                else
                    ipc_async_write_bytes += bytes;
                if (handler)
                    handler (ec, bytes);
            };
            std::array<boost::asio::const_buffer, 2> buffers = {
              boost::asio::buffer (header, header_size), boost::asio::buffer (body, body_size)};
            boost::asio::async_write (*socket, buffers, stats_handler);
        } else {
            const auto wrapped_handler = [handler, socket] (const boost::system::error_code &ec,
                                                            std::size_t bytes) {
                LIBZLINK_UNUSED (socket);
                if (handler)
                    handler (ec, bytes);
            };
            std::array<boost::asio::const_buffer, 2> buffers = {
              boost::asio::buffer (header, header_size), boost::asio::buffer (body, body_size)};
            boost::asio::async_write (*socket, buffers, wrapped_handler);
        }
        return;
    }

    struct writev_state_t
    {
        const unsigned char *header;
        size_t header_size;
        size_t header_sent;
        const unsigned char *body;
        size_t body_size;
        size_t body_sent;
        completion_handler_t handler;
    };

    const std::shared_ptr<writev_state_t> state (
      new writev_state_t{header, header_size, 0, body, body_size, 0, handler});

    const std::shared_ptr<std::function<void (const boost::system::error_code &)>> do_write (
      new std::function<void (const boost::system::error_code &)>);

    *do_write = [socket, state, do_write] (const boost::system::error_code &ec) {
        if (ec) {
            if (ipc_stats_on)
                ++ipc_async_write_errors;
            if (state->handler)
                state->handler (ec, state->header_sent + state->body_sent);
            return;
        }

        if (!socket || !socket->is_open ()) {
            if (state->handler)
                state->handler (boost::asio::error::bad_descriptor,
                                state->header_sent + state->body_sent);
            return;
        }

        for (;;) {
            const size_t header_left = state->header_size - state->header_sent;
            const size_t body_left = state->body_size - state->body_sent;
            if (header_left == 0 && body_left == 0) {
                if (ipc_stats_on)
                    ipc_async_write_bytes += (state->header_size + state->body_size);
                if (state->handler)
                    state->handler (boost::system::error_code (),
                                    state->header_size + state->body_size);
                return;
            }

            struct iovec iov[2];
            int iovcnt = 0;
            if (header_left > 0) {
                iov[iovcnt].iov_base =
                  const_cast<unsigned char *> (state->header + state->header_sent);
                iov[iovcnt].iov_len = header_left;
                ++iovcnt;
            }
            if (body_left > 0) {
                iov[iovcnt].iov_base = const_cast<unsigned char *> (state->body + state->body_sent);
                iov[iovcnt].iov_len = body_left;
                ++iovcnt;
            }

            const ssize_t rc = ::writev (socket->native_handle (), iov, iovcnt);
            if (rc > 0) {
                size_t remaining = static_cast<size_t> (rc);
                if (header_left > 0) {
                    const size_t adv = std::min (header_left, remaining);
                    state->header_sent += adv;
                    remaining -= adv;
                }
                if (remaining > 0 && body_left > 0) {
                    state->body_sent += remaining;
                }
                if (ipc_writev_single_shot_on) {
                    const size_t left = (state->header_size - state->header_sent)
                                        + (state->body_size - state->body_sent);
                    if (left == 0) {
                        if (ipc_stats_on)
                            ipc_async_write_bytes += (state->header_size + state->body_size);
                        if (state->handler)
                            state->handler (boost::system::error_code (),
                                            state->header_size + state->body_size);
                        return;
                    }
                    socket->async_wait (
                      boost::asio::socket_base::wait_write,
                      [do_write] (const boost::system::error_code &wec) { (*do_write) (wec); });
                    return;
                }
                continue;
            }
            if (rc == -1 && errno == EINTR)
                continue;
            if (rc == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
                socket->async_wait (
                  boost::asio::socket_base::wait_write,
                  [do_write] (const boost::system::error_code &wec) { (*do_write) (wec); });
                return;
            }

            boost::system::error_code werr (errno, boost::system::system_category ());
            if (ipc_stats_on)
                ++ipc_async_write_errors;
            if (state->handler)
                state->handler (werr, state->header_sent + state->body_sent);
            return;
        }
    };

    (*do_write) (boost::system::error_code ());
#endif
}

std::size_t ipc_transport_t::write_some (const std::uint8_t *data, std::size_t len)
{
    if (len == 0) {
        return 0;
    }

    if (!_socket || !_socket->is_open ()) {
        errno = EBADF;
        return 0;
    }

    //  Only force EAGAIN if explicitly requested via ZLINK_ASIO_IPC_FORCE_ASYNC.
    //  Otherwise, attempt actual socket write like TCP transport does.
    //  Since write_some() is non-blocking, there's no deadlock risk.
    if (ipc_force_async_on) {
        errno = EAGAIN;
        if (ipc_stats_on) {
            ipc_stats_maybe_register ();
            ++ipc_write_some_calls;
            ++ipc_write_some_eagain;
        }
        return 0;
    }

    if (ipc_stats_on) {
        ipc_stats_maybe_register ();
        ++ipc_write_some_calls;
    }

    boost::system::error_code ec;
    const std::size_t bytes_written = _socket->write_some (boost::asio::buffer (data, len), ec);

    if (ec) {
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
            errno = EAGAIN;
            if (ipc_stats_on)
                ++ipc_write_some_eagain;
            return 0;
        }
        if (ec == boost::asio::error::broken_pipe || ec == boost::asio::error::connection_reset) {
            errno = EPIPE;
        } else if (ec == boost::asio::error::not_connected) {
            errno = ENOTCONN;
        } else if (ec == boost::asio::error::bad_descriptor) {
            errno = EBADF;
        } else {
            errno = EIO;
        }
        if (ipc_stats_on)
            ++ipc_write_some_errors;
        return 0;
    }

    errno = 0;
    if (ipc_stats_on)
        ipc_write_some_bytes += bytes_written;
    return bytes_written;
}

bool ipc_transport_t::supports_speculative_write () const
{
    return ipc_allow_sync_write_on && !ipc_force_async_on;
}

bool ipc_transport_t::supports_speculative_read () const
{
    return !ipc_force_async_on;
}

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO && ZLINK_HAVE_IPC
