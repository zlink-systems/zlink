/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/http/http_host_service.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/http/http_request_pipeline.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#ifdef ZLINK_FRAMEWORK_HTTP_WITH_OPENSSL
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/ssl.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>

#include <netdb.h>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace zlink::framework::runtime
{
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace
{

bool is_would_block (const beast::error_code &ec) noexcept
{
    return ec == asio::error::would_block || ec == asio::error::try_again
           || ec.value () == EAGAIN;
}

} // namespace

class fd_stream_t
{
  public:
    using executor_type = asio::system_executor;

    explicit fd_stream_t (int fd) : _fd (fd) {}
    ~fd_stream_t () { close (); }

    fd_stream_t (const fd_stream_t &) = delete;
    fd_stream_t &operator= (const fd_stream_t &) = delete;

    fd_stream_t (fd_stream_t &&other) noexcept : _fd (std::exchange (other._fd, -1)) {}

    fd_stream_t &operator= (fd_stream_t &&other) noexcept
    {
        if (this != &other) {
            close ();
            _fd = std::exchange (other._fd, -1);
        }
        return *this;
    }

    template <typename MutableBufferSequence>
    std::size_t read_some (const MutableBufferSequence &buffers)
    {
        beast::error_code ec;
        const auto read = read_some (buffers, ec);
        if (ec) {
            throw boost::system::system_error (ec);
        }
        return read;
    }

    template <typename MutableBufferSequence>
    std::size_t read_some (const MutableBufferSequence &buffers, beast::error_code &ec)
    {
        ec.clear ();
        for (auto it = asio::buffer_sequence_begin (buffers);
             it != asio::buffer_sequence_end (buffers); ++it) {
            auto buffer = *it;
            if (asio::buffer_size (buffer) == 0) {
                continue;
            }
            const auto received =
              //  `asio::buffer_cast`는 Boost 1.87에서 제거됐다. mutable_buffer의
              //  `data()`가 같은 포인터를 돌려주므로 그것을 쓴다.
              ::recv (_fd, buffer.data (), asio::buffer_size (buffer), 0);
            if (received > 0) {
                return static_cast<std::size_t> (received);
            }
            if (received == 0) {
                ec = asio::error::eof;
                return 0;
            }
            ec = boost::system::error_code (errno, boost::system::generic_category ());
            return 0;
        }
        return 0;
    }

    template <typename ConstBufferSequence>
    std::size_t write_some (const ConstBufferSequence &buffers)
    {
        beast::error_code ec;
        const auto written = write_some (buffers, ec);
        if (ec) {
            throw boost::system::system_error (ec);
        }
        return written;
    }

    template <typename ConstBufferSequence>
    std::size_t write_some (const ConstBufferSequence &buffers, beast::error_code &ec)
    {
        ec.clear ();
        for (auto it = asio::buffer_sequence_begin (buffers);
             it != asio::buffer_sequence_end (buffers); ++it) {
            auto buffer = *it;
            if (asio::buffer_size (buffer) == 0) {
                continue;
            }
#ifdef MSG_NOSIGNAL
            constexpr int send_flags = MSG_NOSIGNAL;
#else
            constexpr int send_flags = 0;
#endif
            const auto sent = ::send (_fd, buffer.data (),
                                      asio::buffer_size (buffer), send_flags);
            if (sent >= 0) {
                return static_cast<std::size_t> (sent);
            }
            ec = boost::system::error_code (errno, boost::system::generic_category ());
            return 0;
        }
        return 0;
    }

    void expires_after (std::chrono::milliseconds timeout) noexcept
    {
        timeval value{};
        value.tv_sec = static_cast<long> (timeout.count () / 1000);
        value.tv_usec = static_cast<long> ((timeout.count () % 1000) * 1000);
        (void) ::setsockopt (_fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof (value));
        (void) ::setsockopt (_fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof (value));
    }

    void shutdown_send () noexcept
    {
        if (_fd >= 0) {
            (void) ::shutdown (_fd, SHUT_WR);
        }
    }

    int fd () const noexcept { return _fd; }

    executor_type get_executor () noexcept { return {}; }

  private:
    void close () noexcept
    {
        if (_fd >= 0) {
            (void) ::close (_fd);
            _fd = -1;
        }
    }

    int _fd = -1;
};

class http_host_service_t::listener_t
{
  public:
    listener_t (const http_endpoint_t &endpoint,
                const http_options_snapshot_t &options,
                health_builder_t &health,
                service_provider_t &services,
                std::size_t handler_worker_count,
                std::atomic_bool &stop) :
        _endpoint (&endpoint),
        _options (&options),
        _health (&health),
        _services (&services),
        _stop (&stop),
        _active_connections (0),
        _active_requests (0),
        _handler_executor (
          0,
          std::max<std::size_t> (
            1, handler_worker_count == 0 ? std::thread::hardware_concurrency ()
                                         : handler_worker_count),
          1024,
          std::chrono::seconds (30),
          "zlink-http-hdl"),
        _connection_workers (
          0,
          std::max<std::size_t> (2, std::thread::hardware_concurrency ()),
          1024,
          std::chrono::seconds (30),
          "zlink-http-conn")
    {
    }

    ~listener_t () { stop_workers (); }

    void run ()
    {
        try {
            _parsed = parse_http_endpoint (_endpoint->uri);
            _listen_fd = open_listener ();
            configure_tls_context ();
        }
        catch (const std::exception &) {
            if (_stop->load (std::memory_order_acquire)) {
                return;
            }
            throw;
        }

        while (!_stop->load (std::memory_order_acquire)) {
            const int fd = ::accept (_listen_fd, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                continue;
            }
            if (_stop->load (std::memory_order_acquire)) {
                (void) ::close (fd);
                break;
            }
            if (_active_connections.load (std::memory_order_acquire)
                >= _options->server.max_connections) {
                reject_overloaded (fd);
                continue;
            }
            _active_connections.fetch_add (1, std::memory_order_acq_rel);
            _connection_workers.submit ([this, fd] () mutable {
                auto guard = std::unique_ptr<void, void (*) (void *)> (this, [] (void *listener) {
                    static_cast<listener_t *> (listener)->_active_connections.fetch_sub (
                      1, std::memory_order_acq_rel);
                });
                if (_parsed.scheme == "https") {
                    handle_https (fd);
                } else {
                    handle_http (fd);
                }
            });
        }
        if (_listen_fd >= 0) {
            (void) ::close (_listen_fd);
            _listen_fd = -1;
        }
    }

    void stop () noexcept
    {
        wake_acceptor ();
        close_open_connections ();
    }

    void stop_after_accept_loop () noexcept
    {
        (void) wait_for_active_requests (_options->server.graceful_shutdown_timeout);
        close_open_connections ();
        wait_for_workers ();
    }

  private:
    int open_listener ()
    {
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        hints.ai_flags = AI_PASSIVE;
        addrinfo *addresses = nullptr;
        const int resolved =
          ::getaddrinfo (_parsed.host.c_str (), _parsed.port.c_str (), &hints, &addresses);
        if (resolved != 0) {
            throw std::runtime_error ("HTTP endpoint address resolution failed");
        }

        int listen_fd = -1;
        int last_errno = 0;
        for (addrinfo *address = addresses; address != nullptr; address = address->ai_next) {
            listen_fd =
              ::socket (address->ai_family, address->ai_socktype, address->ai_protocol);
            if (listen_fd < 0) {
                last_errno = errno;
                continue;
            }
            const int reuse = 1;
            (void) ::setsockopt (listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof (reuse));
            if (::bind (listen_fd, address->ai_addr, address->ai_addrlen) == 0
                && ::listen (listen_fd, SOMAXCONN) == 0) {
                break;
            }
            last_errno = errno;
            (void) ::close (listen_fd);
            listen_fd = -1;
        }
        ::freeaddrinfo (addresses);
        if (listen_fd < 0) {
            throw std::runtime_error ("HTTP endpoint bind failed errno="
                                      + std::to_string (last_errno) + " "
                                      + std::strerror (last_errno));
        }
        return listen_fd;
    }

    void configure_tls_context ()
    {
#ifdef ZLINK_FRAMEWORK_HTTP_WITH_OPENSSL
        if (_parsed.scheme != "https") {
            return;
        }
        _tls_context.emplace (asio::ssl::context::tls_server);
        _tls_context->use_certificate_chain_file (_endpoint->tls->certificate_file);
        _tls_context->use_private_key_file (_endpoint->tls->private_key_file,
                                            asio::ssl::context::pem);
#endif
    }

    void wake_acceptor () noexcept
    {
        if (_parsed.host.empty () || _parsed.port.empty ()) {
            return;
        }
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        addrinfo *addresses = nullptr;
        if (::getaddrinfo (_parsed.host.c_str (), _parsed.port.c_str (), &hints, &addresses) != 0) {
            return;
        }
        for (addrinfo *address = addresses; address != nullptr; address = address->ai_next) {
            const int fd = ::socket (address->ai_family, address->ai_socktype, address->ai_protocol);
            if (fd < 0) {
                continue;
            }
            const int connected = ::connect (fd, address->ai_addr, address->ai_addrlen);
            ::close (fd);
            if (connected == 0) {
                break;
            }
        }
        ::freeaddrinfo (addresses);
    }

    void stop_workers () noexcept { stop_and_join_workers (); }

    void wait_for_workers () noexcept { stop_and_join_workers (); }

    void stop_and_join_workers () noexcept
    {
        std::lock_guard lock (_worker_stop_mutex);
        if (_workers_stopped.load (std::memory_order_acquire)) {
            return;
        }
        _workers_stopped.store (true, std::memory_order_release);
        _connection_workers.drain ();
    }

    bool wait_for_active_requests (std::chrono::milliseconds timeout) const noexcept
    {
        if (timeout <= std::chrono::milliseconds::zero ()) {
            return _active_requests.load (std::memory_order_acquire) == 0;
        }
        const auto deadline = std::chrono::steady_clock::now () + timeout;
        while (_active_requests.load (std::memory_order_acquire) != 0
               && std::chrono::steady_clock::now () < deadline) {
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
        return _active_requests.load (std::memory_order_acquire) == 0;
    }

    // Keep-alive clients hold connections open between requests, and the
    // worker's synchronous read cannot be cancelled by a timer. Stop must
    // shut the open sockets down so blocked workers unblock and join.
    void close_open_connections () noexcept
    {
        const std::lock_guard<std::mutex> lock (_sockets_mutex);
        for (auto fd : _sockets) {
            (void) ::shutdown (fd, SHUT_RDWR);
        }
    }

    class connection_registration_t
    {
      public:
        connection_registration_t (listener_t &listener, int fd) :
            _listener (listener), _fd (fd)
        {
            const std::lock_guard<std::mutex> lock (_listener._sockets_mutex);
            _listener._sockets.insert (_fd);
            if (_listener._stop->load (std::memory_order_acquire)) {
                (void) ::shutdown (_fd, SHUT_RDWR);
            }
        }

        ~connection_registration_t ()
        {
            const std::lock_guard<std::mutex> lock (_listener._sockets_mutex);
            _listener._sockets.erase (_fd);
        }

        connection_registration_t (const connection_registration_t &) = delete;
        connection_registration_t &operator= (const connection_registration_t &) = delete;

      private:
        listener_t &_listener;
        int _fd;
    };

    void reject_overloaded (int fd)
    {
        if (_parsed.scheme != "http") {
            (void) ::shutdown (fd, SHUT_RDWR);
            (void) ::close (fd);
            return;
        }
        fd_stream_t stream (fd);
        beast::error_code ec;
        auto response = make_http_status_response (http::status::service_unavailable, 11,
                                                   R"({"error":"server overloaded"})", false);
        http::write (stream, response, ec);
        stream.shutdown_send ();
    }

    void set_request_timeout (fd_stream_t &stream, std::chrono::milliseconds timeout)
    {
        stream.expires_after (timeout);
    }

#ifdef ZLINK_FRAMEWORK_HTTP_WITH_OPENSSL
    void set_request_timeout (beast::ssl_stream<beast::tcp_stream> &stream,
                              std::chrono::milliseconds timeout)
    {
        stream.next_layer ().expires_after (timeout);
    }
#endif

    template <typename TStream> bool serve_requests (TStream &stream)
    {
        beast::flat_buffer buffer;
        std::size_t served = 0;
        while (!_stop->load (std::memory_order_acquire)
               && served < _options->server.max_keep_alive_requests) {
            http::request_parser<http::string_body> parser;
            parser.body_limit (_options->server.max_request_body_size);
            parser.header_limit (_options->server.max_header_size);
            beast::error_code ec;
            set_request_timeout (stream, served == 0 ? _options->server.request_headers_timeout
                                                     : _options->server.keep_alive_timeout);
            http::read_header (stream, buffer, parser, ec);
            if (ec == http::error::end_of_stream || ec == asio::error::eof) {
                return true;
            }
            if (served > 0 && is_would_block (ec)) {
                return true;
            }
            if (ec) {
                auto response = make_http_parser_error_response (ec, 11);
                http::write (stream, response, ec);
                return false;
            }
            if (parser.content_length ()
                && *parser.content_length () > _options->server.max_request_body_size) {
                auto response =
                  make_http_status_response (http::status::payload_too_large, 11,
                                             R"({"error":"request body too large"})", false);
                http::write (stream, response, ec);
                return false;
            }
            set_request_timeout (stream, _options->server.request_body_timeout);
            http::read (stream, buffer, parser, ec);
            if (ec) {
                auto response = make_http_parser_error_response (ec, 11);
                http::write (stream, response, ec);
                return false;
            }
            auto request = parser.release ();
            _active_requests.fetch_add (1, std::memory_order_acq_rel);
            auto request_guard =
              std::unique_ptr<void, void (*) (void *)> (this, [] (void *listener) {
                  static_cast<listener_t *> (listener)->_active_requests.fetch_sub (
                    1, std::memory_order_acq_rel);
              });
            auto response =
              handle_http_request (*_options, *_services, *_health, _handler_executor, request);
            request_guard.reset ();
            response.keep_alive (request.keep_alive ()
                                 && served + 1 < _options->server.max_keep_alive_requests
                                 && !_stop->load (std::memory_order_acquire));
            set_request_timeout (stream, _options->server.write_timeout);
            http::write (stream, response, ec);
            if (ec || !response.keep_alive ()) {
                return false;
            }
            ++served;
        }
        return true;
    }

    void handle_http (int fd)
    {
        fd_stream_t stream (fd);
        connection_registration_t registration (*this, stream.fd ());
        beast::error_code ec;
        serve_requests (stream);
        stream.shutdown_send ();
    }

    void handle_https (int fd)
    {
#ifdef ZLINK_FRAMEWORK_HTTP_WITH_OPENSSL
        if (!_tls_context) {
            (void) ::shutdown (fd, SHUT_RDWR);
            (void) ::close (fd);
            return;
        }
        sockaddr_storage local_address{};
        socklen_t local_address_size = sizeof (local_address);
        const int family =
          ::getsockname (fd, reinterpret_cast<sockaddr *> (&local_address), &local_address_size)
              == 0
            ? local_address.ss_family
            : AF_INET;

        asio::io_context io;
        tcp::socket socket (io);
        beast::error_code ec;
        socket.assign (family == AF_INET6 ? tcp::v6 () : tcp::v4 (), fd, ec);
        if (ec) {
            (void) ::shutdown (fd, SHUT_RDWR);
            (void) ::close (fd);
            return;
        }
        beast::ssl_stream<beast::tcp_stream> stream (std::move (socket), *_tls_context);
        connection_registration_t registration (*this, fd);
        set_request_timeout (stream, _options->server.request_headers_timeout);
        stream.handshake (asio::ssl::stream_base::server, ec);
        if (ec) {
            return;
        }
        serve_requests (stream);
        stream.shutdown (ec);
#else
        (void) ::shutdown (fd, SHUT_RDWR);
        (void) ::close (fd);
#endif
    }

    const http_endpoint_t *_endpoint;
    const http_options_snapshot_t *_options;
    health_builder_t *_health;
    service_provider_t *_services;
    std::atomic_bool *_stop;
    std::atomic_size_t _active_connections;
    std::atomic_size_t _active_requests;
    std::atomic_bool _workers_stopped{false};
    std::mutex _worker_stop_mutex;
    std::mutex _sockets_mutex;
    std::unordered_set<int> _sockets;
    parsed_http_endpoint_t _parsed;
    offload_executor_t _handler_executor;
    offload_executor_t _connection_workers;
    int _listen_fd = -1;
#ifdef ZLINK_FRAMEWORK_HTTP_WITH_OPENSSL
    std::optional<asio::ssl::context> _tls_context;
#endif
};

http_host_service_t::http_host_service_t (http_options_snapshot_t options,
                                          health_builder_t &health,
                                          std::size_t handler_worker_count) :
    _options (std::move (options)),
    _health (&health),
    _handler_worker_count (handler_worker_count)
{
}

http_host_service_t::~http_host_service_t () = default;

void http_host_service_t::start (service_provider_t &services)
{
    _stop.store (false, std::memory_order_release);
    for (const auto &endpoint : _options.endpoints) {
        auto listener =
          std::make_unique<listener_t> (endpoint, _options, *_health, services,
                                        _handler_worker_count, _stop);
        auto *raw = listener.get ();
        _listeners.push_back (std::move (listener));
        _threads.emplace_back ([raw] { raw->run (); });
    }
}

void http_host_service_t::request_stop () noexcept
{
    _stop.store (true, std::memory_order_release);
    for (auto &listener : _listeners) {
        listener->stop ();
    }
}

void http_host_service_t::stop () noexcept
{
    request_stop ();
    for (auto &thread : _threads) {
        if (thread.joinable ()) {
            thread.join ();
        }
    }
    for (auto &listener : _listeners) {
        listener->stop_after_accept_loop ();
    }
    _threads.clear ();
    _listeners.clear ();
}

} // namespace zlink::framework::runtime
