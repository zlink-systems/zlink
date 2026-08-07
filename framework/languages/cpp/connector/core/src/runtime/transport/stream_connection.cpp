/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/transport/stream_connection.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <openssl/ssl.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

namespace zlink::stream_connector::detail
{

namespace
{

class readable_wait_state_t final
{
  private:
    enum class handler_kind_t
    {
        timer,
        socket
    };

  public:
    readable_wait_state_t (
      boost::asio::io_context &io_context,
      std::chrono::steady_clock::time_point deadline) :
        timer (io_context), deadline (deadline)
    {
    }

    void report_timer (boost::system::error_code error_, bool readable_) noexcept
    {
        report (handler_kind_t::timer, error_, readable_);
    }

    void report_socket (boost::system::error_code error_, bool readable_) noexcept
    {
        report (handler_kind_t::socket, error_, readable_);
    }

    void report (handler_kind_t handler,
                 boost::system::error_code error_,
                 bool readable_) noexcept
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            bool &reported = handler == handler_kind_t::timer ? timer_reported : socket_reported;
            if (reported) {
                return;
            }
            reported = true;
            if (!result_ready) {
                result_ready = true;
                error = error_;
                readable = readable_;
            }
            if (handlers_remaining > 0) {
                --handlers_remaining;
            }
            if (handlers_remaining == 0) {
                completed = true;
            }
        }
        condition.notify_all ();
    }

    std::tuple<boost::system::error_code, bool> wait ()
    {
        // Both handlers must report before returning because each handler
        // captures the connection's socket by reference.
        std::unique_lock<std::mutex> lock (mutex);
        condition.wait (lock, [this] { return completed; });
        return {error, readable};
    }

    boost::asio::steady_timer timer;
    boost::asio::cancellation_signal wait_cancellation;
    std::chrono::steady_clock::time_point deadline;

  private:
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t handlers_remaining = 2;
    bool timer_reported = false;
    bool socket_reported = false;
    bool result_ready = false;
    bool completed = false;
    bool readable = false;
    boost::system::error_code error;
};

template <typename Socket, typename Strand>
bool wait_readable_until_asio (boost::asio::io_context &io_context,
                               Strand &strand,
                               Socket &socket,
                               std::chrono::steady_clock::time_point deadline,
                               boost::system::error_code &error)
{
    if (io_context.stopped ()) {
        error = boost::asio::error::operation_aborted;
        return false;
    }
    if (std::chrono::steady_clock::now () >= deadline) {
        error.clear ();
        return false;
    }

    // A synchronous receive may be called from a callback already executing
    // on this strand. Posting the wait and then blocking would prevent the
    // posted handler from running. available() is the non-blocking Asio query
    // for that context, so bounded polling preserves the synchronous API
    // without introducing an operating-system-specific transport path.
    if (strand.running_in_this_thread ()) {
        for (;;) {
            const auto now = std::chrono::steady_clock::now ();
            if (now >= deadline) {
                error.clear ();
                return false;
            }
            const auto available = socket.available (error);
            if (error || available != 0) {
                return available != 0;
            }
            const auto remaining = deadline - now;
            std::this_thread::sleep_for (
              std::min<std::chrono::steady_clock::duration> (
                remaining, std::chrono::milliseconds (1)));
        }
    }

    auto state = std::make_shared<readable_wait_state_t> (io_context, deadline);
    try {
        boost::asio::post (strand, [state, &socket, &strand] {
            state->timer.expires_at (state->deadline);
            state->timer.async_wait (boost::asio::bind_executor (
              strand, [state] (const boost::system::error_code &timer_error) {
                  if (timer_error != boost::asio::error::operation_aborted) {
                      // Cancel only this readiness wait. socket.cancel() would
                      // also abort unrelated reads and writes on the same
                      // connection.
                      state->wait_cancellation.emit (
                        boost::asio::cancellation_type::terminal);
                  }
                  state->report_timer (timer_error == boost::asio::error::operation_aborted
                                         ? boost::system::error_code{}
                                         : timer_error,
                                       false);
              }));
            socket.async_wait (
              boost::asio::ip::tcp::socket::wait_read,
              boost::asio::bind_cancellation_slot (
                state->wait_cancellation.slot (),
                boost::asio::bind_executor (
                  strand, [state] (const boost::system::error_code &socket_error) {
                      boost::system::error_code ignored;
                      state->timer.cancel (ignored);
                      state->report_socket (socket_error, !socket_error);
                  })));
        });
    }
    catch (const boost::system::system_error &exception) {
        error = exception.code ();
        return false;
    }
    catch (const std::exception &) {
        error = boost::asio::error::operation_aborted;
        return false;
    }

    auto [wait_error, readable] = state->wait ();
    error = wait_error;
    return readable;
}

class tcp_stream_connection_t final : public stream_connection_t
{
  public:
    explicit tcp_stream_connection_t (boost::asio::io_context &io_context,
                                     boost::asio::ip::tcp::socket socket) :
        _io_context (io_context),
        _socket (std::move (socket)),
        _strand (io_context.get_executor ())
    {
    }

    bool is_open () const override
    {
        return run_serialized_sync (_io_context, _strand, [this] { return _socket.is_open (); });
    }

    std::size_t available (boost::system::error_code &error) override
    {
        return run_serialized_sync (
          _io_context, _strand, [this, &error] { return _socket.available (error); });
    }

    std::size_t
    read_some (std::uint8_t *buffer, std::size_t size, boost::system::error_code &error) override
    {
        return run_serialized_sync (_io_context, _strand, [this, buffer, size, &error] {
            return _socket.read_some (boost::asio::buffer (buffer, size), error);
        });
    }

    void async_read_some (
      std::size_t max_size,
      std::function<void (boost::system::error_code, std::vector<std::uint8_t>)> completion) override
    {
        auto buffer = std::make_shared<std::vector<std::uint8_t>> (max_size);
        boost::asio::post (
          _strand,
          [this, buffer, completion = std::move (completion)] () mutable {
              _socket.async_read_some (
                boost::asio::buffer (*buffer),
                boost::asio::bind_executor (
                  _strand,
                  [buffer, completion = std::move (completion)] (
                    boost::system::error_code error, std::size_t bytes_read) mutable {
                      buffer->resize (bytes_read);
                      if (completion) {
                          completion (error, std::move (*buffer));
                      }
                  }));
          });
    }

    bool wait_readable_until (std::chrono::steady_clock::time_point deadline,
                              boost::system::error_code &error) override
    {
        return wait_readable_until_asio (_io_context, _strand, _socket, deadline, error);
    }

    void write (const std::vector<std::uint8_t> &bytes) override
    {
        run_serialized_sync (_io_context, _strand, [this, &bytes] {
            boost::asio::write (_socket, boost::asio::buffer (bytes));
        });
    }

    void async_write (std::vector<std::uint8_t> bytes,
                      std::function<void (boost::system::error_code)> completion) override
    {
        auto buffer = std::make_shared<std::vector<std::uint8_t>> (std::move (bytes));
        boost::asio::post (
          _strand,
          [this, buffer, completion = std::move (completion)] () mutable {
              boost::asio::async_write (
                _socket, boost::asio::buffer (*buffer),
                boost::asio::bind_executor (
                  _strand,
                  [buffer, completion = std::move (completion)] (
                    boost::system::error_code error, std::size_t) mutable {
                      if (completion) {
                          completion (error);
                      }
                  }));
          });
    }

    void shutdown_and_close () override
    {
        run_serialized_sync (_io_context, _strand, [this] {
            boost::system::error_code ignored;
            _socket.shutdown (boost::asio::ip::tcp::socket::shutdown_both, ignored);
            _socket.close (ignored);
        });
    }

    void shutdown_and_close_async () override
    {
        std::shared_ptr<stream_connection_t> self;
        try {
            self = shared_from_this ();
        }
        catch (const std::bad_weak_ptr &) {
            shutdown_and_close ();
            return;
        }
        boost::asio::post (_strand, [this, self] {
            boost::system::error_code ignored;
            _socket.shutdown (boost::asio::ip::tcp::socket::shutdown_both, ignored);
            _socket.close (ignored);
        });
    }

    void close (boost::system::error_code &error) override
    {
        run_serialized_sync (_io_context, _strand, [this, &error] { _socket.close (error); });
    }

  private:
    boost::asio::io_context &_io_context;
    boost::asio::ip::tcp::socket _socket;
    boost::asio::strand<boost::asio::io_context::executor_type> _strand;
};

#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
namespace ssl = boost::asio::ssl;

class tls_stream_connection_t final : public stream_connection_t
{
  public:
    explicit tls_stream_connection_t (ssl::stream<boost::asio::ip::tcp::socket> stream,
                                     boost::asio::io_context &io_context,
                                     std::shared_ptr<ssl::context> context) :
        _context (std::move (context)),
        _io_context (io_context),
        _stream (std::move (stream)),
        _strand (io_context.get_executor ())
    {
    }

    bool is_open () const override
    {
        return run_serialized_sync (
          _io_context, _strand, [this] { return _stream.next_layer ().is_open (); });
    }

    std::size_t available (boost::system::error_code &error) override
    {
        return run_serialized_sync (
          _io_context, _strand,
          [this, &error] { return _stream.next_layer ().available (error); });
    }

    std::size_t
    read_some (std::uint8_t *buffer, std::size_t size, boost::system::error_code &error) override
    {
        return run_serialized_sync (_io_context, _strand, [this, buffer, size, &error] {
            return _stream.read_some (boost::asio::buffer (buffer, size), error);
        });
    }

    void async_read_some (
      std::size_t max_size,
      std::function<void (boost::system::error_code, std::vector<std::uint8_t>)> completion) override
    {
        auto buffer = std::make_shared<std::vector<std::uint8_t>> (max_size);
        boost::asio::post (
          _strand,
          [this, buffer, completion = std::move (completion)] () mutable {
              _stream.async_read_some (
                boost::asio::buffer (*buffer),
                boost::asio::bind_executor (
                  _strand,
                  [buffer, completion = std::move (completion)] (
                    boost::system::error_code error, std::size_t bytes_read) mutable {
                      buffer->resize (bytes_read);
                      if (completion) {
                          completion (error, std::move (*buffer));
                      }
                  }));
          });
    }

    bool wait_readable_until (std::chrono::steady_clock::time_point deadline,
                              boost::system::error_code &error) override
    {
        return wait_readable_until_asio (
          _io_context, _strand, _stream.next_layer (), deadline, error);
    }

    void write (const std::vector<std::uint8_t> &bytes) override
    {
        run_serialized_sync (_io_context, _strand, [this, &bytes] {
            boost::asio::write (_stream, boost::asio::buffer (bytes));
        });
    }

    void async_write (std::vector<std::uint8_t> bytes,
                      std::function<void (boost::system::error_code)> completion) override
    {
        auto buffer = std::make_shared<std::vector<std::uint8_t>> (std::move (bytes));
        boost::asio::post (
          _strand,
          [this, buffer, completion = std::move (completion)] () mutable {
              boost::asio::async_write (
                _stream, boost::asio::buffer (*buffer),
                boost::asio::bind_executor (
                  _strand,
                  [buffer, completion = std::move (completion)] (
                    boost::system::error_code error, std::size_t) mutable {
                      if (completion) {
                          completion (error);
                      }
                  }));
          });
    }

    void shutdown_and_close () override
    {
        run_serialized_sync (_io_context, _strand, [this] {
            boost::system::error_code ignored;
            _stream.next_layer ().shutdown (boost::asio::ip::tcp::socket::shutdown_both, ignored);
            _stream.next_layer ().close (ignored);
        });
    }

    void shutdown_and_close_async () override
    {
        std::shared_ptr<stream_connection_t> self;
        try {
            self = shared_from_this ();
        }
        catch (const std::bad_weak_ptr &) {
            shutdown_and_close ();
            return;
        }
        boost::asio::post (_strand, [this, self] {
            boost::system::error_code ignored;
            _stream.next_layer ().shutdown (boost::asio::ip::tcp::socket::shutdown_both, ignored);
            _stream.next_layer ().close (ignored);
        });
    }

    void close (boost::system::error_code &error) override
    {
        run_serialized_sync (_io_context, _strand,
                             [this, &error] { _stream.next_layer ().close (error); });
    }

  private:
    // ssl::stream stores a reference to its context, so the connection must
    // retain that context until the stream has been destroyed.
    std::shared_ptr<ssl::context> _context;
    boost::asio::io_context &_io_context;
    ssl::stream<boost::asio::ip::tcp::socket> _stream;
    boost::asio::strand<boost::asio::io_context::executor_type> _strand;
};
#endif

} // namespace

std::unique_ptr<stream_connection_t> make_tcp_connection (boost::asio::io_context &io_context,
                                                          boost::asio::ip::tcp::socket socket)
{
    return std::make_unique<tcp_stream_connection_t> (io_context, std::move (socket));
}

namespace
{

std::optional<endpoint_parts_t> parse_host_port_endpoint (const std::string &endpoint,
                                                          std::string_view prefix)
{
    if (endpoint.rfind (std::string (prefix), 0) != 0) {
        return std::nullopt;
    }
    const auto host_start = prefix.size ();
    const auto colon = endpoint.rfind (':');
    if (colon == std::string::npos || colon <= host_start || colon + 1 >= endpoint.size ()) {
        return std::nullopt;
    }
    return endpoint_parts_t{endpoint.substr (host_start, colon - host_start),
                            endpoint.substr (colon + 1)};
}

} // namespace

std::optional<endpoint_parts_t> parse_tcp_endpoint (const std::string &endpoint)
{
    return parse_host_port_endpoint (endpoint, "tcp://");
}

std::optional<endpoint_parts_t> parse_tls_endpoint (const std::string &endpoint)
{
    return parse_host_port_endpoint (endpoint, "tls://");
}

#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
std::unique_ptr<stream_connection_t> connect_tls (boost::asio::io_context &io_context,
                                                  const endpoint_parts_t &endpoint,
                                                  bool skip_server_certificate_validation)
{
    auto context = std::make_shared<ssl::context> (ssl::context::tls_client);
    context->set_default_verify_paths ();
    ssl::stream<boost::asio::ip::tcp::socket> stream (io_context, *context);
    SSL_set_tlsext_host_name (stream.native_handle (), endpoint.host.c_str ());
    if (skip_server_certificate_validation) {
        stream.set_verify_mode (ssl::verify_none);
    } else {
        stream.set_verify_mode (ssl::verify_peer);
        stream.set_verify_callback (ssl::host_name_verification (endpoint.host));
    }

    boost::asio::ip::tcp::resolver resolver (io_context);
    auto endpoints = resolver.resolve (endpoint.host, endpoint.port);
    boost::asio::connect (stream.next_layer (), endpoints);
    stream.handshake (ssl::stream_base::client);
    return std::make_unique<tls_stream_connection_t> (
      std::move (stream), io_context, std::move (context));
}

void connect_tls_async (
  boost::asio::io_context &io_context,
  endpoint_parts_t endpoint,
  bool skip_server_certificate_validation,
  std::shared_ptr<transport_connect_control_t> control,
  std::function<void (boost::system::error_code, std::unique_ptr<stream_connection_t>)> callback)
{
    auto context = std::make_shared<ssl::context> (ssl::context::tls_client);
    context->set_default_verify_paths ();
    auto stream =
      std::make_shared<ssl::stream<boost::asio::ip::tcp::socket>> (io_context, *context);
    SSL_set_tlsext_host_name (stream->native_handle (), endpoint.host.c_str ());
    if (skip_server_certificate_validation) {
        stream->set_verify_mode (ssl::verify_none);
    } else {
        stream->set_verify_mode (ssl::verify_peer);
        stream->set_verify_callback (ssl::host_name_verification (endpoint.host));
    }

    const auto host = endpoint.host;
    const auto port = endpoint.port;
    auto resolver = std::make_shared<boost::asio::ip::tcp::resolver> (io_context);
    control->set_cancel_handler ([resolver, stream] {
        boost::system::error_code ignored;
        resolver->cancel ();
        stream->next_layer ().cancel (ignored);
        stream->next_layer ().close (ignored);
    });
    resolver->async_resolve (
      host, port,
      [&io_context, resolver, context, stream, control, callback = std::move (callback)] (
        boost::system::error_code error,
        boost::asio::ip::tcp::resolver::results_type endpoints) mutable {
          if (control->cancelled ()) {
              callback (boost::asio::error::operation_aborted, nullptr);
              return;
          }
          if (error) {
              callback (error, nullptr);
              return;
          }
          boost::asio::async_connect (
            stream->next_layer (), endpoints,
            [&io_context, context, stream, control, callback = std::move (callback)] (
              boost::system::error_code connect_error,
              const boost::asio::ip::tcp::endpoint &) mutable {
                if (control->cancelled ()) {
                    callback (boost::asio::error::operation_aborted, nullptr);
                    return;
                }
                if (connect_error) {
                    callback (connect_error, nullptr);
                    return;
                }
                stream->async_handshake (
                  ssl::stream_base::client,
                  [&io_context, context, stream, control, callback = std::move (callback)] (
                    boost::system::error_code handshake_error) mutable {
                      if (control->cancelled ()) {
                          callback (boost::asio::error::operation_aborted, nullptr);
                          return;
                      }
                      if (handshake_error) {
                          callback (handshake_error, nullptr);
                          return;
                      }
                      callback ({}, std::make_unique<tls_stream_connection_t> (
                                      std::move (*stream), io_context, std::move (context)));
                  });
            });
      });
}
#endif

bool is_transport_connected (const connector_state_t &state)
{
    // The lifecycle state and connection pointer are protected by the caller's
    // transport/lifecycle lock. Calling the serialized transport query here
    // would pump the shared io_context while that lock is held and can run a
    // completion that needs the same lock.
    return state.state == connection_state_t::connected && state.connection != nullptr;
}

std::vector<std::uint8_t> read_exact (connector_state_t &state, std::size_t size)
{
    std::vector<std::uint8_t> bytes (size);
    std::size_t offset = 0;
    while (offset < size) {
        boost::system::error_code error;
        const auto read =
          state.connection->read_some (bytes.data () + offset, bytes.size () - offset, error);
        if (error) {
            throw boost::system::system_error (error);
        }
        offset += read;
    }
    return bytes;
}

void write_bytes (connector_state_t &state, const std::vector<std::uint8_t> &bytes)
{
    state.connection->write (bytes);
}

} // namespace zlink::stream_connector::detail
