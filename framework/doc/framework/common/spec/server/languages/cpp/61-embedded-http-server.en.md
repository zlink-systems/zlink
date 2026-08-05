<!-- framework-adapter-nav:start -->
[Spec table of contents](README.en.md) | [Previous: C++ HTTP Hosting](60-http-hosting.en.md)
<!-- framework-adapter-nav:end -->

[Framework common document](../../../../README.en.md)


# C++ Embedded HTTP Server Public Contract

> This document organizes the public contract of the embedded HTTP
> server the `C++` framework provides.
>
> While [HTTP Hosting](60-http-hosting.en.md) covers the route,
> handler, and DTO binding surface the user sees, this document covers
> the endpoint, connection lifecycle, timeout, TLS, shutdown, and
> observability contract.

## 1. Choices Fixed In The Public Contract

`ZLink Framework for C++` can provide a backend API without a separate
HTTP framework. The public application model provides route mapping,
typed DTO binding, middleware, and lifecycle in C++ style.

The core decisions are below.

- The public API is owned by zlink.
- Route mapping uses `map_get`, `map_post`, `map_put`, `map_delete`,
  where the HTTP method is visible.
- The server endpoint is expressed with `listen(...)`, intuitive to a
  C++ user.
- A `Boost.Beast`, `Boost.Asio`, OpenSSL stream, socket, or acceptor
  type isn't exposed in the public header.
- The embedded server provides an HTTP/1.1 backend API server
  contract.
- HTTP/2, HTTP/3, WebSocket, static file server, template engine, and
  ORM aren't in this contract's support scope.

## 2. Embedded Server Capability

Registering an endpoint with `options.http().listen(...)` starts and
stops the server in step with the application host lifecycle. The
embedded server provides the following capability.

- `http://`, `https://` endpoint parse
- TCP listen and HTTP request receipt
- TLS handshake for an HTTPS endpoint
- Per-connection request loop and keep-alive
- Request header/body/write/keep-alive timeout
- Request body size limit and header size limit
- Max connections bound and overload handling
- HTTP method- and path-based route matching
- Path parameter and query parameter binding
- Health/readiness/liveness route
- Per-request DI scope creation
- Middleware before/after execution
- Sync/async handler call
- Maps framework error kind to HTTP status and JSON error body
- HTTP response write
- Stops accepting a new request and drains an in-progress request on
  application shutdown

The embedded HTTP host provides the following capability as the
backend API framework's default server.

- Connection/request observability extension point
- Standardization of request logging and correlation id
- `400 Bad Request` for a malformed request
- Server option startup validation

## 3. Public API

The HTTP server API mixes with the route handler API but hides the
internal server implementation type.

```cpp
app.add_zlink_framework ([&] (auto &options) {
    options.http ()
      .listen ("https://0.0.0.0:8443")
      .configure_tls ([] (auto &tls) {
          tls.certificate_file ("cert.pem")
             .private_key_file ("key.pem");
      })
      .configure_server ([] (auto &server) {
          server.set_max_connections (4096)
                .set_max_request_body_size (1024 * 1024)
                .set_request_headers_timeout (std::chrono::seconds (15))
                .set_keep_alive_timeout (std::chrono::seconds (60));
      })
      .map_get<get_game_handler_t> ("/games/{id}")
      .map_post<create_game_http_handler_t> ("/games")
      .map_put<update_game_handler_t> ("/games/{id}")
      .map_delete<delete_game_handler_t> ("/games/{id}");
});
```

The example above is the formal public API including the embedded HTTP
server option. `map_get`, `map_post`, `map_put`, `map_delete`,
`listen`, `configure_tls`, `configure_server` follow the
[framework option builder naming](../../../../../../../../doc/principal/framework-option-builder-naming.ko.md)
principle.

This surface follows the rules below.

- A C++ public method uses `snake_case`.
- Route mapping follows the `.NET` Minimal API concept.
- Endpoint listen configuration uses `listen(...)` to read as C++
  server configuration.
- The TLS configuration area is opened with `configure_tls(...)`.
- TLS configuration is applied to the last `listen(...)` endpoint, or
  clearly bundled by returning an endpoint builder.
- The server runtime configuration area is opened with
  `configure_server(...)`.
- A server option applies to the HTTP server runtime, not the route
  handler.
- Logger, DI, serializer, and monitoring connect to a separate
  framework surface, and the HTTP server isn't exposed as its own
  independent framework.

## 4. Endpoint And TLS

An endpoint is the `http://host:port` or `https://host:port` format.
If port is omitted, it's filled with the default port by scheme
(`http` 80, `https` 443). If host is absent, it fails at startup
validation.

TLS is a per-endpoint configuration. An HTTPS endpoint needs a
certificate and private key. An HTTPS endpoint with no TLS
configuration must fail before options apply or hosted service start,
not after runtime start.

The TLS contract is below.

- Confirms certificate/private key file presence at startup.
- Puts a TLS handshake timeout.
- Aggregates a TLS handshake failure as a connection error, not a
  request handler error.
- The public API doesn't expose an OpenSSL or Asio SSL type.

## 5. Connection Lifecycle

The connection lifecycle guarantees the following public behavior.

- An HTTP/1.1 connection with keep-alive on processes multiple
  requests.
- A new connection exceeding `max_connections` ends with the overload
  result set in the server option.
- No new connection is accepted after
  [shutdown](../../../01-glossary.en.md#shutdown) starts.
- During shutdown drain, an active request waits to complete within
  the timeout.
- A connection is closed once the drain timeout passes.

## 6. Request Processing Flow

The standard request processing flow is below.

```text
Read request
  -> validate method / target / headers
  -> create context
  -> match system route
  -> match user route
  -> create DI scope
  -> run middleware before
  -> bind body / route / query
  -> invoke handler
  -> run middleware after
  -> map response
  -> write response
```

Important rules:

- A malformed request closes with `400 Bad Request`.
- An unsupported method closes with `405 Method Not Allowed`.
- A missing route closes with `404 Not Found`.
- A mismatched content type throws `protocol_error` and closes with
  `400 Bad Request`.
- If body size exceeds the limit, `413 Payload Too Large` is used.
- A handler timeout is identified as
  `framework_error_kind_t::deadline_exceeded`, using
  `504 Gateway Timeout`.
- A new request during shutdown is identified with
  `framework_error_kind_t::shutting_down` and host state, closed with
  `503 Service Unavailable` or the connection closed per the drain
  policy.
- A Framework exception's HTTP status and JSON body are built based on
  `framework_error_kind_t`. `framework_exception_t::code()` is used
  only to leave the platform cause in the log.

## 7. Binding And Handler Integration

The HTTP server doesn't build a new handler model — it uses
[C++ HTTP Hosting](60-http-hosting.en.md)'s handler signature as is.

```cpp
class create_game_http_handler_t {
  public:
    using request_type = create_game_http_req_t;
    using reply_type = create_game_http_res_t;
    using dependency_types =
      zlink::framework::dependency_list_t<
        zlink::framework::request_client_t,
        zlink::framework::logger_t<create_game_http_handler_t>>;

    task_t<create_game_http_res_t> handle (const create_game_http_req_t &request);
};
```

The server runtime handles the following work for a typed route.

- Merges route path and query into the request binding input.
- Deserializes the JSON body to the request DTO.
- Builds a request DI scope.
- Resolves the handler from framework DI.
- Serializes the handler result DTO to the JSON body.

The server runtime handles the following work for a raw route.

- Builds `http_request_t`.
- Copies method, path, target, route value, query value, header, body,
  content type, and remote endpoint into the public framework type.
- Resolves the handler from framework DI.
- Writes the status, header, content type, body of the `http_response_t`
  the handler returned as the HTTP response.

Handler signatures that must be supported:

- `reply_type handle(const request_type &request)`
- `task_t<reply_type> handle(const request_type &request)`
- `reply_type handle(const request_type &request, http_context_t &context)`
- `task_t<reply_type> handle(const request_type &request, http_context_t &context)`
- `reply_type handle(const request_type &request, const http_request_t &http)`
- `task_t<reply_type> handle(const request_type &request, const http_request_t &http)`
- `reply_type handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- `task_t<reply_type> handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- `http_response_t handle(const request_type &request)`
- `http_response_t handle(const request_type &request, http_context_t &context)`
- `task_t<http_response_t> handle(const request_type &request)`
- `task_t<http_response_t> handle(const request_type &request, http_context_t &context)`
- `http_response_t handle(const request_type &request, const http_request_t &http)`
- `task_t<http_response_t> handle(const request_type &request, const http_request_t &http)`
- `http_response_t handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- `task_t<http_response_t> handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- `http_response_t handle(const http_request_t &request)`
- `task_t<http_response_t> handle(const http_request_t &request)`

A handler must not know socket, HTTP parser, Beast request, or TLS
stream. If HTTP detail is needed, use `http_request_t`; if direct
response control is needed, use `http_response_t`.

## 8. Middleware, Filter, Error Boundary

Middleware handles HTTP context. A filter handles handler invocation
and message dispatch policy. The two aren't mixed as the same concept.

HTTP middleware responsibility:

- Correlation id
- Request logging
- Auth header check
- Adding a response header
- Short-circuit response
- An HTTP-only policy such as CORS

Handler/filter responsibility:

- DTO validation
- Handler exception masking
- zlink request failure mapping
- Business-level audit

Middleware `after` must run whenever possible, not just on handler
success but also on the short-circuit, handler exception, and binding
failure path. That way logging/correlation knowledge isn't repeated
per handler.

## 9. Server Options

A server option applies to a whole endpoint or the whole HTTP server.
It doesn't make a route handler repeat the same option.

The public option and default value are below.

| Option | Default | Meaning |
|--------|--------|------|
| `max_connections` | 1,024 | Number of active connections to keep concurrently |
| `max_request_body_size` | 1 MiB | Maximum JSON body size |
| `max_header_size` | 64 KiB | Whole header size limit |
| `request_headers_timeout` | 5s | Header read time limit |
| `request_body_timeout` | 5s | Body read time limit |
| `write_timeout` | 5s | Response write time limit |
| `keep_alive_timeout` | 5s | Time limit waiting for the next request header on a keep-alive connection |
| `graceful_shutdown_timeout` | 5s | Time to wait for an in-progress request to end on shutdown |
| `max_keep_alive_requests` | 100 | Per-connection request count limit |

The public builder name is fixed as below.

```cpp
namespace zlink::framework {

class http_tls_options_builder_t {
public:
    http_tls_options_builder_t &certificate_file(std::string path);
    http_tls_options_builder_t &private_key_file(std::string path);
};

class http_server_options_builder_t {
public:
    http_server_options_builder_t &set_max_connections(std::size_t value);
    http_server_options_builder_t &set_max_request_body_size(std::size_t bytes);
    http_server_options_builder_t &set_max_header_size(std::size_t bytes);
    http_server_options_builder_t &set_request_headers_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_request_body_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_write_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_keep_alive_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_graceful_shutdown_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_max_keep_alive_requests(
      std::size_t value);
};

class http_options_builder_t {
public:
    http_options_builder_t &configure_tls(
      std::function<void(http_tls_options_builder_t &)> configure);

    http_options_builder_t &configure_server(
      std::function<void(http_server_options_builder_t &)> configure);
};

} // namespace zlink::framework
```

If a value isn't changed in `configure_server(...)`, the default above
is used. A startup error for `0` or an out-of-range value follows
[HTTP Hosting](60-http-hosting.en.md)'s validation contract.

## 10. Observability

The embedded server must connect to framework logging, monitoring, and
health.

Logging:

- Request start/end log
- Status code and duration
- Route template
- Correlation id
- Remote endpoint
- Error kind

Monitoring event:

- Listener started/stopped
- Connection accepted/closed
- Request started/completed
- Request rejected
- Timeout
- TLS handshake failed
- Graceful shutdown started/completed

Metrics:

- Active connections
- Total accepted connections
- Rejected connections
- In-flight requests
- Request duration
- Response status count
- Request body bytes

Health:

- A listener bind failure is a startup failure.
- If the started listener count differs from the expected endpoint
  count, it's unhealthy.
- Readiness switches to unhealthy during shutdown.

## 11. Security And Operational Standard

Since the embedded server is a backend API server, it must provide a
basic security boundary.

- Puts a default body limit.
- Puts a header limit.
- Doesn't allow a request read with no timeout.
- Catches a TLS certificate/private key configuration error before
  start.
- An error response doesn't expose a stack trace or internal file
  path.
- Puts a forwarded header policy as a separate option so it can be
  used behind a reverse proxy.
- Doesn't record a sensitive header as is in request logging.

An auth provider isn't in this contract's support scope. Auth uses the
middleware/filter extension point and the header/context API.

## 12. Contract Verification

Required regression tests:

| Test | Expectation |
|--------|------|
| Startup validation | An invalid endpoint, missing TLS file, or duplicate system route fails |
| Route mapping | `map_get/post/put/delete` calls the correct handler |
| Not found | A missing path is `404` |
| Method not allowed | A path exists but a different method is `405` |
| Unsupported media type | An invalid content type on a JSON route is `400` |
| Malformed body | A JSON decode failure is `400` |
| Body limit | Exceeding the limit is `413` |
| Typed handler signature | Calls every sync/async signature of DTO, DTO+context, DTO+request, and response return. |
| Raw request handler | Receives `http_request_t` and responds with `http_response_t` |
| Raw handler no serializer | A raw route registers without a request/reply JSON serializer |
| Ambiguous handler signature | An ambiguous handler signature fails via static assertion or startup validation. |
| Keep-alive | Processes two requests on the same connection |
| Request timeout | A header/body timeout closes the connection and records an event |
| Handler timeout | `504` response |
| Middleware | `after` runs on success, short-circuit, and exception |
| TLS | An HTTPS route succeeds, a TLS configuration error fails |
| Graceful shutdown | Stops a new accept, drains an active request |
| Logging | Records route, status, duration, correlation id |
| Metrics | Updates the request/status/connection counter |
| zlink integration | Uses a channel request or SPOT call from an HTTP handler |

Contract verification is performed from the perspective of a consumer
using the installed public header and package.

## 13. Conformance Standard

For the embedded HTTP server to stand as the backend API framework's
default server, it must satisfy every condition below.

- Keeps the `options.http().listen(...).map_*<THandler>(...)` public
  surface.
- Supports HTTP/1.1, HTTPS, keep-alive, timeout, limit, graceful
  shutdown.
- A handler doesn't need to know a Beast/Asio/TLS type.
- Supports typed DTO, typed response, and raw HTTP request handler
  signatures all together.
- A raw HTTP request handler also only uses `http_request_t` and
  `http_response_t`, and doesn't take a Beast/Asio/TLS type.
- Integrates with the app model, such as logging, monitoring, health,
  DI, and serializer.
- Malformed request, missing route, method mismatch, body limit, and
  handler failure consistently map to status and JSON body.
- A sample and HTTP E2E is verified with a public consumer flow using
  `zlink::http_client`.
- The public header doesn't expose a Boost.Beast, Boost.Asio, or
  OpenSSL implementation type.
