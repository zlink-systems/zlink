/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/errors/error.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <set>
#include <vector>

namespace zlink::framework
{

namespace runtime
{
class http_route_invoker_access_t;
} // namespace runtime

enum class http_method_t
{
    get = 0,
    post = 1,
    put = 2,
    delete_ = 3
};

struct http_context_t
{
    http_method_t method = http_method_t::get;
    std::string path;
    std::string correlation_id;
    std::map<std::string, std::string> request_headers;
    std::map<std::string, std::string> response_headers;
    std::optional<std::string> response_body;
    int response_status = 200;

    http_context_t &response_header (std::string name, std::string value)
    {
        response_headers[std::move (name)] = std::move (value);
        return *this;
    }

    http_context_t &json_response (int status, std::string body)
    {
        response_status = status;
        response_body = std::move (body);
        return *this;
    }
};

struct http_request_t
{
    http_method_t method = http_method_t::get;
    std::string path;
    std::string target;
    std::string query_string;
    std::string correlation_id;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> route_values;
    std::map<std::string, std::string> query_values;
    std::string body;
    std::string content_type;
    std::string remote_endpoint;
};

struct http_response_t
{
    int status = 200;
    std::string body;
    std::string content_type = "application/json";
    std::map<std::string, std::string> headers;

    http_response_t &header (std::string name, std::string value)
    {
        headers[std::move (name)] = std::move (value);
        return *this;
    }
};

struct http_tls_options_t
{
    std::string certificate_file;
    std::string private_key_file;
};

struct http_server_options_t
{
    std::size_t max_connections = 1024;
    std::size_t max_request_body_size = 1024 * 1024;
    std::size_t max_header_size = 64 * 1024;
    std::chrono::milliseconds request_headers_timeout{5000};
    std::chrono::milliseconds request_body_timeout{5000};
    std::chrono::milliseconds write_timeout{5000};
    std::chrono::milliseconds keep_alive_timeout{5000};
    std::chrono::milliseconds graceful_shutdown_timeout{5000};
    std::size_t max_keep_alive_requests = 100;
};

struct http_middleware_t
{
    std::string name;
    std::function<std::shared_ptr<void> ()> create_instance;
    std::function<void (service_provider_t &, http_context_t &, const std::shared_ptr<void> &)>
      before;
    std::function<void (service_provider_t &, http_context_t &, const std::shared_ptr<void> &)>
      after;
};

class http_route_t
{
  public:
    http_method_t method;
    std::string path;
    std::string handler_name;
    bool context_response_precedence = false;
    bool validates_json_content_type = true;

  private:
    std::function<task_t<http_response_t> (
      service_provider_t &, http_context_t &, const http_request_t &, const std::string &)>
      invoke;

    friend class http_options_builder_t;
    friend class runtime::http_route_invoker_access_t;
};

struct http_endpoint_t
{
    std::string uri;
    std::optional<http_tls_options_t> tls;
};

struct http_options_snapshot_t
{
    std::vector<http_endpoint_t> endpoints;
    std::vector<http_route_t> routes;
    std::vector<http_middleware_t> middleware;
    http_server_options_t server;
    std::optional<std::string> health_path;
    std::optional<std::string> readiness_path;
    std::optional<std::string> liveness_path;
};

class http_tls_options_builder_t
{
  public:
    explicit http_tls_options_builder_t (http_tls_options_t &options) noexcept : _options (&options)
    {
    }

    http_tls_options_builder_t &certificate_file (std::string path)
    {
        _options->certificate_file = std::move (path);
        return *this;
    }

    http_tls_options_builder_t &private_key_file (std::string path)
    {
        _options->private_key_file = std::move (path);
        return *this;
    }

  private:
    http_tls_options_t *_options;
};

class http_server_options_builder_t
{
  public:
    explicit http_server_options_builder_t (http_server_options_t &options) noexcept :
        _options (&options)
    {
    }

    http_server_options_builder_t &set_max_connections (std::size_t value)
    {
        _options->max_connections = value;
        return *this;
    }

    http_server_options_builder_t &set_max_request_body_size (std::size_t bytes)
    {
        _options->max_request_body_size = bytes;
        return *this;
    }

    http_server_options_builder_t &set_max_header_size (std::size_t bytes)
    {
        _options->max_header_size = bytes;
        return *this;
    }

    http_server_options_builder_t &set_request_headers_timeout (std::chrono::milliseconds value)
    {
        _options->request_headers_timeout = value;
        return *this;
    }

    http_server_options_builder_t &set_request_body_timeout (std::chrono::milliseconds value)
    {
        _options->request_body_timeout = value;
        return *this;
    }

    http_server_options_builder_t &set_write_timeout (std::chrono::milliseconds value)
    {
        _options->write_timeout = value;
        return *this;
    }

    http_server_options_builder_t &set_keep_alive_timeout (std::chrono::milliseconds value)
    {
        _options->keep_alive_timeout = value;
        return *this;
    }

    http_server_options_builder_t &set_graceful_shutdown_timeout (std::chrono::milliseconds value)
    {
        _options->graceful_shutdown_timeout = value;
        return *this;
    }

    http_server_options_builder_t &set_max_keep_alive_requests (std::size_t value)
    {
        _options->max_keep_alive_requests = value;
        return *this;
    }

  private:
    http_server_options_t *_options;
};

class http_options_builder_t
{
  public:
    http_options_builder_t &listen (std::string endpoint)
    {
        if (!starts_with (endpoint, "http://") && !starts_with (endpoint, "https://")) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "HTTP endpoint must start with http:// or https://");
        }
        _snapshot.endpoints.push_back ({.uri = std::move (endpoint)});
        return *this;
    }

    http_options_builder_t &
    configure_tls (std::function<void (http_tls_options_builder_t &)> configure)
    {
        if (_snapshot.endpoints.empty ()) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "HTTP TLS options require a listen endpoint");
        }
        auto &endpoint = _snapshot.endpoints.back ();
        endpoint.tls.emplace ();
        http_tls_options_builder_t builder (*endpoint.tls);
        if (configure) {
            configure (builder);
        }
        return *this;
    }

    http_options_builder_t &
    configure_server (std::function<void (http_server_options_builder_t &)> configure)
    {
        http_server_options_builder_t builder (_snapshot.server);
        if (configure) {
            configure (builder);
        }
        return *this;
    }

    template <typename THandler> http_options_builder_t &map_get (std::string path)
    {
        return add_route<THandler> (http_method_t::get, std::move (path));
    }

    template <typename THandler> http_options_builder_t &map_post (std::string path)
    {
        return add_route<THandler> (http_method_t::post, std::move (path));
    }

    template <typename THandler> http_options_builder_t &map_put (std::string path)
    {
        return add_route<THandler> (http_method_t::put, std::move (path));
    }

    template <typename THandler> http_options_builder_t &map_delete (std::string path)
    {
        return add_route<THandler> (http_method_t::delete_, std::move (path));
    }

    template <typename TMiddleware> http_options_builder_t &use ()
    {
        http_middleware_t middleware;
        middleware.name = typeid (TMiddleware).name ();
        middleware.create_instance = [] () -> std::shared_ptr<void> {
            if constexpr (std::is_default_constructible_v<TMiddleware>) {
                return std::make_shared<TMiddleware> ();
            } else {
                return {};
            }
        };
        middleware.before = [] (service_provider_t &services, http_context_t &context,
                                const std::shared_ptr<void> &instance) {
            invoke_middleware_before<TMiddleware> (services, context, instance);
        };
        middleware.after = [] (service_provider_t &services, http_context_t &context,
                               const std::shared_ptr<void> &instance) {
            invoke_middleware_after<TMiddleware> (services, context, instance);
        };
        _snapshot.middleware.push_back (std::move (middleware));
        return *this;
    }

    http_options_builder_t &map_health (std::string path)
    {
        _snapshot.health_path = validate_system_path (std::move (path));
        return *this;
    }

    http_options_builder_t &map_readiness (std::string path)
    {
        _snapshot.readiness_path = validate_system_path (std::move (path));
        return *this;
    }

    http_options_builder_t &map_liveness (std::string path)
    {
        _snapshot.liveness_path = validate_system_path (std::move (path));
        return *this;
    }

    const http_options_snapshot_t &snapshot () const noexcept { return _snapshot; }

    void validate () const
    {
        std::set<std::string> route_keys;
        std::set<std::string> system_paths;
        auto add_system_path = [&] (const std::optional<std::string> &path) {
            if (!path) {
                return;
            }
            if (!system_paths.insert (*path).second) {
                throw framework_exception_t (framework_error_kind_t::protocol_error,
                                             "duplicate HTTP system route registration");
            }
        };
        add_system_path (_snapshot.health_path);
        add_system_path (_snapshot.readiness_path);
        add_system_path (_snapshot.liveness_path);
        for (const auto &endpoint : _snapshot.endpoints) {
            if (!starts_with (endpoint.uri, "https://")) {
                continue;
            }
            if (!endpoint.tls || endpoint.tls->certificate_file.empty ()
                || endpoint.tls->private_key_file.empty ()) {
                throw framework_exception_t (
                  framework_error_kind_t::protocol_error,
                  "HTTPS endpoint requires TLS certificate and private key");
            }
        }
        for (const auto &route : _snapshot.routes) {
            const auto key = route_key (route.method, route.path);
            if (!route_keys.insert (key).second) {
                throw framework_exception_t (framework_error_kind_t::protocol_error,
                                             "duplicate HTTP route registration");
            }
            if (matches_system_path (route.path)) {
                throw framework_exception_t (framework_error_kind_t::protocol_error,
                                             "HTTP route conflicts with a system health route");
            }
        }
    }

  private:
    friend class zlink_framework_options_t;

    void bind_services (service_collection_t &services, serializer_registry_t &serializers) noexcept
    {
        _services = &services;
        _serializers = &serializers;
    }

    template <typename T> static constexpr bool has_request_type_v = requires
    {
        typename T::request_type;
    };

    template <typename T> static constexpr bool has_reply_type_v = requires
    {
        typename T::reply_type;
    };

    template <typename T>
    static constexpr bool has_raw_http_shape_v = requires (T handler, const http_request_t &request)
    {
        handler.handle (request);
    };

    static bool starts_with (const std::string &value, const char *prefix)
    {
        return value.rfind (prefix, 0) == 0;
    }

    static std::string route_key (http_method_t method, const std::string &path)
    {
        return std::to_string (static_cast<int> (method)) + ":" + path;
    }

    bool matches_system_path (const std::string &path) const
    {
        return (_snapshot.health_path && *_snapshot.health_path == path)
               || (_snapshot.readiness_path && *_snapshot.readiness_path == path)
               || (_snapshot.liveness_path && *_snapshot.liveness_path == path);
    }

    static std::string validate_system_path (std::string path)
    {
        if (path.empty () || path.front () != '/') {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "HTTP route path must start with /");
        }
        return path;
    }

    template <typename TMiddleware>
    static void invoke_middleware_before_instance (TMiddleware &middleware,
                                                   service_provider_t &services,
                                                   http_context_t &context)
    {
        if constexpr (requires (TMiddleware value, http_context_t & ctx) { value.before (ctx); }) {
            middleware.before (context);
        } else if constexpr (requires (TMiddleware value, service_provider_t & provider,
                                       http_context_t & ctx) { value.before (provider, ctx); }) {
            middleware.before (services, context);
        }
    }

    template <typename TMiddleware>
    static void invoke_middleware_after_instance (TMiddleware &middleware,
                                                  service_provider_t &services,
                                                  http_context_t &context)
    {
        if constexpr (requires (TMiddleware value, http_context_t & ctx) { value.after (ctx); }) {
            middleware.after (context);
        } else if constexpr (requires (TMiddleware value, service_provider_t & provider,
                                       http_context_t & ctx) { value.after (provider, ctx); }) {
            middleware.after (services, context);
        }
    }

    template <typename TMiddleware>
    static TMiddleware &resolve_middleware (service_provider_t &services,
                                            const std::shared_ptr<void> &instance)
    {
        if constexpr (std::is_default_constructible_v<TMiddleware>) {
            return *std::static_pointer_cast<TMiddleware> (instance);
        } else {
            return services.get_required<TMiddleware> ();
        }
    }

    template <typename TMiddleware>
    static void invoke_middleware_before (service_provider_t &services,
                                          http_context_t &context,
                                          const std::shared_ptr<void> &instance)
    {
        auto &middleware = resolve_middleware<TMiddleware> (services, instance);
        invoke_middleware_before_instance<TMiddleware> (middleware, services, context);
    }

    template <typename TMiddleware>
    static void invoke_middleware_after (service_provider_t &services,
                                         http_context_t &context,
                                         const std::shared_ptr<void> &instance)
    {
        auto &middleware = resolve_middleware<TMiddleware> (services, instance);
        invoke_middleware_after_instance<TMiddleware> (middleware, services, context);
    }

    template <typename THandler>
    static auto invoke_raw_handler (THandler &handler, const http_request_t &request)
    {
        if constexpr (requires { handler.handle (request); }) {
            return handler.handle (request);
        } else {
            static_assert (sizeof (THandler) == 0,
                           "HTTP raw handler must handle const http_request_t&");
        }
    }

    template <typename THandler, typename TRequest>
    static auto invoke_typed_handler (THandler &handler,
                                      const TRequest &request,
                                      const http_request_t &http,
                                      http_context_t &context)
    {
        if constexpr (requires { handler.handle (request, http, context); }) {
            return handler.handle (request, http, context);
        } else if constexpr (requires { handler.handle (request, http); }) {
            return handler.handle (request, http);
        } else if constexpr (requires { handler.handle (request, context); }) {
            return handler.handle (request, context);
        } else if constexpr (requires { handler.handle (request); }) {
            return handler.handle (request);
        } else {
            static_assert (sizeof (THandler) == 0,
                           "HTTP typed handler has no supported handle overload");
        }
    }

    template <typename TReply, typename TValue>
    static http_response_t response_from_value (TValue &&value,
                                                serializer_registry_t &serializers,
                                                const http_context_t &context)
    {
        using value_type = std::remove_cvref_t<TValue>;
        if constexpr (std::is_same_v<value_type, http_response_t>) {
            return std::forward<TValue> (value);
        } else {
            http_response_t response;
            response.status = context.response_status;
            response.headers = context.response_headers;
            response.body = context.response_body.value_or (
              serializers.template get<TReply> ().serialize (value).to_string ());
            return response;
        }
    }

    template <typename TReply, typename TResult>
    static task_t<http_response_t> resolve_handler_response (TResult &&result,
                                                             serializer_registry_t &serializers,
                                                             const http_context_t &context)
    {
        using result_type = std::remove_cvref_t<TResult>;
        if constexpr (detail::is_task_v<result_type>) {
            using value_type = typename detail::task_value_type_t<result_type>::type;
            static_assert (std::is_same_v<value_type, TReply>
                             || std::is_same_v<value_type, http_response_t>,
                           "HTTP handler task_t<T> must return reply_type or http_response_t");
            auto value = co_await std::forward<TResult> (result);
            co_return response_from_value<TReply> (std::move (value), serializers, context);
        } else {
            using value_type = std::remove_cvref_t<TResult>;
            static_assert (std::is_same_v<value_type, TReply>
                             || std::is_same_v<value_type, http_response_t>,
                           "HTTP handler must return reply_type or http_response_t");
            co_return response_from_value<TReply> (std::forward<TResult> (result), serializers,
                                                   context);
        }
    }

    template <typename TRequest>
    static void validate_request (const TRequest &request, const http_context_t &context)
    {
        if constexpr (requires { request.validate (context); }) {
            request.validate (context);
        } else if constexpr (requires { request.validate (); }) {
            request.validate ();
        }
    }

    template <typename THandler>
    http_options_builder_t &add_route (http_method_t method, std::string path)
    {
        if constexpr (has_request_type_v<THandler>) {
            static_assert (!has_raw_http_shape_v<THandler>,
                           "HTTP handler cannot expose both typed request_type and raw "
                           "http_request_t route shapes");
        }
        register_handler_service<THandler> ();
        register_route_serializers<THandler> ();
        path = validate_system_path (std::move (path));
        http_route_t route;
        route.method = method;
        route.path = std::move (path);
        route.handler_name = typeid (THandler).name ();
        route.validates_json_content_type = has_request_type_v<THandler>;
        auto *serializers = _serializers;
        route.invoke = [serializers] (service_provider_t &services, http_context_t &context,
                                      const http_request_t &http,
                                      const std::string &body) -> task_t<http_response_t> {
            if (serializers == nullptr) {
                throw framework_exception_t (framework_error_kind_t::protocol_error,
                                             "HTTP route serializer registry is not configured");
            }
            try {
                auto &handler = services.get_required<THandler> ();
                if constexpr (has_request_type_v<THandler>) {
                    using request_type = typename THandler::request_type;
                    using reply_type = typename THandler::reply_type;
                    const auto request = serializers->template get<request_type> ().deserialize (
                      encoded_payload_t::from_string (body));
                    validate_request (request, context);
                    auto handler_result = invoke_typed_handler (handler, request, http, context);
                    co_return co_await resolve_handler_response<reply_type> (
                      std::move (handler_result), *serializers, context);
                } else {
                    auto handler_result = invoke_raw_handler (handler, http);
                    co_return co_await resolve_handler_response<http_response_t> (
                      std::move (handler_result), *serializers, context);
                }
            }
            catch (const framework_exception_t &ex) {
                co_return detail::result_access_t::failure<http_response_t> (ex);
            }
            catch (const std::exception &ex) {
                co_return result_t<http_response_t>::failure (
                  framework_error_kind_t::internal_failure, ex.what ());
            }
        };
        _snapshot.routes.push_back (std::move (route));
        return *this;
    }

    template <typename THandler> void register_handler_service ()
    {
        if (_services == nullptr) {
            return;
        }
        if (!_handler_service_types.emplace (std::type_index (typeid (THandler))).second) {
            return;
        }
        if (_services->contains (std::type_index (typeid (THandler)))) {
            return;
        }
        detail::injected_handler_registrar_t<
          THandler, typename detail::handler_dependencies_t<THandler>::type>::add (*_services);
    }

    /* Fulfills route DTO serializer registration at map_* time instead of
     * relying on the lazy get<T>() fallback (HTTP serializer contract). A
     * custom serializer registered beforehand is kept as-is. */
    template <typename T> void register_json_serializer ()
    {
        if (_serializers == nullptr) {
            return;
        }
        if (!_json_serializer_types.emplace (std::type_index (typeid (T))).second) {
            return;
        }
        if (_serializers->contains (std::type_index (typeid (T)))) {
            return;
        }
        if constexpr (detail::is_json_serializer_compatible_v<T>) {
            _serializers->template add<T> (
              [] (const T &value) {
                  return detail::encoded_payload_from_raw (zlink::message_t::from_json (value));
              },
              [] (const encoded_payload_t &payload) {
                  return detail::encoded_payload_to_raw (payload).template parse_json<T> ();
              },
              "application/json");
        }
    }

    template <typename THandler> void register_route_serializers ()
    {
        if constexpr (has_request_type_v<THandler>) {
            register_json_serializer<typename THandler::request_type> ();
            register_json_serializer<typename THandler::reply_type> ();
        }
    }

    http_options_snapshot_t _snapshot;
    service_collection_t *_services = nullptr;
    serializer_registry_t *_serializers = nullptr;
    std::set<std::type_index> _handler_service_types;
    std::set<std::type_index> _json_serializer_types;
};

} // namespace zlink::framework
