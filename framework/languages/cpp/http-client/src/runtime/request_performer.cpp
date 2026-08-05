/* SPDX-License-Identifier: Apache-2.0 */

#include "runtime/request_performer.hpp"

#include <zlink/http_client/contracts/types.hpp>

#include "runtime/compression.hpp"
#include "runtime/connection_opener.hpp"
#include "runtime/connection_pool.hpp"
#include "runtime/runtime_errors.hpp"
#include "runtime/text.hpp"
#include "runtime/url.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace zlink::http_client::detail
{
namespace
{
namespace beast = boost::beast;
namespace http = beast::http;
using body_provider_t = std::function<std::optional<std::string> ()>;

struct exchange_outcome_t
{
    http::response<http::string_body> response;
    bool reusable = false;
};

raw_http_response_t to_raw_response (const http::response<http::string_body> &response)
{
    raw_http_response_t raw;
    raw.status = static_cast<int> (response.result_int ());
    raw.body = response.body ();
    for (const auto &field : response) {
        //  Names are lower-cased so lookups do not depend on the server's spelling
        //  (cross-language contract: header lookup is case-insensitive).
        std::string name (field.name_string ());
        std::transform (name.begin (), name.end (), name.begin (),
                        [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
        raw.headers.emplace (std::move (name), std::string (field.value ()));
    }
    return raw;
}

bool is_authorization_header (const std::string &name)
{
    return iequals (name, "authorization");
}

bool is_content_type_header (const std::string &name)
{
    return iequals (name, "content-type");
}

bool can_retry_reused_connection (http_method_t method)
{
    return method == http_method_t::get || method == http_method_t::head
           || method == http_method_t::options;
}

zlink::framework::result_t<raw_http_response_t>
finish_response (raw_http_response_t response,
                 std::chrono::steady_clock::time_point started_at,
                 std::chrono::milliseconds timeout)
{
    if (std::chrono::steady_clock::now () - started_at > timeout) {
        return zlink::framework::detail::boundary_failure<raw_http_response_t> (
          zlink::framework::detail::boundary_error_t::timed_out,
          "HTTP request exceeded timeout");
    }
    return zlink::framework::result_t<raw_http_response_t>::success (std::move (response));
}

class request_performer_t
{
  public:
    request_performer_t (const http_client_options_t &options,
                         cookie_jar_t &cookie_jar,
                         connection_pool_t &pool,
                         const http_request_t &request) :
        _options (options), _cookie_jar (cookie_jar), _pool (pool), _request (request)
    {
    }

    zlink::framework::result_t<raw_http_response_t> perform ()
    {
        const auto started_at = std::chrono::steady_clock::now ();
        const auto url = parse_base_url (_options.base_url);

        hop_target_t hop{.scheme = url.scheme,
                         .host = url.host,
                         .port = url.port,
                         .target = make_target (url, _request.path)};
        const auto origin = hop;
        auto method = _request.method;
        auto body = _request.body;
        auto body_provider = _request.body_provider;
        int redirects_left = _options.follow_redirects;

        for (;;) {
            auto response = perform_hop (origin, hop, method, body, body_provider);
            if (_options.cookies) {
                for (const auto &field : response) {
                    if (field.name () == http::field::set_cookie) {
                        _cookie_jar.store (hop.host, std::string (field.value ()));
                    }
                }
            }

            const auto status = static_cast<int> (response.result_int ());
            const auto location = std::string (response[http::field::location]);
            if (_options.follow_redirects > 0 && is_redirect_status (status)
                && !location.empty ()) {
                if (redirects_left == 0) {
                    throw request_error ("HTTP request exceeded the redirect limit");
                }
                --redirects_left;
                if (status == 303
                    || ((status == 301 || status == 302) && method == http_method_t::post)) {
                    method = http_method_t::get;
                    body.reset ();
                }
                body_provider = {};
                hop = resolve_location (hop, location);
                continue;
            }

            auto raw = to_raw_response (response);
            if (_options.compression && !_request.sink) {
                const auto encoding = find_header (raw.headers, "content-encoding");
                if (encoding && iequals (*encoding, "gzip")) {
                    raw.body = gunzip (raw.body, _options.max_response_body_size);
                    erase_header (raw.headers, "content-encoding");
                    erase_header (raw.headers, "content-length");
                } else if (encoding && iequals (*encoding, "deflate")) {
                    raw.body = inflate_deflate (raw.body, _options.max_response_body_size);
                    erase_header (raw.headers, "content-encoding");
                    erase_header (raw.headers, "content-length");
                }
            }
            return finish_response (std::move (raw), started_at, effective_timeout ());
        }
    }

  private:
    std::chrono::milliseconds effective_timeout () const
    {
        return _request.timeout.value_or (_options.timeout);
    }

    template <typename TBody>
    http::request<TBody>
    build_wire_request (const hop_target_t &origin,
                        const hop_target_t &hop,
                        http_method_t method,
                        bool has_body,
                        bool absolute_form) const
    {
        const auto wire_target =
          absolute_form ? "http://" + hop.host + ":" + hop.port + hop.target : hop.target;
        http::request<TBody> wire{to_beast_method (method), wire_target, 11};

        const bool default_port = (hop.scheme == "http" && hop.port == "80")
                                  || (hop.scheme == "https" && hop.port == "443");
        wire.set (http::field::host, default_port ? hop.host : hop.host + ":" + hop.port);
        // Version identity: derived from contracts/types.hpp version constants (single source).
        static const std::string user_agent = "zlink-http-client/"
                                              + std::to_string (zlink::http_client::version_major) + "."
                                              + std::to_string (zlink::http_client::version_minor);
        wire.set (http::field::user_agent, user_agent);
        wire.set (http::field::accept, "application/json");
        if (_options.compression) {
            wire.set (http::field::accept_encoding, "gzip, deflate");
        }
        if (absolute_form && _options.proxy_authorization) {
            wire.set (http::field::proxy_authorization, *_options.proxy_authorization);
        }
        const bool keep_authorization = same_origin (origin, hop);
        for (const auto &[name, value] : _options.headers) {
            if (!keep_authorization && is_authorization_header (name)) {
                continue;
            }
            if (!has_body && is_content_type_header (name)) {
                continue;
            }
            wire.set (name, value);
        }
        for (const auto &[name, value] : _request.headers) {
            if (!keep_authorization && is_authorization_header (name)) {
                continue;
            }
            if (!has_body && is_content_type_header (name)) {
                continue;
            }
            wire.set (name, value);
        }
        if (_options.cookies) {
            const auto path = hop.target.substr (0, hop.target.find ('?'));
            const auto cookie_header =
              _cookie_jar.header_for (hop.host, path, hop.scheme == "https");
            if (!cookie_header.empty ()) {
                wire.set (http::field::cookie, cookie_header);
            }
        }
        return wire;
    }

    std::string pool_key (const hop_target_t &hop) const
    {
        auto key = hop.scheme + "|" + hop.host + ":" + hop.port;
        if (_options.proxy) {
            key += "|proxy=" + *_options.proxy;
        }
        return key;
    }

    http::response<http::string_body> perform_hop (const hop_target_t &origin,
                                                   const hop_target_t &hop,
                                                   http_method_t method,
                                                   const std::optional<std::string> &body,
                                                   const body_provider_t &body_provider)
    {
        const auto key = pool_key (hop);
        const bool can_reuse = !body_provider;

        for (int attempt = 0; attempt < 2; ++attempt) {
            std::unique_ptr<pooled_connection_t> connection;
            bool reused = false;
            if (can_reuse && attempt == 0) {
                connection = _pool.acquire (key);
                reused = connection != nullptr;
            }
            if (!connection) {
                connection = open_connection (_options, hop, effective_timeout ());
            }

            try {
                auto outcome =
                  run_exchange (*connection, origin, hop, method, body, body_provider);
                if (outcome.reusable && can_reuse) {
                    _pool.release (key, std::move (connection));
                }
                return std::move (outcome.response);
            }
            catch (...) {
                if (!reused || !can_retry_reused_connection (method)) {
                    throw;
                }
            }
        }
        throw request_error ("HTTP connection retry exhausted");
    }

    exchange_outcome_t run_exchange (pooled_connection_t &connection,
                                     const hop_target_t &origin,
                                     const hop_target_t &hop,
                                     http_method_t method,
                                     const std::optional<std::string> &body,
                                     const body_provider_t &body_provider)
    {
        if (connection.plain) {
            connection.plain->expires_after (effective_timeout ());
            return run_exchange_on (*connection.plain, connection.buffer, origin, hop, method,
                                    body, body_provider);
        }
#ifdef ZLINK_HTTP_CLIENT_WITH_OPENSSL
        if (connection.secure) {
            beast::get_lowest_layer (*connection.secure).expires_after (effective_timeout ());
            return run_exchange_on (*connection.secure, connection.buffer, origin, hop, method,
                                    body, body_provider);
        }
#endif
        throw request_error ("HTTP connection is not open");
    }

    template <typename TStream>
    exchange_outcome_t run_exchange_on (TStream &stream,
                                        beast::flat_buffer &buffer,
                                        const hop_target_t &origin,
                                        const hop_target_t &hop,
                                        http_method_t method,
                                        const std::optional<std::string> &body,
                                        const body_provider_t &body_provider)
    {
        const bool absolute_form = _options.proxy.has_value () && hop.scheme == "http";
        const bool has_body = body.has_value () || static_cast<bool> (body_provider);

        if (body_provider) {
            auto wire = build_wire_request<http::buffer_body> (origin, hop, method,
                                                               has_body,
                                                               absolute_form);
            wire.chunked (true);
            send_streamed (stream, wire, body_provider);
        } else {
            auto wire = build_wire_request<http::string_body> (origin, hop, method,
                                                               has_body,
                                                               absolute_form);
            if (body) {
                wire.body () = *body;
                wire.prepare_payload ();
            }
            http::write (stream, wire);
        }
        return receive (stream, buffer);
    }

    template <typename TStream>
    static void send_streamed (TStream &stream,
                               http::request<http::buffer_body> &request,
                               const std::function<std::optional<std::string> ()> &provider)
    {
        http::request_serializer<http::buffer_body> serializer{request};
        beast::error_code ec;
        http::write_header (stream, serializer, ec);
        if (ec) {
            throw boost::system::system_error (ec);
        }

        while (!serializer.is_done ()) {
            auto chunk = provider ();
            if (chunk && chunk->empty ()) {
                continue;
            }
            if (chunk) {
                request.body ().data = chunk->data ();
                request.body ().size = chunk->size ();
                request.body ().more = true;
            } else {
                request.body ().data = nullptr;
                request.body ().size = 0;
                request.body ().more = false;
            }
            http::write (stream, serializer, ec);
            if (ec == http::error::need_buffer) {
                ec = {};
            }
            if (ec) {
                throw boost::system::system_error (ec);
            }
        }
    }

    template <typename TStream>
    exchange_outcome_t receive (TStream &stream, beast::flat_buffer &buffer)
    {
        const bool skip_body = _request.method == http_method_t::head;

        if (!_request.sink) {
            http::response_parser<http::string_body> parser;
            parser.body_limit (_options.max_response_body_size);
            parser.skip (skip_body);
            http::read (stream, buffer, parser);
            const bool reusable = parser.get ().keep_alive ();
            return {parser.release (), reusable};
        }

        http::response_parser<http::buffer_body> parser;
        parser.body_limit (_options.max_response_body_size);
        parser.skip (skip_body);
        http::read_header (stream, buffer, parser);

        const bool draining = _options.follow_redirects > 0
                              && is_redirect_status (static_cast<int> (parser.get ().result_int ()))
                              && parser.get ().count (http::field::location) > 0;

        char chunk[16384];
        while (!parser.is_done ()) {
            parser.get ().body ().data = chunk;
            parser.get ().body ().size = sizeof chunk;
            beast::error_code ec;
            http::read (stream, buffer, parser, ec);
            if (ec && ec != http::error::need_buffer) {
                throw boost::system::system_error (ec);
            }
            const auto produced = sizeof chunk - parser.get ().body ().size;
            if (!draining && produced > 0) {
                _request.sink (std::string_view (chunk, produced));
            }
        }

        const bool reusable = parser.get ().keep_alive ();
        http::response<http::string_body> response;
        response.base () = parser.get ().base ();
        return {std::move (response), reusable};
    }

    const http_client_options_t &_options;
    cookie_jar_t &_cookie_jar;
    connection_pool_t &_pool;
    const http_request_t &_request;
};

} // namespace

zlink::framework::result_t<raw_http_response_t> perform_once (const http_client_options_t &options,
                                                              cookie_jar_t &cookie_jar,
                                                              connection_pool_t &pool,
                                                              const http_request_t &request)
{
    try {
        return request_performer_t (options, cookie_jar, pool, request).perform ();
    }
    catch (const zlink::framework::framework_exception_t &ex) {
        return zlink::framework::detail::result_access_t::failure<raw_http_response_t> (ex);
    }
    catch (const boost::system::system_error &ex) {
        if (ex.code () == boost::beast::error::timeout
            || ex.code () == boost::asio::error::timed_out) {
            return zlink::framework::detail::boundary_failure<raw_http_response_t> (
              zlink::framework::detail::boundary_error_t::timed_out, ex.what ());
        }
        if (ex.code () == boost::beast::http::error::body_limit) {
            return zlink::framework::result_t<raw_http_response_t>::failure (
              zlink::framework::framework_error_kind_t::capacity_exceeded,
              ex.what ());
        }
        const std::string_view category (ex.code ().category ().name ());
        if (category.find ("ssl") != std::string_view::npos) {
            return map_unexpected_exception (ex);
        }
        return map_transport_exception (ex);
    }
    catch (const std::exception &ex) {
        return map_unexpected_exception (ex);
    }
}

} // namespace zlink::http_client::detail
