/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/transport/websocket_connection.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <openssl/ssl.h>
#endif
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace zlink::stream_connector::detail
{

namespace
{

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
namespace ssl = asio::ssl;
#endif

template <typename TStream> class websocket_stream_connection_t final : public stream_connection_t
{
  public:
    explicit websocket_stream_connection_t (websocket::stream<TStream> stream,
                                            asio::io_context &io_context) :
        _io_context (io_context),
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
        _tls_context (),
#endif
        _stream (std::move (stream)),
        _strand (io_context.get_executor ())
    {
        _stream.binary (true);
    }

#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
    explicit websocket_stream_connection_t (websocket::stream<TStream> stream,
                                            asio::io_context &io_context,
                                            std::shared_ptr<ssl::context> context) :
        _io_context (io_context),
        _tls_context (std::move (context)),
        _stream (std::move (stream)),
        _strand (io_context.get_executor ())
    {
        _stream.binary (true);
    }
#endif

    bool is_open () const override
    {
        return run_serialized_sync (_io_context, _strand, [this] { return _stream.is_open (); });
    }

    std::size_t available (boost::system::error_code &error) override
    {
        return run_serialized_sync (
          _io_context, _strand, [this, &error] { return available_impl (error); });
    }

    std::size_t read_some (std::uint8_t *buffer,
                           std::size_t size,
                           boost::system::error_code &error) override
    {
        return run_serialized_sync (_io_context, _strand, [this, buffer, size, &error] {
            return read_some_impl (buffer, size, error);
        });
    }

    std::size_t available_impl (boost::system::error_code &error)
    {
        if (!_read_buffer.empty ()) {
            return _read_buffer.size () - _read_offset;
        }

        const auto tcp_available = beast::get_lowest_layer (_stream).available (error);
        if (error || tcp_available == 0) {
            return 0;
        }

        beast::flat_buffer buffer;
        _stream.read (buffer, error);
        if (error) {
            return 0;
        }

        _read_buffer.resize (buffer.size ());
        asio::buffer_copy (asio::buffer (_read_buffer), buffer.data ());
        _read_offset = 0;
        return _read_buffer.size ();
    }

    std::size_t read_some_impl (std::uint8_t *buffer,
                                std::size_t size,
                                boost::system::error_code &error)
    {
        if (available_impl (error) == 0 || error) {
            return 0;
        }

        const auto remaining = _read_buffer.size () - _read_offset;
        const auto copied = std::min (remaining, size);
        std::copy_n (_read_buffer.data () + _read_offset, copied, buffer);
        _read_offset += copied;
        if (_read_offset == _read_buffer.size ()) {
            _read_buffer.clear ();
            _read_offset = 0;
        }
        return copied;
    }

    void async_read_some (
      std::size_t max_size,
      std::function<void (boost::system::error_code, std::vector<std::uint8_t>)> completion) override
    {
        (void) max_size;
        auto buffer = std::make_shared<beast::flat_buffer> ();
        asio::post (
          _strand,
          [this, buffer, completion = std::move (completion)] () mutable {
              _stream.read_message_max (_read_message_limit);
              _stream.async_read (
                *buffer,
                asio::bind_executor (
                  _strand,
                  [buffer, completion = std::move (completion)] (
                    boost::system::error_code error, std::size_t) mutable {
                      if (error) {
                          buffer->consume (buffer->size ());
                          if (completion) {
                              completion (error, {});
                          }
                          return;
                      }
                      std::vector<std::uint8_t> bytes (buffer->size ());
                      asio::buffer_copy (asio::buffer (bytes), buffer->data ());
                      if (completion) {
                          completion (error, std::move (bytes));
                      }
                  }));
          });
    }

    void write (const std::vector<std::uint8_t> &bytes) override
    {
        run_serialized_sync (_io_context, _strand, [this, &bytes] {
            _stream.binary (true);
            _stream.write (asio::buffer (bytes));
        });
    }

    void async_write (std::vector<std::uint8_t> bytes,
                      std::function<void (boost::system::error_code)> completion) override
    {
        auto buffer = std::make_shared<std::vector<std::uint8_t>> (std::move (bytes));
        asio::post (
          _strand,
          [this, buffer, completion = std::move (completion)] () mutable {
              _stream.binary (true);
              _stream.async_write (
                asio::buffer (*buffer),
                asio::bind_executor (
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
            auto &socket = beast::get_lowest_layer (_stream);
            socket.shutdown (tcp::socket::shutdown_both, ignored);
            socket.close (ignored);
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
        asio::post (_strand, [this, self] {
            boost::system::error_code ignored;
            auto &socket = beast::get_lowest_layer (_stream);
            socket.shutdown (tcp::socket::shutdown_both, ignored);
            socket.close (ignored);
        });
    }

    void close (boost::system::error_code &error) override
    {
        run_serialized_sync (_io_context, _strand, [this, &error] {
            beast::get_lowest_layer (_stream).close (error);
        });
    }

    void set_read_message_limit (std::size_t limit) override
    {
        _read_message_limit = limit;
    }

  private:
    asio::io_context &_io_context;
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
    // The nested SSL stream stores a reference to its context. Retain the
    // context until the WebSocket stream has been destroyed.
    std::shared_ptr<ssl::context> _tls_context;
#endif
    websocket::stream<TStream> _stream;
    asio::strand<asio::io_context::executor_type> _strand;
    std::vector<std::uint8_t> _read_buffer;
    std::size_t _read_offset = 0;
    std::size_t _read_message_limit = 64u * 1024u + 65535u + 6u;
};

std::optional<websocket_endpoint_parts_t>
parse_websocket_endpoint_with_prefix (const std::string &endpoint, std::string_view prefix)
{
    if (endpoint.rfind (std::string (prefix), 0) != 0) {
        return std::nullopt;
    }

    const auto host_start = prefix.size ();
    const auto path_start = endpoint.find ('/', host_start);
    const auto authority = endpoint.substr (
      host_start, path_start == std::string::npos ? std::string::npos : path_start - host_start);
    const auto colon = authority.rfind (':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= authority.size ()) {
        return std::nullopt;
    }

    return websocket_endpoint_parts_t{
      authority.substr (0, colon), authority.substr (colon + 1),
      path_start == std::string::npos ? std::string ("/") : endpoint.substr (path_start)};
}

} // namespace

std::optional<websocket_endpoint_parts_t> parse_websocket_endpoint (const std::string &endpoint)
{
    return parse_websocket_endpoint_with_prefix (endpoint, "ws://");
}

std::optional<websocket_endpoint_parts_t>
parse_websocket_secure_endpoint (const std::string &endpoint)
{
    return parse_websocket_endpoint_with_prefix (endpoint, "wss://");
}

std::unique_ptr<stream_connection_t> connect_websocket (boost::asio::io_context &io_context,
                                                        const websocket_endpoint_parts_t &endpoint)
{
    tcp::resolver resolver (io_context);
    auto endpoints = resolver.resolve (endpoint.host, endpoint.port);
    websocket::stream<tcp::socket> stream (io_context);
    asio::connect (stream.next_layer (), endpoints);
    stream.binary (true);
    stream.handshake (endpoint.host, endpoint.target);
    return std::make_unique<websocket_stream_connection_t<tcp::socket>> (std::move (stream),
                                                                           io_context);
}

void connect_websocket_async (
  boost::asio::io_context &io_context,
  websocket_endpoint_parts_t endpoint,
  std::shared_ptr<transport_connect_control_t> control,
  std::function<void (boost::system::error_code, std::unique_ptr<stream_connection_t>)> callback)
{
    auto resolver = std::make_shared<tcp::resolver> (io_context);
    auto stream = std::make_shared<websocket::stream<tcp::socket>> (io_context);
    control->set_cancel_handler ([resolver, stream] {
        boost::system::error_code ignored;
        resolver->cancel ();
        stream->next_layer ().cancel (ignored);
        stream->next_layer ().close (ignored);
    });
    const auto host = endpoint.host;
    const auto port = endpoint.port;
    resolver->async_resolve (
      host, port,
      [&io_context, resolver, stream, control, endpoint = std::move (endpoint), callback = std::move (callback)] (
        boost::system::error_code error, tcp::resolver::results_type endpoints) mutable {
          if (control->cancelled ()) {
              callback (asio::error::operation_aborted, nullptr);
              return;
          }
          if (error) {
              callback (error, nullptr);
              return;
          }
          asio::async_connect (
            stream->next_layer (), endpoints,
            [&io_context, stream, control, endpoint = std::move (endpoint), callback = std::move (callback)] (
              boost::system::error_code connect_error, const tcp::endpoint &) mutable {
                if (control->cancelled ()) {
                    callback (asio::error::operation_aborted, nullptr);
                    return;
                }
                if (connect_error) {
                    callback (connect_error, nullptr);
                    return;
                }
                stream->binary (true);
                stream->async_handshake (
                  endpoint.host, endpoint.target,
                  [&io_context, stream, control, callback = std::move (callback)] (
                    boost::system::error_code handshake_error) mutable {
                      if (control->cancelled ()) {
                          callback (asio::error::operation_aborted, nullptr);
                          return;
                      }
                      if (handshake_error) {
                          callback (handshake_error, nullptr);
                          return;
                      }
                      callback ({}, std::make_unique<websocket_stream_connection_t<tcp::socket>> (
                                          std::move (*stream), io_context));
                  });
            });
      });
}

#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
std::unique_ptr<stream_connection_t>
connect_websocket_secure (boost::asio::io_context &io_context,
                          const websocket_endpoint_parts_t &endpoint,
                          bool skip_server_certificate_validation)
{
    auto context = std::make_shared<ssl::context> (ssl::context::tls_client);
    context->set_default_verify_paths ();
    websocket::stream<ssl::stream<tcp::socket>> stream (io_context, *context);
    SSL_set_tlsext_host_name (stream.next_layer ().native_handle (), endpoint.host.c_str ());
    if (skip_server_certificate_validation) {
        stream.next_layer ().set_verify_mode (ssl::verify_none);
    } else {
        stream.next_layer ().set_verify_mode (ssl::verify_peer);
        stream.next_layer ().set_verify_callback (ssl::host_name_verification (endpoint.host));
    }

    tcp::resolver resolver (io_context);
    auto endpoints = resolver.resolve (endpoint.host, endpoint.port);
    asio::connect (beast::get_lowest_layer (stream), endpoints);
    stream.next_layer ().handshake (ssl::stream_base::client);
    stream.binary (true);
    stream.handshake (endpoint.host, endpoint.target);
    return std::make_unique<websocket_stream_connection_t<ssl::stream<tcp::socket>>> (
      std::move (stream), io_context, std::move (context));
}

void connect_websocket_secure_async (
  boost::asio::io_context &io_context,
  websocket_endpoint_parts_t endpoint,
  bool skip_server_certificate_validation,
  std::shared_ptr<transport_connect_control_t> control,
  std::function<void (boost::system::error_code, std::unique_ptr<stream_connection_t>)> callback)
{
    auto context = std::make_shared<ssl::context> (ssl::context::tls_client);
    context->set_default_verify_paths ();
    auto stream =
      std::make_shared<websocket::stream<ssl::stream<tcp::socket>>> (io_context, *context);
    SSL_set_tlsext_host_name (stream->next_layer ().native_handle (), endpoint.host.c_str ());
    if (skip_server_certificate_validation) {
        stream->next_layer ().set_verify_mode (ssl::verify_none);
    } else {
        stream->next_layer ().set_verify_mode (ssl::verify_peer);
        stream->next_layer ().set_verify_callback (ssl::host_name_verification (endpoint.host));
    }

    const auto host = endpoint.host;
    const auto port = endpoint.port;
    auto resolver = std::make_shared<tcp::resolver> (io_context);
    control->set_cancel_handler ([resolver, stream] {
        boost::system::error_code ignored;
        resolver->cancel ();
        beast::get_lowest_layer (*stream).cancel (ignored);
        beast::get_lowest_layer (*stream).close (ignored);
    });
    resolver->async_resolve (
      host, port,
      [&io_context, resolver, context, stream, control, endpoint = std::move (endpoint),
       callback = std::move (callback)] (boost::system::error_code error,
                                         tcp::resolver::results_type endpoints) mutable {
          if (control->cancelled ()) {
              callback (asio::error::operation_aborted, nullptr);
              return;
          }
          if (error) {
              callback (error, nullptr);
              return;
          }
          asio::async_connect (
            beast::get_lowest_layer (*stream), endpoints,
            [&io_context, context, stream, control, endpoint = std::move (endpoint),
             callback = std::move (callback)] (
              boost::system::error_code connect_error, const tcp::endpoint &) mutable {
                if (control->cancelled ()) {
                    callback (asio::error::operation_aborted, nullptr);
                    return;
                }
                if (connect_error) {
                    callback (connect_error, nullptr);
                    return;
                }
                stream->next_layer ().async_handshake (
                  ssl::stream_base::client,
                  [&io_context, context, stream, control, endpoint = std::move (endpoint),
                   callback = std::move (callback)] (
                    boost::system::error_code tls_error) mutable {
                      if (control->cancelled ()) {
                          callback (asio::error::operation_aborted, nullptr);
                          return;
                      }
                      if (tls_error) {
                          callback (tls_error, nullptr);
                          return;
                      }
                      stream->binary (true);
                      stream->async_handshake (
                        endpoint.host, endpoint.target,
                          [&io_context, context, stream, control, callback = std::move (callback)] (
                          boost::system::error_code websocket_error) mutable {
                            if (control->cancelled ()) {
                                callback (asio::error::operation_aborted, nullptr);
                                return;
                            }
                            if (websocket_error) {
                                callback (websocket_error, nullptr);
                                return;
                            }
                            callback (
                              {}, std::make_unique<
                                    websocket_stream_connection_t<ssl::stream<tcp::socket>>> (
                                    std::move (*stream), io_context, std::move (context)));
                        });
                  });
            });
      });
}
#endif

} // namespace zlink::stream_connector::detail
