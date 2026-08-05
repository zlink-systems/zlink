/* SPDX-License-Identifier: Apache-2.0 */

#include "runtime/connection_opener.hpp"

#include "runtime/runtime_errors.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>

#ifdef ZLINK_HTTP_CLIENT_WITH_OPENSSL
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/ssl.hpp>
#endif

namespace zlink::http_client::detail
{
namespace
{
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

void open_proxy_tunnel (const http_client_options_t &options,
                        beast::tcp_stream &stream,
                        const hop_target_t &hop)
{
    const auto authority = hop.host + ":" + hop.port;
    http::request<http::empty_body> connect_request{http::verb::connect, authority, 11};
    connect_request.set (http::field::host, authority);
    if (options.proxy_authorization) {
        connect_request.set (http::field::proxy_authorization, *options.proxy_authorization);
    }
    http::write (stream, connect_request);

    beast::flat_buffer buffer;
    http::response_parser<http::empty_body> parser;
    parser.skip (true);
    http::read (stream, buffer, parser);
    if (parser.get ().result () != http::status::ok) {
        throw request_error ("HTTP proxy CONNECT failed with status "
                             + std::to_string (parser.get ().result_int ()));
    }
}

} // namespace

std::unique_ptr<pooled_connection_t>
open_connection (const http_client_options_t &options,
                 const hop_target_t &hop,
                 std::chrono::milliseconds timeout)
{
    auto connection = std::make_unique<pooled_connection_t> ();
    tcp::resolver resolver (connection->io);

    const bool via_proxy = options.proxy.has_value ();
    std::string connect_host = hop.host;
    std::string connect_port = hop.port;
    if (via_proxy) {
        const auto proxy = parse_base_url (*options.proxy);
        connect_host = proxy.host;
        connect_port = proxy.port;
    }
    const auto endpoints = resolver.resolve (connect_host, connect_port);

    if (hop.scheme == "http") {
        connection->plain.emplace (connection->io);
        connection->plain->expires_after (timeout);
        connection->plain->connect (endpoints);
        return connection;
    }

#ifdef ZLINK_HTTP_CLIENT_WITH_OPENSSL
    connection->tls_context.emplace (asio::ssl::context::tls_client);
    auto &context = *connection->tls_context;
    context.set_default_verify_paths ();
    if (options.trust_certificate_file) {
        context.load_verify_file (*options.trust_certificate_file);
    }
    if (options.client_certificate) {
        context.use_certificate_chain_file (options.client_certificate->first);
        context.use_private_key_file (options.client_certificate->second, asio::ssl::context::pem);
    }

    connection->secure.emplace (connection->io, context);
    auto &stream = *connection->secure;
    SSL_set_tlsext_host_name (stream.native_handle (), hop.host.c_str ());
    stream.set_verify_mode (asio::ssl::verify_peer);
    stream.set_verify_callback (asio::ssl::host_name_verification (hop.host));
    beast::get_lowest_layer (stream).expires_after (timeout);
    beast::get_lowest_layer (stream).connect (endpoints);
    if (via_proxy) {
        open_proxy_tunnel (options, beast::get_lowest_layer (stream), hop);
    }
    stream.handshake (asio::ssl::stream_base::client);
    return connection;
#else
    throw zlink::framework::framework_exception_t (
      zlink::framework::framework_error_kind_t::protocol_error,
      "HTTPS support requires OpenSSL");
#endif
}

} // namespace zlink::http_client::detail
