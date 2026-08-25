/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/http/http_host_service.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/http/http_request_pipeline.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#ifdef ZLINK_FRAMEWORK_HTTP_WITH_OPENSSL
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/ssl.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

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
          "zlink-http-conn"),
        _acceptor (_io),
        _accept_retry_timer (_io)
    {
    }

    ~listener_t () { stop_workers (); }

    void run ()
    {
        try {
            _parsed = parse_http_endpoint (_endpoint->uri);
            open_listener ();
            configure_tls_context ();
        }
        catch (const std::exception &) {
            if (_stop->load (std::memory_order_acquire)) {
                return;
            }
            throw;
        }

        start_accept ();
        _io.run ();
    }

    void stop () noexcept
    {
        close_open_connections ();
        asio::post (_io, [this] {
            beast::error_code ignored;
            _acceptor.cancel (ignored);
            _acceptor.close (ignored);
            _accept_retry_timer.cancel (ignored);
        });
    }

    void stop_after_accept_loop () noexcept
    {
        (void) wait_for_active_requests (_options->server.graceful_shutdown_timeout);
        close_open_connections ();
        wait_for_workers ();
    }

  private:
    void open_listener ()
    {
        tcp::resolver resolver (_io);
        beast::error_code error;
        const auto wildcard = _parsed.host == "*";
        const auto resolve_host = wildcard ? std::string () : _parsed.host;
        const auto resolve_flags = wildcard ? tcp::resolver::flags::passive
                                            : tcp::resolver::flags ();
        const auto endpoints = resolver.resolve (resolve_host, _parsed.port, resolve_flags, error);
        if (error || endpoints.begin () == endpoints.end ()) {
            throw std::runtime_error ("HTTP endpoint address resolution failed: "
                                      + (error ? error.message () : "no addresses"));
        }
        for (const auto &candidate : endpoints) {
            _acceptor.open (candidate.endpoint ().protocol (), error);
            if (!error) {
                _acceptor.set_option (tcp::acceptor::reuse_address (true), error);
            }
            if (!error) {
                _acceptor.bind (candidate.endpoint (), error);
            }
            if (!error) {
                _acceptor.listen (asio::socket_base::max_listen_connections, error);
            }
            if (!error) {
                return;
            }
            beast::error_code ignored;
            _acceptor.close (ignored);
        }
        throw std::runtime_error ("HTTP listener bind/listen failed: " + error.message ());
    }

    struct connection_t
    {
        connection_t () : socket (io) {}

        asio::io_context io;
        tcp::socket socket;
    };

    void start_accept ()
    {
        if (_stop->load (std::memory_order_acquire)) {
            return;
        }
        auto connection = std::make_shared<connection_t> ();
        _acceptor.async_accept (connection->socket,
                                [this, connection] (beast::error_code error) {
            if (!error && !_stop->load (std::memory_order_acquire)) {
                if (_active_connections.load (std::memory_order_acquire)
                    >= _options->server.max_connections) {
                    reject_overloaded (std::move (connection->socket));
                } else {
                    _active_connections.fetch_add (1, std::memory_order_acq_rel);
                    try {
                        _connection_workers.submit ([this, connection] () mutable {
                            auto guard = std::unique_ptr<void, void (*) (void *)> (
                              this, [] (void *listener) {
                                  static_cast<listener_t *> (listener)->_active_connections.fetch_sub (
                                    1, std::memory_order_acq_rel);
                              });
                            if (_parsed.scheme == "https") {
                                handle_https (std::move (connection));
                            } else {
                                handle_http (std::move (connection));
                            }
                        });
                    }
                    catch (...) {
                        _active_connections.fetch_sub (1, std::memory_order_acq_rel);
                        beast::error_code ignored;
                        connection->socket.close (ignored);
                    }
                }
            }
            if (!_stop->load (std::memory_order_acquire)
                && error != asio::error::operation_aborted) {
                if (error) {
                    _accept_retry_timer.expires_after (std::chrono::milliseconds (100));
                    _accept_retry_timer.async_wait ([this] (beast::error_code retry_error) {
                        if (!retry_error) {
                            start_accept ();
                        }
                    });
                } else {
                    start_accept ();
                }
            }
        });
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

    // Keep-alive clients hold connections open between requests. Closing is
    // posted to the connection's executor so the stream object is not touched
    // concurrently by the shutdown thread and its I/O coroutine.
    void close_open_connections () noexcept
    {
        std::vector<std::function<void ()>> closers;
        {
            const std::lock_guard<std::mutex> lock (_sockets_mutex);
            closers.reserve (_sockets.size ());
            for (const auto &entry : _sockets) {
                closers.push_back (entry.second);
            }
        }
        for (auto &close : closers) {
            close ();
        }
    }

    class connection_registration_t
    {
      public:
        connection_registration_t (listener_t &listener,
                                   const std::shared_ptr<connection_t> &owner,
                                   beast::tcp_stream &stream,
                                   std::shared_ptr<void> stream_lifetime) :
            _listener (listener), _stream (&stream)
        {
            const std::weak_ptr<connection_t> weak_owner = owner;
            const std::weak_ptr<void> weak_stream_lifetime = stream_lifetime;
            const auto close = [weak_owner, weak_stream_lifetime, stream_ptr = _stream] {
                const auto owner = weak_owner.lock ();
                if (!owner || weak_stream_lifetime.expired ()) {
                    return;
                }
                asio::post (owner->io, [weak_owner, weak_stream_lifetime, stream_ptr] {
                    if (weak_owner.expired () || weak_stream_lifetime.expired ()) {
                        return;
                    }
                    beast::error_code ignored;
                    stream_ptr->socket ().cancel (ignored);
                    stream_ptr->socket ().close (ignored);
                });
            };
            bool close_now = false;
            {
                const std::lock_guard<std::mutex> lock (_listener._sockets_mutex);
                _listener._sockets.emplace (_stream, close);
                close_now = _listener._stop->load (std::memory_order_acquire);
            }
            if (close_now) {
                close ();
            }
        }

        ~connection_registration_t ()
        {
            const std::lock_guard<std::mutex> lock (_listener._sockets_mutex);
            _listener._sockets.erase (_stream);
        }

        connection_registration_t (const connection_registration_t &) = delete;
        connection_registration_t &operator= (const connection_registration_t &) = delete;

      private:
        listener_t &_listener;
        beast::tcp_stream *_stream;
    };

    void close_connection (beast::tcp_stream *stream) noexcept
    {
        std::function<void ()> close;
        {
            const std::lock_guard<std::mutex> lock (_sockets_mutex);
            const auto found = _sockets.find (stream);
            if (found != _sockets.end ()) {
                close = found->second;
            }
        }
        if (close) {
            close ();
        }
    }

    void reject_overloaded (tcp::socket socket)
    {
        if (_parsed.scheme != "http") {
            beast::error_code ignored;
            socket.shutdown (tcp::socket::shutdown_both, ignored);
            socket.close (ignored);
            return;
        }
        beast::tcp_stream stream (std::move (socket));
        beast::error_code ec;
        auto response = make_http_status_response (http::status::service_unavailable, 11,
                                                   R"({"error":"server overloaded"})", false);
        http::write (stream, response, ec);
        stream.socket ().shutdown (tcp::socket::shutdown_send, ec);
    }

    void set_request_timeout (beast::tcp_stream &stream, std::chrono::milliseconds timeout)
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

    template <typename TStream> asio::awaitable<bool> serve_requests_async (TStream &stream)
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
            (void) co_await http::async_read_header (
              stream, buffer, parser,
              asio::redirect_error (asio::use_awaitable, ec));
            if (ec == http::error::end_of_stream || ec == asio::error::eof) {
                co_return true;
            }
            if (ec) {
                auto response = make_http_parser_error_response (ec, 11);
                set_request_timeout (stream, _options->server.write_timeout);
                (void) co_await http::async_write (
                  stream, response, asio::redirect_error (asio::use_awaitable, ec));
                co_return false;
            }
            if (parser.content_length ()
                && *parser.content_length () > _options->server.max_request_body_size) {
                auto response =
                  make_http_status_response (http::status::payload_too_large, 11,
                                             R"({"error":"request body too large"})", false);
                set_request_timeout (stream, _options->server.write_timeout);
                (void) co_await http::async_write (
                  stream, response, asio::redirect_error (asio::use_awaitable, ec));
                co_return false;
            }
            set_request_timeout (stream, _options->server.request_body_timeout);
            (void) co_await http::async_read (
              stream, buffer, parser, asio::redirect_error (asio::use_awaitable, ec));
            if (ec) {
                auto response = make_http_parser_error_response (ec, 11);
                set_request_timeout (stream, _options->server.write_timeout);
                (void) co_await http::async_write (
                  stream, response, asio::redirect_error (asio::use_awaitable, ec));
                co_return false;
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
            (void) co_await http::async_write (
              stream, response, asio::redirect_error (asio::use_awaitable, ec));
            if (ec || !response.keep_alive ()) {
                co_return false;
            }
            ++served;
        }
        co_return true;
    }

    void handle_http (std::shared_ptr<connection_t> connection)
    {
        asio::co_spawn (
          connection->io,
          [this, connection] () -> asio::awaitable<void> {
              auto stream = std::make_shared<beast::tcp_stream> (std::move (connection->socket));
              connection_registration_t registration (*this, connection, *stream, stream);
              (void) co_await serve_requests_async (*stream);
              beast::error_code ignored;
              stream->socket ().shutdown (tcp::socket::shutdown_send, ignored);
              co_return;
          },
          asio::detached);
        connection->io.run ();
    }

    void handle_https (std::shared_ptr<connection_t> connection)
    {
#ifdef ZLINK_FRAMEWORK_HTTP_WITH_OPENSSL
        if (!_tls_context) {
            beast::error_code ignored;
            connection->socket.close (ignored);
            return;
        }
        asio::co_spawn (
          connection->io,
          [this, connection] () -> asio::awaitable<void> {
              auto stream = std::make_shared<beast::ssl_stream<beast::tcp_stream>> (
                std::move (connection->socket), *_tls_context);
              connection_registration_t registration (*this, connection,
                                                       stream->next_layer (), stream);
              beast::error_code ec;
              set_request_timeout (*stream, _options->server.request_headers_timeout);
              co_await stream->async_handshake (
                asio::ssl::stream_base::server,
                asio::redirect_error (asio::use_awaitable, ec));
              if (!ec) {
                  (void) co_await serve_requests_async (*stream);
              }
              co_await stream->async_shutdown (
                asio::redirect_error (asio::use_awaitable, ec));
              co_return;
          },
          asio::detached);
        connection->io.run ();
#else
        beast::error_code ignored;
        connection->socket.close (ignored);
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
    std::unordered_map<beast::tcp_stream *, std::function<void ()>> _sockets;
    parsed_http_endpoint_t _parsed;
    offload_executor_t _handler_executor;
    offload_executor_t _connection_workers;
    asio::io_context _io;
    tcp::acceptor _acceptor;
    asio::steady_timer _accept_retry_timer;
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

task_t<void> http_host_service_t::start (service_provider_t &services)
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
    return task_t<void> (result_t<void>::success ());
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
