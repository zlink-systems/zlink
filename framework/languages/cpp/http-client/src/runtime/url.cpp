/* SPDX-License-Identifier: Apache-2.0 */

#include "runtime/url.hpp"

#include "runtime/runtime_errors.hpp"
#include "runtime/text.hpp"

#include <stdexcept>

namespace zlink::http_client::detail
{

namespace http = boost::beast::http;

parsed_url_t parse_base_url (const std::string &url)
{
    std::string scheme;
    std::string rest;
    if (starts_with (url, "http://")) {
        scheme = "http";
        rest = url.substr (7);
    } else if (starts_with (url, "https://")) {
        scheme = "https";
        rest = url.substr (8);
    } else {
        throw std::invalid_argument ("HTTP client base_url must start with http:// or https://");
    }

    const auto slash = rest.find ('/');
    auto authority = slash == std::string::npos ? rest : rest.substr (0, slash);
    auto target_prefix = slash == std::string::npos ? std::string () : rest.substr (slash);
    if (authority.empty ()) {
        throw std::invalid_argument ("HTTP client base_url requires a host");
    }

    std::string host;
    std::string port = scheme == "https" ? "443" : "80";
    if (authority.front () == '[') {
        const auto close = authority.find (']');
        if (close == std::string::npos) {
            throw std::invalid_argument ("HTTP client IPv6 host is missing ]");
        }
        host = authority.substr (1, close - 1);
        if (close + 1 < authority.size ()) {
            if (authority[close + 1] != ':') {
                throw std::invalid_argument ("HTTP client base_url has invalid IPv6 authority");
            }
            port = authority.substr (close + 2);
        }
    } else {
        const auto colon = authority.rfind (':');
        if (colon == std::string::npos) {
            host = authority;
        } else {
            host = authority.substr (0, colon);
            port = authority.substr (colon + 1);
        }
    }

    if (host.empty () || port.empty ()) {
        throw std::invalid_argument ("HTTP client base_url requires host and port");
    }

    return {.scheme = std::move (scheme),
            .host = std::move (host),
            .port = std::move (port),
            .target_prefix = std::move (target_prefix)};
}

std::string make_target (const parsed_url_t &url, const std::string &path)
{
    if (path.empty () || path.front () != '/') {
        throw std::invalid_argument ("HTTP request path must start with /");
    }
    if (url.target_prefix.empty () || url.target_prefix == "/") {
        return path;
    }
    if (url.target_prefix.back () == '/') {
        return url.target_prefix.substr (0, url.target_prefix.size () - 1) + path;
    }
    return url.target_prefix + path;
}

http::verb to_beast_method (http_method_t method)
{
    switch (method) {
        case http_method_t::get:
            return http::verb::get;
        case http_method_t::post:
            return http::verb::post;
        case http_method_t::put:
            return http::verb::put;
        case http_method_t::delete_:
            return http::verb::delete_;
        case http_method_t::patch:
            return http::verb::patch;
        case http_method_t::head:
            return http::verb::head;
        case http_method_t::options:
            return http::verb::options;
    }
    return http::verb::get;
}

bool is_redirect_status (int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool same_origin (const hop_target_t &left, const hop_target_t &right)
{
    return iequals (left.scheme, right.scheme) && iequals (left.host, right.host)
           && left.port == right.port;
}

hop_target_t resolve_location (const hop_target_t &current, const std::string &location)
{
    if (starts_with (location, "http://") || starts_with (location, "https://")) {
        const auto url = parse_base_url (location);
        return {.scheme = url.scheme,
                .host = url.host,
                .port = url.port,
                .target = url.target_prefix.empty () ? "/" : url.target_prefix};
    }
    if (!location.empty () && location.front () == '/') {
        return {
          .scheme = current.scheme, .host = current.host, .port = current.port, .target = location};
    }
    throw request_error ("HTTP redirect location is not supported: " + location);
}

} // namespace zlink::http_client::detail
