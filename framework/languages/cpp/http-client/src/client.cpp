/* SPDX-License-Identifier: Apache-2.0 */

#include <zlink/http_client.hpp>

#include "runtime/http_client_runtime.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>

namespace zlink::http_client
{
namespace
{

bool is_blank (const std::string &value)
{
    return value.empty () || std::all_of (value.begin (), value.end (), [] (char ch) {
               return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
           });
}

void require_non_blank (const std::string &value, const char *message)
{
    if (is_blank (value)) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error, message);
    }
}

void require_positive_timeout (std::chrono::milliseconds value)
{
    if (value <= std::chrono::milliseconds::zero ()) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "HTTP client timeout must be greater than zero");
    }
}

std::string percent_encode (const std::string &value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve (value.size ());
    for (const unsigned char ch : value) {
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
                                || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.'
                                || ch == '~';
        if (unreserved) {
            encoded.push_back (static_cast<char> (ch));
        } else {
            encoded.push_back ('%');
            encoded.push_back (hex[ch >> 4]);
            encoded.push_back (hex[ch & 0x0f]);
        }
    }
    return encoded;
}

std::string base64_encode (std::string_view input)
{
    static constexpr char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve ((input.size () + 2) / 3 * 4);
    unsigned value = 0;
    int bits = -6;
    for (const unsigned char ch : input) {
        value = (value << 8) + ch;
        bits += 8;
        while (bits >= 0) {
            encoded.push_back (table[(value >> bits) & 0x3f]);
            bits -= 6;
        }
    }
    if (bits > -6) {
        encoded.push_back (table[((value << 8) >> (bits + 8)) & 0x3f]);
    }
    while (encoded.size () % 4 != 0) {
        encoded.push_back ('=');
    }
    return encoded;
}

std::string make_multipart_boundary ()
{
    std::random_device device;
    std::mt19937_64 generator (device ());
    std::uniform_int_distribution<unsigned long long> distribution;
    std::string boundary = "zlink-boundary-";
    static constexpr char hex[] = "0123456789abcdef";
    auto bits = distribution (generator);
    for (int nibble = 0; nibble < 16; ++nibble) {
        boundary.push_back (hex[bits & 0x0f]);
        bits >>= 4;
    }
    return boundary;
}

} // namespace

client_builder_t client_t::create ()
{
    return {};
}

client_builder_t client_t::create (std::string base_url)
{
    client_builder_t builder;
    builder.base_url (std::move (base_url));
    return builder;
}

client_t::client_t (std::shared_ptr<detail::http_client_runtime_t> runtime) :
    _runtime (std::move (runtime))
{
}

request_builder_t client_t::get (std::string path) const
{
    return request_builder_t (*this, http_method_t::get, std::move (path));
}

request_builder_t client_t::post (std::string path) const
{
    return request_builder_t (*this, http_method_t::post, std::move (path));
}

request_builder_t client_t::put (std::string path) const
{
    return request_builder_t (*this, http_method_t::put, std::move (path));
}

request_builder_t client_t::delete_ (std::string path) const
{
    return request_builder_t (*this, http_method_t::delete_, std::move (path));
}

request_builder_t client_t::patch (std::string path) const
{
    return request_builder_t (*this, http_method_t::patch, std::move (path));
}

request_builder_t client_t::head (std::string path) const
{
    return request_builder_t (*this, http_method_t::head, std::move (path));
}

request_builder_t client_t::options (std::string path) const
{
    return request_builder_t (*this, http_method_t::options, std::move (path));
}

client_builder_t &client_builder_t::base_url (std::string value)
{
    require_non_blank (value, "HTTP client base_url is required");
    _base_url = std::move (value);
    return *this;
}

client_builder_t &client_builder_t::timeout (std::chrono::milliseconds value)
{
    require_positive_timeout (value);
    _timeout = value;
    return *this;
}

client_builder_t &client_builder_t::default_header (std::string name, std::string value)
{
    require_non_blank (name, "HTTP client default header name is required");
    _headers[std::move (name)] = std::move (value);
    return *this;
}

client_builder_t &client_builder_t::basic_auth (const std::string &user,
                                                const std::string &password)
{
    require_non_blank (user, "HTTP client basic auth user is required");
    _headers["authorization"] = "Basic " + base64_encode (user + ":" + password);
    return *this;
}

client_builder_t &client_builder_t::bearer_token (const std::string &token)
{
    require_non_blank (token, "HTTP client bearer token is required");
    _headers["authorization"] = "Bearer " + token;
    return *this;
}

client_builder_t &client_builder_t::max_response_body_size (std::size_t bytes)
{
    if (bytes == 0) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "HTTP client max response body size must be greater than zero");
    }
    _max_response_body_size = bytes;
    return *this;
}

client_builder_t &client_builder_t::trust_certificate_file (std::string path)
{
    require_non_blank (path, "HTTP client trust certificate file is required");
    _trust_certificate_file = std::move (path);
    return *this;
}

client_builder_t &client_builder_t::client_certificate_file (std::string certificate_path,
                                                             std::string key_path)
{
    require_non_blank (certificate_path, "HTTP client certificate file is required");
    require_non_blank (key_path, "HTTP client certificate key file is required");
    _client_certificate = {std::move (certificate_path), std::move (key_path)};
    return *this;
}

client_builder_t &client_builder_t::follow_redirects (int max_redirects)
{
    if (max_redirects <= 0) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "HTTP client follow_redirects must be greater than zero");
    }
    _follow_redirects = max_redirects;
    return *this;
}

client_builder_t &client_builder_t::retry (int attempts)
{
    if (attempts <= 0) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "HTTP client retry attempts must be greater than zero");
    }
    _retry_attempts = attempts;
    return *this;
}

client_builder_t &client_builder_t::cookies ()
{
    _cookies = true;
    return *this;
}

client_builder_t &client_builder_t::proxy (std::string url)
{
    require_non_blank (url, "HTTP client proxy url is required");
    if (url.rfind ("http://", 0) != 0) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "HTTP client proxy url must start with http://");
    }
    _proxy = std::move (url);
    return *this;
}

client_builder_t &client_builder_t::proxy_basic_auth (const std::string &user,
                                                      const std::string &password)
{
    require_non_blank (user, "HTTP client proxy auth user is required");
    _proxy_authorization = "Basic " + base64_encode (user + ":" + password);
    return *this;
}

client_builder_t &client_builder_t::compression ()
{
    _compression = true;
    return *this;
}

client_builder_t &client_builder_t::coroutines ()
{
    _coroutines = true;
    _execute_scheduler.reset ();
    _resume_scheduler.reset ();
    return *this;
}

client_builder_t &
client_builder_t::coroutines (std::shared_ptr<coroutine_resume_scheduler_t> resume_scheduler)
{
    if (!resume_scheduler) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "HTTP client coroutine resume scheduler is required");
    }
    _coroutines = true;
    _execute_scheduler.reset ();
    _resume_scheduler = std::move (resume_scheduler);
    return *this;
}

client_builder_t &
client_builder_t::coroutines (std::shared_ptr<coroutine_execute_scheduler_t> execute_scheduler,
                              std::shared_ptr<coroutine_resume_scheduler_t> resume_scheduler)
{
    if (!execute_scheduler || !resume_scheduler) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "HTTP client coroutine execute and resume schedulers are required");
    }
    _coroutines = true;
    _execute_scheduler = std::move (execute_scheduler);
    _resume_scheduler = std::move (resume_scheduler);
    return *this;
}

client_t client_builder_t::build () const
{
    require_non_blank (_base_url, "HTTP client base_url is required");
    require_positive_timeout (_timeout);
    auto execute_scheduler = _execute_scheduler;
    auto resume_scheduler = _resume_scheduler;
    if (_coroutines) {
        if (!execute_scheduler) {
            execute_scheduler = detail::default_coroutine_execute_scheduler ();
        }
        if (!resume_scheduler) {
            resume_scheduler = detail::default_coroutine_resume_scheduler ();
        }
    }
    detail::http_client_options_t options{.base_url = _base_url,
                                          .timeout = _timeout,
                                          .max_response_body_size = _max_response_body_size,
                                          .headers = _headers,
                                          .trust_certificate_file = _trust_certificate_file,
                                          .client_certificate = _client_certificate,
                                          .follow_redirects = _follow_redirects,
                                          .retry_attempts = _retry_attempts,
                                          .cookies = _cookies,
                                          .proxy = _proxy,
                                          .proxy_authorization = _proxy_authorization,
                                          .compression = _compression,
                                          .coroutines = _coroutines,
                                          .execute_scheduler = std::move (execute_scheduler),
                                          .resume_scheduler = std::move (resume_scheduler)};
    try {
        return client_t (std::make_shared<detail::http_client_runtime_t> (std::move (options)));
    }
    catch (const std::invalid_argument &error) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error, error.what ());
    }
}

server_client_t
client_builder_t::build_server (std::shared_ptr<execution_turn_t> execution_turn) const
{
    if (!execution_turn) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "HTTP server client execution turn is required");
    }
    auto builder = *this;
    builder.coroutines ();
    return server_client_t (builder.build (), std::move (execution_turn));
}

request_builder_t client_builder_t::get (std::string path) const
{
    return build ().get (std::move (path));
}

request_builder_t client_builder_t::post (std::string path) const
{
    return build ().post (std::move (path));
}

request_builder_t client_builder_t::put (std::string path) const
{
    return build ().put (std::move (path));
}

request_builder_t client_builder_t::delete_ (std::string path) const
{
    return build ().delete_ (std::move (path));
}

request_builder_t client_builder_t::patch (std::string path) const
{
    return build ().patch (std::move (path));
}

request_builder_t client_builder_t::head (std::string path) const
{
    return build ().head (std::move (path));
}

request_builder_t client_builder_t::options (std::string path) const
{
    return build ().options (std::move (path));
}

request_builder_t::request_builder_t (client_t client, http_method_t method, std::string path) :
    _client (std::move (client)), _method (method), _path (std::move (path))
{
    if (_path.empty () || _path.front () != '/') {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "HTTP request path must start with /");
    }
}

request_builder_t &request_builder_t::header (std::string name, std::string value)
{
    require_non_blank (name, "HTTP request header name is required");
    _headers[std::move (name)] = std::move (value);
    return *this;
}

request_builder_t &request_builder_t::query (std::string name, std::string value)
{
    require_non_blank (name, "HTTP request query name is required");
    _query.emplace_back (std::move (name), std::move (value));
    return *this;
}

request_builder_t &request_builder_t::timeout (std::chrono::milliseconds value)
{
    require_positive_timeout (value);
    _timeout = value;
    return *this;
}

request_builder_t &request_builder_t::body (std::string content, std::string content_type)
{
    require_non_blank (content_type, "HTTP request body content type is required");
    _body = std::move (content);
    _headers["content-type"] = std::move (content_type);
    return *this;
}

request_builder_t &
request_builder_t::body_stream (body_stream_provider_t provider, std::string content_type)
{
    if (!provider) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "HTTP request body stream provider is required");
    }
    require_non_blank (content_type, "HTTP request body content type is required");
    _body_provider = std::move (provider);
    _headers["content-type"] = std::move (content_type);
    return *this;
}

request_builder_t &request_builder_t::form (std::string name, std::string value)
{
    require_non_blank (name, "HTTP request form field name is required");
    _form.emplace_back (std::move (name), std::move (value));
    return *this;
}

request_builder_t &request_builder_t::multipart (std::string name, std::string value)
{
    require_non_blank (name, "HTTP request multipart field name is required");
    _multipart.push_back (
      {.name = std::move (name), .filename = {}, .content = std::move (value), .content_type = {}});
    return *this;
}

request_builder_t &request_builder_t::multipart_file (std::string name,
                                                      std::string filename,
                                                      std::string content,
                                                      std::string content_type)
{
    require_non_blank (name, "HTTP request multipart field name is required");
    require_non_blank (filename, "HTTP request multipart filename is required");
    require_non_blank (content_type, "HTTP request multipart content type is required");
    _multipart.push_back ({.name = std::move (name),
                           .filename = std::move (filename),
                           .content = std::move (content),
                           .content_type = std::move (content_type)});
    return *this;
}

std::string request_builder_t::resolve_target () const
{
    if (_query.empty ()) {
        return _path;
    }
    std::string target = _path;
    char separator = target.find ('?') == std::string::npos ? '?' : '&';
    for (const auto &[name, value] : _query) {
        target.push_back (separator);
        target += percent_encode (name);
        target.push_back ('=');
        target += percent_encode (value);
        separator = '&';
    }
    return target;
}

std::pair<std::optional<std::string>, std::map<std::string, std::string>>
request_builder_t::resolve_body_and_headers () const
{
    const int body_sources = (_body ? 1 : 0) + (_body_provider ? 1 : 0) + (!_form.empty () ? 1 : 0)
                             + (!_multipart.empty () ? 1 : 0);
    if (body_sources > 1) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "HTTP request accepts a single body source: body, body_stream, form, or multipart");
    }

    auto headers = _headers;
    if (_body) {
        return {_body, std::move (headers)};
    }

    if (!_form.empty ()) {
        std::string encoded;
        for (const auto &[name, value] : _form) {
            if (!encoded.empty ()) {
                encoded.push_back ('&');
            }
            encoded += percent_encode (name);
            encoded.push_back ('=');
            encoded += percent_encode (value);
        }
        headers["content-type"] = "application/x-www-form-urlencoded";
        return {std::move (encoded), std::move (headers)};
    }

    if (!_multipart.empty ()) {
        const auto boundary = make_multipart_boundary ();
        std::string encoded;
        for (const auto &part : _multipart) {
            encoded += "--" + boundary + "\r\n";
            encoded += "Content-Disposition: form-data; name=\"" + part.name + "\"";
            if (!part.filename.empty ()) {
                encoded += "; filename=\"" + part.filename + "\"";
            }
            encoded += "\r\n";
            if (!part.content_type.empty ()) {
                encoded += "Content-Type: " + part.content_type + "\r\n";
            }
            encoded += "\r\n";
            encoded += part.content;
            encoded += "\r\n";
        }
        encoded += "--" + boundary + "--\r\n";
        headers["content-type"] = "multipart/form-data; boundary=" + boundary;
        return {std::move (encoded), std::move (headers)};
    }

    return {std::nullopt, std::move (headers)};
}

detail::http_request_t
request_builder_t::make_request (std::function<void (std::string_view)> sink) const
{
    auto [body, headers] = resolve_body_and_headers ();
    return {.method = _method,
            .path = resolve_target (),
            .body = std::move (body),
            .body_provider = _body_provider,
            .headers = std::move (headers),
            .timeout = _timeout,
            .sink = std::move (sink)};
}

zlink::framework::task_t<raw_http_response_t>
request_builder_t::dispatch_request (detail::http_request_t request) const
{
    if (!_client._runtime) {
        return zlink::framework::task_t<raw_http_response_t> (
          zlink::framework::detail::boundary_failure<raw_http_response_t> (zlink::framework::detail::boundary_error_t::closed, "HTTP client is not initialized"));
    }

    if (_client._runtime->uses_coroutines ()) {
        return _client._runtime->submit (std::move (request));
    }
    return zlink::framework::task_t<raw_http_response_t> (_client._runtime->execute (request));
}

zlink::framework::task_t<raw_http_response_t> request_builder_t::submit_raw () const
{
    return dispatch_request (make_request (nullptr));
}

zlink::framework::task_t<raw_http_response_t>
request_builder_t::download (std::function<void (std::string_view)> sink) const
{
    if (!sink) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "HTTP request download sink is required");
    }
    return dispatch_request (make_request (std::move (sink)));
}

server_request_builder_t server_client_t::get (std::string path) const
{
    return server_request_builder_t (_client, http_method_t::get, std::move (path),
                                     _execution_turn);
}

server_request_builder_t server_client_t::post (std::string path) const
{
    return server_request_builder_t (_client, http_method_t::post, std::move (path),
                                     _execution_turn);
}

server_request_builder_t server_client_t::put (std::string path) const
{
    return server_request_builder_t (_client, http_method_t::put, std::move (path),
                                     _execution_turn);
}

server_request_builder_t server_client_t::delete_ (std::string path) const
{
    return server_request_builder_t (_client, http_method_t::delete_, std::move (path),
                                     _execution_turn);
}

server_request_builder_t server_client_t::patch (std::string path) const
{
    return server_request_builder_t (_client, http_method_t::patch, std::move (path),
                                     _execution_turn);
}

server_request_builder_t server_client_t::head (std::string path) const
{
    return server_request_builder_t (_client, http_method_t::head, std::move (path),
                                     _execution_turn);
}

server_request_builder_t server_client_t::options (std::string path) const
{
    return server_request_builder_t (_client, http_method_t::options, std::move (path),
                                     _execution_turn);
}

} // namespace zlink::http_client
