/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "transports/tcp/tcp_transport.hpp"

#if defined ZLINK_IOTHREAD_POLLER_USE_ASIO

#include "engine/asio/asio_debug.hpp"
#include "core/address.hpp"
#include "utils/env.hpp"
#include <atomic>
#include <algorithm>
#include <array>
#include <limits>
#ifndef ZLINK_HAVE_WINDOWS
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace zlink
{
namespace
{
std::atomic<uint64_t> tcp_async_read_calls (0);
std::atomic<uint64_t> tcp_async_read_bytes (0);
std::atomic<uint64_t> tcp_async_read_errors (0);
std::atomic<uint64_t> tcp_read_some_calls (0);
std::atomic<uint64_t> tcp_read_some_bytes (0);
std::atomic<uint64_t> tcp_read_some_eagain (0);
std::atomic<uint64_t> tcp_read_some_errors (0);
std::atomic<uint64_t> tcp_async_write_calls (0);
std::atomic<uint64_t> tcp_async_write_bytes (0);
std::atomic<uint64_t> tcp_async_write_errors (0);
std::atomic<uint64_t> tcp_write_some_calls (0);
std::atomic<uint64_t> tcp_write_some_bytes (0);
std::atomic<uint64_t> tcp_write_some_eagain (0);
std::atomic<uint64_t> tcp_write_some_errors (0);
std::atomic<bool> tcp_stats_registered (false);

const bool tcp_stats_on = env::flag_enabled ("ZLINK_ASIO_TCP_STATS");

void tcp_stats_dump ()
{
    std::fprintf (stderr,
                  "[ASIO_TCP_STATS] async_read calls=%llu bytes=%llu errors=%llu\n"
                  "[ASIO_TCP_STATS] read_some calls=%llu bytes=%llu eagain=%llu "
                  "errors=%llu\n"
                  "[ASIO_TCP_STATS] async_write calls=%llu bytes=%llu errors=%llu\n"
                  "[ASIO_TCP_STATS] write_some calls=%llu bytes=%llu eagain=%llu errors=%llu\n",
                  static_cast<unsigned long long> (tcp_async_read_calls.load ()),
                  static_cast<unsigned long long> (tcp_async_read_bytes.load ()),
                  static_cast<unsigned long long> (tcp_async_read_errors.load ()),
                  static_cast<unsigned long long> (tcp_read_some_calls.load ()),
                  static_cast<unsigned long long> (tcp_read_some_bytes.load ()),
                  static_cast<unsigned long long> (tcp_read_some_eagain.load ()),
                  static_cast<unsigned long long> (tcp_read_some_errors.load ()),
                  static_cast<unsigned long long> (tcp_async_write_calls.load ()),
                  static_cast<unsigned long long> (tcp_async_write_bytes.load ()),
                  static_cast<unsigned long long> (tcp_async_write_errors.load ()),
                  static_cast<unsigned long long> (tcp_write_some_calls.load ()),
                  static_cast<unsigned long long> (tcp_write_some_bytes.load ()),
                  static_cast<unsigned long long> (tcp_write_some_eagain.load ()),
                  static_cast<unsigned long long> (tcp_write_some_errors.load ()));
}

void tcp_stats_maybe_register ()
{
    bool expected = false;
    if (tcp_stats_registered.compare_exchange_strong (expected, true)) {
        std::atexit (tcp_stats_dump);
    }
}

boost::asio::ip::tcp protocol_for_fd (fd_t fd_)
{
    sockaddr_storage ss;
    const zlink_socklen_t sl = get_socket_address (fd_, socket_end_local, &ss);
    if (sl != 0 && ss.ss_family == AF_INET6)
        return boost::asio::ip::tcp::v6 ();
    return boost::asio::ip::tcp::v4 ();
}

const bool tcp_allow_sync_write_on = env::flag_enabled ("ZLINK_ASIO_TCP_SYNC_WRITE");

const bool tcp_use_async_write_some_on = env::flag_enabled ("ZLINK_ASIO_TCP_ASYNC_WRITE_SOME");

const bool tcp_use_asio_writev_on = env::flag_enabled ("ZLINK_ASIO_WRITEV_USE_ASIO");

const bool tcp_writev_single_shot_on = env::flag_enabled ("ZLINK_ASIO_WRITEV_SINGLE_SHOT");

#if defined ZLINK_HAVE_WINDOWS
const bool tcp_use_native_send_on =
  !env::flag_enabled ("ZLINK_ASIO_TCP_DISABLE_NATIVE_SEND");
#else
const bool tcp_use_native_send_on = false;
#endif
}

tcp_transport_t::tcp_transport_t ()
{
}

tcp_transport_t::~tcp_transport_t ()
{
    close ();
}

bool tcp_transport_t::open (boost::asio::io_context &io_context, fd_t fd)
{
    try {
        _socket = std::shared_ptr<boost::asio::ip::tcp::socket> (
          new boost::asio::ip::tcp::socket (io_context));
    }
    catch (const std::bad_alloc &) {
        return false;
    }

    boost::system::error_code ec;
    _socket->assign (protocol_for_fd (fd), fd, ec);
    if (ec) {
        ASIO_GLOBAL_ERROR ("tcp_transport open failed: %s", ec.message ().c_str ());
        _socket.reset ();
        return false;
    }

    //  The fd is already set to non-blocking; inform Asio to avoid blocking
    //  synchronous write_some/read_some paths.
    _socket->native_non_blocking (true, ec);
    if (ec) {
        ASIO_GLOBAL_ERROR ("tcp_transport non-blocking failed: %s", ec.message ().c_str ());
        _socket.reset ();
        return false;
    }

    return true;
}

bool tcp_transport_t::is_open () const
{
    return _socket && _socket->is_open ();
}

void tcp_transport_t::close ()
{
    std::shared_ptr<boost::asio::ip::tcp::socket> socket = _socket;
    _socket.reset ();
    if (!socket)
        return;

    boost::system::error_code ec;
    socket->cancel (ec);
    ec.clear ();
    socket->shutdown (boost::asio::ip::tcp::socket::shutdown_both, ec);
    ec.clear ();
    socket->close (ec);
}

void tcp_transport_t::async_read_some (unsigned char *buffer,
                                       std::size_t buffer_size,
                                       completion_handler_t handler)
{
    if (tcp_stats_on) {
        tcp_stats_maybe_register ();
        ++tcp_async_read_calls;
    }

    if (_socket) {
        const std::shared_ptr<boost::asio::ip::tcp::socket> socket = _socket;
        if (tcp_stats_on) {
            socket->async_read_some (
              boost::asio::buffer (buffer, buffer_size),
              [handler, socket] (const boost::system::error_code &ec, std::size_t bytes) {
                  LIBZLINK_UNUSED (socket);
                  if (ec)
                      ++tcp_async_read_errors;
                  else
                      tcp_async_read_bytes += bytes;
                  if (handler)
                      handler (ec, bytes);
              });
        } else {
            socket->async_read_some (
              boost::asio::buffer (buffer, buffer_size),
              [handler, socket] (const boost::system::error_code &ec, std::size_t bytes) {
                  LIBZLINK_UNUSED (socket);
                  if (handler)
                      handler (ec, bytes);
              });
        }
    } else if (handler) {
        handler (boost::asio::error::bad_descriptor, 0);
    }
}

std::size_t tcp_transport_t::read_some (std::uint8_t *buffer, std::size_t len)
{
    if (len == 0) {
        errno = 0;
        return 0;
    }

    if (!_socket || !_socket->is_open ()) {
        errno = EBADF;
        return 0;
    }

    if (tcp_stats_on) {
        tcp_stats_maybe_register ();
        ++tcp_read_some_calls;
    }

    const fd_t fd = _socket->native_handle ();

#if defined(ZLINK_HAVE_WINDOWS)
    const int recv_len =
      len > static_cast<std::size_t> (0x7fffffff) ? 0x7fffffff : static_cast<int> (len);
    const int rc = ::recv (fd, reinterpret_cast<char *> (buffer), recv_len, 0);
    if (rc > 0) {
        errno = 0;
        if (tcp_stats_on)
            tcp_read_some_bytes += static_cast<std::size_t> (rc);
        return static_cast<std::size_t> (rc);
    }
    if (rc == 0) {
        errno = EPIPE;
        if (tcp_stats_on)
            ++tcp_read_some_errors;
        return 0;
    }

    const int werr = WSAGetLastError ();
    if (werr == WSAEWOULDBLOCK) {
        errno = EAGAIN;
        if (tcp_stats_on)
            ++tcp_read_some_eagain;
        return 0;
    }

    if (werr == WSAECONNRESET || werr == WSAESHUTDOWN)
        errno = EPIPE;
    else if (werr == WSAENOTCONN)
        errno = ENOTCONN;
    else if (werr == WSAENOTSOCK)
        errno = EBADF;
    else
        errno = EIO;

    if (tcp_stats_on)
        ++tcp_read_some_errors;
    return 0;
#else
    ssize_t rc = 0;
    do {
#ifdef MSG_DONTWAIT
        rc = ::recv (fd, buffer, len, MSG_DONTWAIT);
#else
        rc = ::recv (fd, buffer, len, 0);
#endif
    } while (rc == -1 && errno == EINTR);

    if (rc > 0) {
        if (tcp_stats_on)
            tcp_read_some_bytes += static_cast<std::size_t> (rc);
        return static_cast<std::size_t> (rc);
    }

    if (rc == 0) {
        errno = EPIPE;
        if (tcp_stats_on)
            ++tcp_read_some_errors;
        return 0;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        errno = EAGAIN;
        if (tcp_stats_on)
            ++tcp_read_some_eagain;
        return 0;
    }

    if (tcp_stats_on)
        ++tcp_read_some_errors;
    return 0;
#endif
}

void tcp_transport_t::async_write_some (const unsigned char *buffer,
                                        std::size_t buffer_size,
                                        completion_handler_t handler)
{
    if (tcp_stats_on) {
        tcp_stats_maybe_register ();
        ++tcp_async_write_calls;
    }

    if (_socket) {
        const std::shared_ptr<boost::asio::ip::tcp::socket> socket = _socket;
        if (tcp_stats_on) {
            const auto stats_handler = [handler, socket] (const boost::system::error_code &ec,
                                                          std::size_t bytes) {
                LIBZLINK_UNUSED (socket);
                if (ec)
                    ++tcp_async_write_errors;
                else
                    tcp_async_write_bytes += bytes;
                if (handler)
                    handler (ec, bytes);
            };
            if (tcp_use_async_write_some_on) {
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
            if (tcp_use_async_write_some_on) {
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

void tcp_transport_t::async_writev (const unsigned char *header,
                                    std::size_t header_size,
                                    const unsigned char *body,
                                    std::size_t body_size,
                                    completion_handler_t handler)
{
    if (tcp_stats_on) {
        tcp_stats_maybe_register ();
        ++tcp_async_write_calls;
    }

    if (!_socket) {
        if (handler)
            handler (boost::asio::error::bad_descriptor, 0);
        return;
    }
    const std::shared_ptr<boost::asio::ip::tcp::socket> socket = _socket;

    if (body_size == 0) {
        async_write_some (header, header_size, handler);
        return;
    }

    if (header_size == 0) {
        async_write_some (body, body_size, handler);
        return;
    }

#if defined(ZLINK_HAVE_WINDOWS)
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
            if (tcp_stats_on)
                ++tcp_async_write_errors;
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
                if (tcp_stats_on)
                    tcp_async_write_bytes += state->header_size + state->body_size;
                if (state->handler)
                    state->handler (boost::system::error_code (),
                                    state->header_size + state->body_size);
                return;
            }

            WSABUF buffers[2];
            DWORD buffer_count = 0;
            if (header_left > 0) {
                buffers[buffer_count].buf = reinterpret_cast<char *> (
                  const_cast<unsigned char *> (state->header + state->header_sent));
                buffers[buffer_count].len = static_cast<ULONG> (
                  std::min (header_left, static_cast<size_t> (std::numeric_limits<ULONG>::max ())));
                ++buffer_count;
            }
            if (body_left > 0) {
                buffers[buffer_count].buf = reinterpret_cast<char *> (
                  const_cast<unsigned char *> (state->body + state->body_sent));
                buffers[buffer_count].len = static_cast<ULONG> (
                  std::min (body_left, static_cast<size_t> (std::numeric_limits<ULONG>::max ())));
                ++buffer_count;
            }

            DWORD bytes_sent = 0;
            const int rc = WSASend (socket->native_handle (), buffers, buffer_count, &bytes_sent, 0,
                                    NULL, NULL);
            if (rc == 0) {
                size_t remaining = static_cast<size_t> (bytes_sent);
                if (header_left > 0) {
                    const size_t advanced = std::min (header_left, remaining);
                    state->header_sent += advanced;
                    remaining -= advanced;
                }
                if (remaining > 0 && body_left > 0)
                    state->body_sent += std::min (body_left, remaining);

                if (bytes_sent == 0) {
                    socket->async_wait (
                      boost::asio::socket_base::wait_write,
                      [do_write, socket] (const boost::system::error_code &wec) {
                          LIBZLINK_UNUSED (socket);
                          (*do_write) (wec);
                      });
                    return;
                }
                continue;
            }

            const int wsa_error = WSAGetLastError ();
            if (wsa_error == WSAEINTR)
                continue;
            if (wsa_error == WSAEWOULDBLOCK || wsa_error == WSAENOBUFS) {
                socket->async_wait (
                  boost::asio::socket_base::wait_write,
                  [do_write, socket] (const boost::system::error_code &wec) {
                      LIBZLINK_UNUSED (socket);
                      (*do_write) (wec);
                  });
                return;
            }

            if (tcp_stats_on)
                ++tcp_async_write_errors;
            if (state->handler)
                state->handler (boost::system::error_code (
                                  wsa_error, boost::system::system_category ()),
                                state->header_sent + state->body_sent);
            return;
        }
    };

    (*do_write) (boost::system::error_code ());
#else
    if (tcp_use_asio_writev_on) {
        if (tcp_stats_on) {
            const auto stats_handler = [handler, socket] (const boost::system::error_code &ec,
                                                          std::size_t bytes) {
                LIBZLINK_UNUSED (socket);
                if (ec)
                    ++tcp_async_write_errors;
                else
                    tcp_async_write_bytes += bytes;
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
            if (tcp_stats_on)
                ++tcp_async_write_errors;
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
                if (tcp_stats_on)
                    tcp_async_write_bytes += (state->header_size + state->body_size);
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
                if (tcp_writev_single_shot_on) {
                    const size_t left = (state->header_size - state->header_sent)
                                        + (state->body_size - state->body_sent);
                    if (left == 0) {
                        if (tcp_stats_on)
                            tcp_async_write_bytes += (state->header_size + state->body_size);
                        if (state->handler)
                            state->handler (boost::system::error_code (),
                                            state->header_size + state->body_size);
                        return;
                    }
                    socket->async_wait (boost::asio::socket_base::wait_write,
                                        [do_write, socket] (const boost::system::error_code &wec) {
                                            LIBZLINK_UNUSED (socket);
                                            (*do_write) (wec);
                                        });
                    return;
                }
                continue;
            }
            if (rc == -1 && errno == EINTR)
                continue;
            if (rc == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
                socket->async_wait (boost::asio::socket_base::wait_write,
                                    [do_write, socket] (const boost::system::error_code &wec) {
                                        LIBZLINK_UNUSED (socket);
                                        (*do_write) (wec);
                                    });
                return;
            }

            boost::system::error_code werr (errno, boost::system::system_category ());
            if (tcp_stats_on)
                ++tcp_async_write_errors;
            if (state->handler)
                state->handler (werr, state->header_sent + state->body_sent);
            return;
        }
    };

    (*do_write) (boost::system::error_code ());
#endif
}

std::size_t tcp_transport_t::write_some (const std::uint8_t *data, std::size_t len)
{
    if (len == 0) {
        return 0;
    }

    boost::system::error_code ec;
    std::size_t bytes_written = 0;

    if (!_socket || !_socket->is_open ()) {
        errno = EBADF;
        return 0;
    }

    if (tcp_stats_on) {
        tcp_stats_maybe_register ();
        ++tcp_write_some_calls;
    }

#if defined ZLINK_HAVE_WINDOWS
    // Keep Windows STREAM speculative writes on the native send path so the
    // hot loop does not pay the Boost.Asio synchronous-write wrapper cost.
    if (tcp_use_native_send_on) {
        const std::size_t send_size =
          std::min (len, static_cast<std::size_t> (std::numeric_limits<int>::max ()));
        int rc;
        int wsa_error = 0;
        do {
            rc = ::send (_socket->native_handle (), reinterpret_cast<const char *> (data),
                         static_cast<int> (send_size), 0);
            if (rc == SOCKET_ERROR)
                wsa_error = WSAGetLastError ();
        } while (rc == SOCKET_ERROR && wsa_error == WSAEINTR);
        if (rc == SOCKET_ERROR) {
            if (wsa_error == WSAEWOULDBLOCK || wsa_error == WSAENOBUFS) {
                errno = EAGAIN;
                if (tcp_stats_on)
                    ++tcp_write_some_eagain;
                return 0;
            }
            if (wsa_error == WSAECONNRESET || wsa_error == WSAECONNABORTED
                || wsa_error == WSAESHUTDOWN) {
                errno = EPIPE;
            } else if (wsa_error == WSAENOTSOCK) {
                errno = EBADF;
            } else if (wsa_error == WSAENOTCONN) {
                errno = ENOTCONN;
            } else {
                errno = EIO;
            }
            if (tcp_stats_on)
                ++tcp_write_some_errors;
            return 0;
        }

        errno = 0;
        if (tcp_stats_on)
            tcp_write_some_bytes += static_cast<std::size_t> (rc);
        return static_cast<std::size_t> (rc);
    }
#endif

    //  Perform synchronous write_some on TCP socket
    bytes_written = _socket->write_some (boost::asio::buffer (data, len), ec);

    if (ec) {
        //  Handle would_block case
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
            errno = EAGAIN;
            if (tcp_stats_on)
                ++tcp_write_some_eagain;
            return 0;
        }

        //  Map boost error codes to errno
        if (ec == boost::asio::error::broken_pipe || ec == boost::asio::error::connection_reset) {
            errno = EPIPE;
        } else if (ec == boost::asio::error::not_connected) {
            errno = ENOTCONN;
        } else if (ec == boost::asio::error::bad_descriptor) {
            errno = EBADF;
        } else {
            errno = EIO;
        }
        if (tcp_stats_on)
            ++tcp_write_some_errors;
        return 0;
    }

    errno = 0;
    if (tcp_stats_on)
        tcp_write_some_bytes += bytes_written;
    return bytes_written;
}

bool tcp_transport_t::supports_speculative_write () const
{
    return tcp_allow_sync_write_on;
}

bool tcp_transport_t::supports_speculative_read () const
{
    return true;
}

} // namespace zlink

#endif // ZLINK_IOTHREAD_POLLER_USE_ASIO
