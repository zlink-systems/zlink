/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/transport/stream_connection.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#ifdef ZLINK_STREAM_CONNECTOR_WITH_OPENSSL
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <openssl/ssl.h>
#endif

#include <algorithm>
#include <cctype>
#include <exception>
#include <memory>
#include <utility>

namespace zlink::stream_connector::detail
{

namespace
{

class tcp_stream_connection_t final : public stream_connection_t
{
  public:
    explicit tcp_stream_connection_t (boost::asio::io_context &io_context,
                                      boost::asio::ip::tcp::socket socket) :
        _io_context (io_context), _socket (std::move (socket)), _strand (io_context.get_executor ())
    {
    }

    void async_read_some (std::size_t max_size,
                          std::function<void (boost::system::error_code, std::vector<std::uint8_t>)>
                            completion) override
    {
        auto buffer = std::make_shared<std::vector<std::uint8_t>> (max_size);
        boost::asio::post (_strand, [this, buffer, completion = std::move (completion)] () mutable {
            _socket.async_read_some (
              boost::asio::buffer (*buffer),
              boost::asio::bind_executor (
                _strand, [buffer, completion = std::move (completion)] (
                           boost::system::error_code error, std::size_t bytes_read) mutable {
                    buffer->resize (bytes_read);
                    if (completion) {
                        completion (error, std::move (*buffer));
                    }
                }));
        });
    }

    void async_write (std::vector<std::uint8_t> bytes,
                      std::function<void (boost::system::error_code)> completion) override
    {
        auto buffer = std::make_shared<std::vector<std::uint8_t>> (std::move (bytes));
        boost::asio::post (_strand, [this, buffer, completion = std::move (completion)] () mutable {
            boost::asio::async_write (
              _socket, boost::asio::buffer (*buffer),
              boost::asio::bind_executor (_strand,
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

    void async_read_some (std::size_t max_size,
                          std::function<void (boost::system::error_code, std::vector<std::uint8_t>)>
                            completion) override
    {
        auto buffer = std::make_shared<std::vector<std::uint8_t>> (max_size);
        boost::asio::post (_strand, [this, buffer, completion = std::move (completion)] () mutable {
            _stream.async_read_some (
              boost::asio::buffer (*buffer),
              boost::asio::bind_executor (
                _strand, [buffer, completion = std::move (completion)] (
                           boost::system::error_code error, std::size_t bytes_read) mutable {
                    buffer->resize (bytes_read);
                    if (completion) {
                        completion (error, std::move (*buffer));
                    }
                }));
        });
    }

    void async_write (std::vector<std::uint8_t> bytes,
                      std::function<void (boost::system::error_code)> completion) override
    {
        auto buffer = std::make_shared<std::vector<std::uint8_t>> (std::move (bytes));
        boost::asio::post (_strand, [this, buffer, completion = std::move (completion)] () mutable {
            boost::asio::async_write (
              _stream, boost::asio::buffer (*buffer),
              boost::asio::bind_executor (_strand,
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

/* Case-insensitive scheme match: endpoint-notation policy §2.6 requires
 * the scheme to be lowercased before the connector decides which
 * transport handles it, so "TCP://host:1" is accepted the same as
 * "tcp://host:1". Only the scheme portion (up to prefix.size()) is
 * compared case-insensitively; the host/port that follows is untouched. */
bool scheme_prefix_matches (const std::string &endpoint, std::string_view prefix) noexcept
{
    if (endpoint.size () < prefix.size ()) {
        return false;
    }
    return std::equal (prefix.begin (), prefix.end (), endpoint.begin (),
                       [] (unsigned char expected, unsigned char actual) {
                           return expected == std::tolower (actual);
                       });
}

std::optional<endpoint_parts_t> parse_host_port_endpoint (const std::string &endpoint,
                                                          std::string_view prefix)
{
    if (!scheme_prefix_matches (endpoint, prefix)) {
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
            [&io_context, context, stream, control,
             callback = std::move (callback)] (boost::system::error_code connect_error,
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

} // namespace zlink::stream_connector::detail
