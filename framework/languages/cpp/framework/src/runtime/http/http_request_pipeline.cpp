/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/http/http_request_pipeline.hpp"
#include "runtime/configuration/service_scope.hpp"

#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/offload_executor.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#ifdef ZLINK_FRAMEWORK_HTTP_WITH_OPENSSL
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/ssl.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{
class http_route_invoker_access_t
{
  public:
    static task_t<http_response_t> invoke (offload_executor_t &handler_executor,
                                           const http_route_t &route,
                                           service_provider_t &services,
                                           http_context_t &context,
                                           const http_request_t &request,
                                           const std::string &body)
    {
        (void) handler_executor;
        return handler_coroutine_executor ().submit<http_response_t> (
          [&route, &services, &context, owned_request = request,
           owned_body = body] () mutable -> boost::asio::awaitable<result_t<http_response_t>> {
              try {
                  co_return co_await await_task_result (
                    route.invoke (services, context, owned_request, owned_body));
              }
              catch (const framework_exception_t &error) {
                  co_return detail::result_access_t::failure<http_response_t> (error);
              }
              catch (...) {
                  co_return result_t<http_response_t>::failure (
                    framework_error_kind_t::internal_failure, "HTTP route handler threw an exception");
              }
          });
    }
};

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

std::atomic_uint64_t g_correlation_sequence{1};

struct matched_route_t
{
    const http_route_t *route = nullptr;
    std::map<std::string, std::string> route_values;
    std::map<std::string, std::string> query_values;
    bool method_not_allowed = false;
};

struct middleware_invocation_t
{
    const http_middleware_t *middleware = nullptr;
    std::shared_ptr<void> instance;
};

enum class health_route_kind_t
{
    health,
    readiness,
    liveness
};

bool starts_with (const std::string &value, const char *prefix)
{
    return value.rfind (prefix, 0) == 0;
}

parsed_http_endpoint_t parse_http_endpoint (const std::string &uri)
{
    parsed_http_endpoint_t parsed;
    std::string rest;
    if (starts_with (uri, "http://")) {
        parsed.scheme = "http";
        rest = uri.substr (7);
    } else if (starts_with (uri, "https://")) {
        parsed.scheme = "https";
        rest = uri.substr (8);
    } else {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "HTTP endpoint must start with http:// or https://");
    }

    const auto slash = rest.find ('/');
    const auto authority = slash == std::string::npos ? rest : rest.substr (0, slash);
    if (!authority.empty () && authority.front () == '[') {
        const auto close = authority.find (']');
        if (close == std::string::npos) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "HTTP endpoint IPv6 host is missing ]");
        }
        parsed.host = authority.substr (1, close - 1);
        if (close + 1 < authority.size ()) {
            if (authority[close + 1] != ':') {
                throw framework_exception_t (framework_error_kind_t::protocol_error,
                                             "HTTP endpoint has invalid IPv6 authority");
            }
            parsed.port = authority.substr (close + 2);
        } else {
            parsed.port = parsed.scheme == "https" ? "443" : "80";
        }
    } else {
        const auto colon = authority.rfind (':');
        if (colon == std::string::npos) {
            parsed.host = authority;
            parsed.port = parsed.scheme == "https" ? "443" : "80";
        } else {
            parsed.host = authority.substr (0, colon);
            parsed.port = authority.substr (colon + 1);
        }
    }
    if (parsed.host.empty () || parsed.port.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "HTTP endpoint requires host and port");
    }
    return parsed;
}

std::optional<http_method_t> from_beast_method (http::verb method)
{
    switch (method) {
        case http::verb::get:
            return http_method_t::get;
        case http::verb::post:
            return http_method_t::post;
        case http::verb::put:
            return http_method_t::put;
        case http::verb::delete_:
            return http_method_t::delete_;
        default:
            return std::nullopt;
    }
}

bool is_hex (char value) noexcept
{
    return std::isxdigit (static_cast<unsigned char> (value)) != 0;
}

int hex_value (char value) noexcept
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return 10 + value - 'a';
    }
    if (value >= 'A' && value <= 'F') {
        return 10 + value - 'A';
    }
    return 0;
}

std::string url_decode (std::string_view value)
{
    std::string decoded;
    decoded.reserve (value.size ());
    for (std::size_t index = 0; index < value.size (); ++index) {
        if (value[index] == '+') {
            decoded.push_back (' ');
            continue;
        }
        if (value[index] == '%' && index + 2 < value.size () && is_hex (value[index + 1])
            && is_hex (value[index + 2])) {
            decoded.push_back (static_cast<char> ((hex_value (value[index + 1]) << 4)
                                                  | hex_value (value[index + 2])));
            index += 2;
            continue;
        }
        decoded.push_back (value[index]);
    }
    return decoded;
}

std::vector<std::string> split_path (const std::string &path)
{
    std::vector<std::string> parts;
    std::istringstream stream (path);
    std::string part;
    while (std::getline (stream, part, '/')) {
        if (!part.empty ()) {
            parts.push_back (part);
        }
    }
    return parts;
}

std::map<std::string, std::string> parse_query (const std::string &target)
{
    std::map<std::string, std::string> values;
    const auto query_start = target.find ('?');
    if (query_start == std::string::npos) {
        return values;
    }

    std::string query = target.substr (query_start + 1);
    std::istringstream stream (query);
    std::string pair;
    while (std::getline (stream, pair, '&')) {
        if (pair.empty ()) {
            continue;
        }
        const auto separator = pair.find ('=');
        if (separator == std::string::npos) {
            values[url_decode (pair)] = "true";
            continue;
        }
        values[url_decode (std::string_view (pair).substr (0, separator))] =
          url_decode (std::string_view (pair).substr (separator + 1));
    }
    return values;
}

bool header_name_equals (std::string_view left, std::string_view right)
{
    if (left.size () != right.size ()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size (); ++index) {
        if (std::tolower (static_cast<unsigned char> (left[index]))
            != std::tolower (static_cast<unsigned char> (right[index]))) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> find_header (const std::map<std::string, std::string> &headers,
                                        std::string_view name)
{
    for (const auto &[key, value] : headers) {
        if (header_name_equals (key, name)) {
            return value;
        }
    }
    return std::nullopt;
}

bool content_type_is_json (std::string value)
{
    const auto separator = value.find (';');
    if (separator != std::string::npos) {
        value.erase (separator);
    }
    while (!value.empty () && std::isspace (static_cast<unsigned char> (value.front ()))) {
        value.erase (value.begin ());
    }
    while (!value.empty () && std::isspace (static_cast<unsigned char> (value.back ()))) {
        value.pop_back ();
    }
    return header_name_equals (value, "application/json");
}

void validate_json_content_type (const http::request<http::string_body> &request,
                                 const http_context_t &context)
{
    if (request.body ().empty ()) {
        return;
    }
    const auto content_type = find_header (context.request_headers, "content-type");
    if (!content_type || !content_type_is_json (*content_type)) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "unsupported content type");
    }
}

bool route_matches (const std::string &pattern,
                    const std::string &target,
                    std::map<std::string, std::string> &route_values)
{
    if (pattern == target) {
        return true;
    }
    const auto pattern_parts = split_path (pattern);
    const auto target_parts = split_path (target);
    if (pattern_parts.size () != target_parts.size ()) {
        return false;
    }
    for (std::size_t index = 0; index < pattern_parts.size (); ++index) {
        const auto &pattern_part = pattern_parts[index];
        const auto &target_part = target_parts[index];
        const bool parameter =
          pattern_part.size () >= 3 && pattern_part.front () == '{' && pattern_part.back () == '}';
        if (parameter) {
            route_values[pattern_part.substr (1, pattern_part.size () - 2)] =
              url_decode (target_part);
            continue;
        }
        if (pattern_part != target_part) {
            return false;
        }
    }
    return true;
}

matched_route_t find_route (const http_options_snapshot_t &options,
                            std::optional<http_method_t> method,
                            const std::string &target)
{
    const auto query = target.find ('?');
    const auto path = query == std::string::npos ? target : target.substr (0, query);
    bool path_matches = false;
    for (const auto &route : options.routes) {
        std::map<std::string, std::string> route_values;
        if (!route_matches (route.path, path, route_values)) {
            continue;
        }
        path_matches = true;
        if (method && route.method == *method) {
            return matched_route_t{&route, std::move (route_values), parse_query (target)};
        }
    }
    matched_route_t match;
    match.method_not_allowed = path_matches;
    return match;
}

const char *status_name (health_status_t status) noexcept
{
    switch (status) {
        case health_status_t::healthy:
            return "healthy";
        case health_status_t::degraded:
            return "degraded";
        case health_status_t::unhealthy:
            return "unhealthy";
    }
    return "unhealthy";
}

nlohmann::json to_json (const health_check_result_t &check)
{
    return nlohmann::json{{"name", check.name},
                          {"component", check.component},
                          {"status", status_name (check.status)},
                          {"message", check.message}};
}

std::optional<health_route_kind_t> match_health_route (const http_options_snapshot_t &options,
                                                       std::optional<http_method_t> method,
                                                       const std::string &target)
{
    if (!method || *method != http_method_t::get) {
        return std::nullopt;
    }
    const auto query = target.find ('?');
    const auto path = query == std::string::npos ? target : target.substr (0, query);
    if (options.health_path && *options.health_path == path) {
        return health_route_kind_t::health;
    }
    if (options.readiness_path && *options.readiness_path == path) {
        return health_route_kind_t::readiness;
    }
    if (options.liveness_path && *options.liveness_path == path) {
        return health_route_kind_t::liveness;
    }
    return std::nullopt;
}

bool system_route_path_exists (const http_options_snapshot_t &options, const std::string &target)
{
    const auto query = target.find ('?');
    const auto path = query == std::string::npos ? target : target.substr (0, query);
    return (options.health_path && *options.health_path == path)
           || (options.readiness_path && *options.readiness_path == path)
           || (options.liveness_path && *options.liveness_path == path);
}

std::string bind_http_request_body (const std::string &body,
                                    const std::map<std::string, std::string> &route_values,
                                    const std::map<std::string, std::string> &query_values)
{
    nlohmann::json json = nlohmann::json::object ();
    if (!body.empty ()) {
        try {
            json = nlohmann::json::parse (body);
            if (!json.is_object ()) {
                throw framework_exception_t (framework_error_kind_t::protocol_error,
                                             "HTTP JSON body must be an object");
            }
        }
        catch (const framework_exception_t &) {
            throw;
        }
        catch (const std::exception &ex) {
            throw framework_exception_t (framework_error_kind_t::protocol_error, ex.what ());
        }
    }
    for (const auto &[key, value] : query_values) {
        json[key] = value;
    }
    for (const auto &[key, value] : route_values) {
        json[key] = value;
    }
    return json.dump ();
}

std::map<std::string, std::string> copy_headers (const http::request<http::string_body> &request)
{
    std::map<std::string, std::string> headers;
    for (const auto &field : request) {
        headers.emplace (std::string (field.name_string ()), std::string (field.value ()));
    }
    return headers;
}

http_context_t make_context (std::optional<http_method_t> method,
                             const http::request<http::string_body> &request)
{
    http_context_t context;
    context.method = method.value_or (http_method_t::get);
    const auto target = std::string (request.target ());
    const auto query = target.find ('?');
    context.path = query == std::string::npos ? target : target.substr (0, query);
    context.request_headers = copy_headers (request);
    if (auto correlation_id = find_header (context.request_headers, "x-correlation-id")) {
        context.correlation_id = *correlation_id;
    } else if (auto request_id = find_header (context.request_headers, "x-request-id")) {
        context.correlation_id = *request_id;
    } else {
        context.correlation_id =
          "http-"
          + std::to_string (g_correlation_sequence.fetch_add (1, std::memory_order_relaxed));
    }
    context.response_header ("X-Correlation-Id", context.correlation_id);
    return context;
}

http_request_t make_http_request (const http_context_t &context,
                                  const http::request<http::string_body> &request,
                                  std::map<std::string, std::string> route_values,
                                  std::map<std::string, std::string> query_values)
{
    http_request_t http_request;
    http_request.method = context.method;
    http_request.path = context.path;
    http_request.target = std::string (request.target ());
    const auto query = http_request.target.find ('?');
    http_request.query_string =
      query == std::string::npos ? std::string{} : http_request.target.substr (query + 1);
    http_request.correlation_id = context.correlation_id;
    http_request.headers = context.request_headers;
    http_request.route_values = std::move (route_values);
    http_request.query_values = std::move (query_values);
    http_request.body = request.body ();
    if (const auto content_type = find_header (http_request.headers, "content-type")) {
        http_request.content_type = *content_type;
    }
    return http_request;
}

void apply_context_response (http::response<http::string_body> &response,
                             const http_context_t &context,
                             bool allow_status_override = true)
{
    if (allow_status_override && context.response_status >= 100 && context.response_status <= 599) {
        response.result (static_cast<unsigned> (context.response_status));
    }
    for (const auto &[name, value] : context.response_headers) {
        response.set (name, value);
    }
}

void apply_http_response (http::response<http::string_body> &response,
                          const http_response_t &source,
                          const http_context_t &context,
                          bool context_response_precedence)
{
    response.result (static_cast<unsigned> (source.status));
    response.set (http::field::content_type, source.content_type);
    response.body () = source.body;
    for (const auto &[name, value] : source.headers) {
        response.set (name, value);
    }
    apply_context_response (response, context, context_response_precedence);
}

const char *error_kind_name (framework_error_kind_t kind) noexcept
{
    switch (kind) {
        case framework_error_kind_t::not_found:
            return "not_found";
        case framework_error_kind_t::already_exists:
            return "already_exists";
        case framework_error_kind_t::type_mismatch:
            return "type_mismatch";
        case framework_error_kind_t::not_configured:
            return "not_configured";
        case framework_error_kind_t::rejected:
            return "rejected";
        case framework_error_kind_t::unavailable:
            return "unavailable";
        case framework_error_kind_t::capacity_exceeded:
            return "capacity_exceeded";
        case framework_error_kind_t::deadline_exceeded:
            return "deadline_exceeded";
        case framework_error_kind_t::shutting_down:
            return "shutting_down";
        case framework_error_kind_t::protocol_error:
            return "protocol_error";
        case framework_error_kind_t::invalid_operation:
            return "invalid_operation";
        case framework_error_kind_t::data_lost:
            return "data_lost";
        case framework_error_kind_t::internal_failure:
            return "internal_failure";
    }
    return "internal_failure";
}

http::status status_for_error (framework_error_kind_t kind) noexcept
{
    switch (kind) {
        case framework_error_kind_t::protocol_error:
        case framework_error_kind_t::type_mismatch:
        case framework_error_kind_t::invalid_operation:
            return http::status::bad_request;
        case framework_error_kind_t::not_found:
            return http::status::not_found;
        case framework_error_kind_t::already_exists:
            return http::status::conflict;
        case framework_error_kind_t::rejected:
            return http::status::forbidden;
        case framework_error_kind_t::not_configured:
        case framework_error_kind_t::unavailable:
        case framework_error_kind_t::shutting_down:
            return http::status::service_unavailable;
        case framework_error_kind_t::deadline_exceeded:
            return http::status::gateway_timeout;
        case framework_error_kind_t::capacity_exceeded:
            return http::status::too_many_requests;
        default:
            return http::status::internal_server_error;
    }
}

void apply_framework_error (http::response<http::string_body> &response,
                            const framework_exception_t &error,
                            const http_context_t &context)
{
    const auto boundary = detail::boundary_state (error);
    auto status = status_for_error (error.kind ());
    const char *error_name = error_kind_name (error.kind ());
    switch (boundary) {
        case detail::boundary_error_t::timed_out:
            status = http::status::gateway_timeout;
            break;
        case detail::boundary_error_t::shutdown:
            status = http::status::service_unavailable;
            break;
        case detail::boundary_error_t::disconnected:
        case detail::boundary_error_t::closed:
        case detail::boundary_error_t::cancelled:
        case detail::boundary_error_t::stale_generation:
            break;
        case detail::boundary_error_t::none:
            break;
    }
    response.result (status);
    response.body () = nlohmann::json{{"error", error_name},
                                      {"message", error.what ()},
                                      {"correlationId", context.correlation_id}}
                         .dump ();
    apply_context_response (response, context, false);
}

http::response<http::string_body>
make_health_response (health_builder_t &health,
                      health_route_kind_t kind,
                      const http::request<http::string_body> &request,
                      http_context_t &context)
{
    const auto report = health.report ();
    health_status_t status = report.status;
    if (kind == health_route_kind_t::readiness) {
        status = report.readiness;
    } else if (kind == health_route_kind_t::liveness) {
        status = report.liveness;
    }

    nlohmann::json checks = nlohmann::json::array ();
    for (const auto &check : report.checks) {
        checks.push_back (to_json (check));
    }
    nlohmann::json body{{"status", status_name (report.status)},
                        {"readiness", status_name (report.readiness)},
                        {"liveness", status_name (report.liveness)},
                        {"checks", checks}};

    http::response<http::string_body> response{
      status == health_status_t::unhealthy ? http::status::service_unavailable : http::status::ok,
      request.version ()};
    response.set (http::field::content_type, "application/json");
    response.body () = body.dump ();
    apply_context_response (response, context, false);
    response.prepare_payload ();
    return response;
}

http::response<http::string_body>
make_json_response (http::status status, unsigned version, std::string body)
{
    http::response<http::string_body> response{status, version};
    response.set (http::field::content_type, "application/json");
    response.body () = std::move (body);
    return response;
}

void apply_route_miss_response (http::response<http::string_body> &response,
                                const matched_route_t &match,
                                const http_context_t &context)
{
    if (match.method_not_allowed) {
        response.result (http::status::method_not_allowed);
        response.body () = R"({"error":"method not allowed"})";
    } else {
        response.result (http::status::not_found);
        response.body () = R"({"error":"route not found"})";
    }
    apply_context_response (response, context, false);
}

void run_route_after_middleware (const std::vector<middleware_invocation_t> &middleware_invocations,
                                 service_provider_t &request_services,
                                 http_context_t &context)
{
    for (auto it = middleware_invocations.rbegin (); it != middleware_invocations.rend (); ++it) {
        if (it->middleware != nullptr && it->middleware->after) {
            it->middleware->after (request_services, context, it->instance);
        }
    }
}

void apply_unhandled_route_error (http::response<http::string_body> &response,
                                  const std::exception &error,
                                  const http_context_t &context)
{
    response.result (http::status::internal_server_error);
    response.body () = nlohmann::json{{"error", "request_failed"},
                                      {"message", error.what ()},
                                      {"correlationId", context.correlation_id}}
                         .dump ();
    apply_context_response (response, context, false);
}

void invoke_matched_route (http::response<http::string_body> &response,
                           const http_options_snapshot_t &options,
                           service_provider_t &services,
                           http_context_t &context,
                           offload_executor_t &handler_executor,
                           const http::request<http::string_body> &request,
                           const matched_route_t &match)
{
    auto request_scope = detail::service_scope_t::create (
      services, detail::service_scope_kind_t::handler_invocation);
    auto &request_services = request_scope.provider ();
    std::vector<middleware_invocation_t> middleware_invocations;
    middleware_invocations.reserve (options.middleware.size ());
    bool after_middleware_ran = false;
    bool context_short_circuit = false;
    auto run_after_middleware_once = [&] () {
        if (after_middleware_ran) {
            return;
        }
        after_middleware_ran = true;
        run_route_after_middleware (middleware_invocations, request_services, context);
    };

    try {
        for (const auto &middleware : options.middleware) {
            auto &invocation = middleware_invocations.emplace_back ();
            invocation.middleware = &middleware;
            if (middleware.create_instance) {
                invocation.instance = middleware.create_instance ();
            }
            if (middleware.before) {
                middleware.before (request_services, context, invocation.instance);
            }
        }
        if (context.response_body) {
            context_short_circuit = true;
            response.body () = *context.response_body;
        } else {
            std::string bound_body = request.body ();
            if (match.route->validates_json_content_type) {
                validate_json_content_type (request, context);
                bound_body =
                  bind_http_request_body (request.body (), match.route_values, match.query_values);
            }
            const auto http_request =
              make_http_request (context, request, match.route_values, match.query_values);
            auto route_result =
              http_route_invoker_access_t::invoke (handler_executor, *match.route,
                                                   request_services, context, http_request,
                                                   bound_body)
                .result ();
            if (!route_result) {
                throw *route_result.error ();
            }
            apply_http_response (response, route_result.value (), context,
                                 match.route->context_response_precedence);
        }
        run_after_middleware_once ();
        apply_context_response (response, context,
                                context_short_circuit || match.route->context_response_precedence);
    }
    catch (const framework_exception_t &ex) {
        run_after_middleware_once ();
        apply_framework_error (response, ex, context);
    }
    catch (const std::exception &ex) {
        run_after_middleware_once ();
        apply_unhandled_route_error (response, ex, context);
    }
}

http::response<http::string_body>
handle_http_request (const http_options_snapshot_t &options,
                     service_provider_t &services,
                     health_builder_t &health,
                     offload_executor_t &handler_executor,
                     const http::request<http::string_body> &request)
{
    const auto method = from_beast_method (request.method ());
    auto context = make_context (method, request);
    if ((!method || *method != http_method_t::get)
        && system_route_path_exists (options, std::string (request.target ()))) {
        auto response = make_json_response (http::status::method_not_allowed, request.version (),
                                            R"({"error":"method not allowed"})");
        apply_context_response (response, context, false);
        response.prepare_payload ();
        return response;
    }
    if (const auto health_route =
          match_health_route (options, method, std::string (request.target ()))) {
        return make_health_response (health, *health_route, request, context);
    }
    const auto match = find_route (options, method, std::string (request.target ()));
    auto response = make_json_response (http::status::ok, request.version (), {});
    if (match.route == nullptr) {
        apply_route_miss_response (response, match, context);
    } else {
        invoke_matched_route (response, options, services, context, handler_executor, request,
                              match);
    }
    response.prepare_payload ();
    return response;
}

http::response<http::string_body>
make_http_status_response (http::status status, unsigned version, std::string body, bool keep_alive)
{
    http::response<http::string_body> response{status, version};
    response.set (http::field::content_type, "application/json");
    response.keep_alive (keep_alive);
    response.body () = std::move (body);
    response.prepare_payload ();
    return response;
}

http::response<http::string_body> make_http_parser_error_response (beast::error_code ec,
                                                                   unsigned version)
{
    if (ec == http::error::body_limit) {
        return make_http_status_response (http::status::payload_too_large, version,
                                          R"({"error":"request body too large"})", false);
    }
    if (ec == http::error::header_limit) {
        return make_http_status_response (http::status::request_header_fields_too_large, version,
                                          R"({"error":"request header too large"})", false);
    }
    return make_http_status_response (http::status::bad_request, version,
                                      R"({"error":"bad request"})", false);
}

} // namespace zlink::framework::runtime
