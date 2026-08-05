<!-- framework-adapter-nav:start -->
[Spec table of contents](README.en.md) | [Previous: C++ exact interface](interfaces/README.en.md) | [Next: C++ Embedded HTTP Server](61-embedded-http-server.en.md)
<!-- framework-adapter-nav:end -->

[Framework common document](../../../../README.en.md)


# C++ HTTP Hosting Public Contract

> This document is the formal contract C++ HTTP hosting must provide.

## 1. Request Processing Contract

C++ HTTP hosting provides route registration, JSON binding, DI handler
execution, and HTTP response generation as one request processing
flow. The flow below is an example to explain the public hosting
contract, and doesn't use a specific sample as the contract's basis.

```text
HTTP client
  POST /games
      |
      v
C++ HTTP hosting
  options.http().map_post<create_game_http_handler_t>("/games")
      |
      v
DI handler
  create_game_http_handler_t::handle(
    create_game_http_req_t request)
      |
      v
request_client_t.request(play_channel, ...).submit<create_game_res_t>()
      |
      v
HTTP JSON response
  create_game_http_res_t {
    room_id, game_name, owner_play_endpoint,
    play_endpoints, play_nodes, required_level
  }
```

The C++ framework aligns this flow to the following meaning.

- The HTTP server is a hosted service tied to the app lifecycle.
- An HTTP route registers method and path with `map_get`, `map_post`,
  `map_put`, `map_delete` (the same concept as ASP.NET Core Minimal
  API's `MapGet` family; the C++ public surface uses `snake_case` per
  §7's convention).
- An HTTP endpoint supports both `http://` and `https://`.
- A request body converts to a JSON DTO.
- A route handler is resolved from DI.
- A handler is injected `request_client_t` to request a zlink channel.
- A DTO the handler returns becomes the JSON response body.
- A normal response defaults to `200 OK`.
- A payload decode failure is `400 Bad Request`.
- A missing route is `404 Not Found`.
- A handler or zlink request failure maps to HTTP status based on error
  kind.
- App shutdown closes the HTTP accept loop and in-progress request
  dispatch.

HTTP hosting is a JSON API capability the framework server package
provides. MVC/view, static file, template engine, and ORM aren't in
this contract's scope.

## 2. Public API

An ordinary application registers HTTP endpoint and route inside
`app.add_zlink_framework(...)`.

```cpp
auto app = zlink::framework::app_t::create();

app.add_zlink_framework([&](auto &options) {
    auto mesh = options.add_route_mesh(sample_names_t::application_mesh)
      .listen(7300) // opens the endpoint this RouteMesh receives peer messages on.
      .set_routing_id(topology.application_rid); // identifies this node within the same mesh.
    mesh.channel(sample_names_t::api_channel)
      .server() // publishes this node as a request-processing candidate for api_channel.
      .add_handler_group("api"); // connects the DI handler group below to the Channel handler.
    mesh.channel(sample_names_t::play_channel)
      .client(); // registers only the play_channel call path, with no Server membership.

    options.http()
      .listen(topology.api_http_endpoint) // opens the listener an HTTP client connects to.
      .map_post<create_game_http_handler_t>("/games"); // connects POST /games to the DI handler.

    options.handlers()
      .group ("api")
      .add<authenticate_player_handler_t> (); // registers the handler to resolve in the api group.
});
```

`options.http()` doesn't expose a low-level `Boost.Beast`, socket,
acceptor, thread, or executor type.

## 3. Handler Signature Forms

An HTTP handler declares DTO and DI dependency the same way as a
channel handler. But since HTTP needs transport metadata, raw body,
and direct response control, `handle(...)`'s argument and return value
can be declared in multiple forms. Every handler signature form below
is the HTTP hosting public contract.

```cpp
struct create_game_http_req_t {
    static constexpr const char *packet_name = "CreateGameHttpReq";
    std::string game_name;
};

struct create_game_http_res_t {
    static constexpr const char *packet_name = "CreateGameHttpRes";
    std::string room_id;
    std::string game_name;
    std::string owner_play_endpoint;
    std::vector<std::string> play_endpoints;
    std::vector<play_node_info_t> play_nodes;
    int required_level = 0;
};

class create_game_http_handler_t {
public:
    using request_type = create_game_http_req_t;
    using reply_type = create_game_http_res_t;
    using dependency_types =
      zlink::framework::dependency_list_t<
        zlink::framework::request_client_t,
        zlink::framework::logger_t<create_game_http_handler_t>>;

    explicit create_game_http_handler_t(
      zlink::framework::request_client_t &client,
      zlink::framework::logger_t<create_game_http_handler_t> &logger);

    task_t<create_game_http_res_t> handle(const create_game_http_req_t &request);
};
```

A handler's `handle(...)` allows both a sync return and a `task_t<T>`
return. A C++ HTTP handler projects `.NET`'s `Task<IResult>` or
`Task<T>` meaning as `task_t<T>`.

```cpp
task_t<create_game_http_res_t>
create_game_http_handler_t::handle(const create_game_http_req_t &request)
{
    auto room = co_await _client
      .request (sample_names_t::play_channel,
                create_game_req_t{request.game_name})
      .submit<create_game_res_t> ();
    co_return create_game_http_res_t {room.room_id,
                                      room.game_name,
                                      room.owner_play_endpoint,
                                      room.play_endpoints,
                                      room.play_nodes,
                                      room.required_level};
}
```

The `request_type`, `reply_type`, `dependency_types`, `handle(...)`
convention stays the same as a message handler. HTTP doesn't make a
separate constructor injection convention.

The HTTP handler signature forms that must be supported are below.

- typed DTO: `reply_type handle(const request_type &request)`
- typed DTO async: `task_t<reply_type> handle(const request_type &request)`
- typed DTO + context:
  `reply_type handle(const request_type &request, http_context_t &context)`
- typed DTO + context async:
  `task_t<reply_type> handle(const request_type &request, http_context_t &context)`
- typed DTO + request:
  `reply_type handle(const request_type &request, const http_request_t &http)`
- typed DTO + request async:
  `task_t<reply_type> handle(const request_type &request, const http_request_t &http)`
- typed DTO + request + context:
  `reply_type handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- typed DTO + request + context async:
  `task_t<reply_type> handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- typed response: `http_response_t handle(const request_type &request)`
- typed response + context:
  `http_response_t handle(const request_type &request, http_context_t &context)`
- typed response async: `task_t<http_response_t> handle(const request_type &request)`
- typed response + context async:
  `task_t<http_response_t> handle(const request_type &request, http_context_t &context)`
- typed response + request:
  `http_response_t handle(const request_type &request, const http_request_t &http)`
- typed response + request async:
  `task_t<http_response_t> handle(const request_type &request, const http_request_t &http)`
- typed response + request + context:
  `http_response_t handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- typed response + request + context async:
  `task_t<http_response_t> handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- raw HTTP request: `http_response_t handle(const http_request_t &request)`
- raw HTTP request async: `task_t<http_response_t> handle(const http_request_t &request)`

`http_request_t` and `http_response_t` are zlink framework public
types. Directly using a `Boost.Beast`, `Boost.Asio`, OpenSSL stream, or
socket type in a handler signature violates the public contract.

`map_*<THandler>(...)` determines the signature form above at compile
time. If `request_type` exists, it registers as a typed route; if only
`handle(const http_request_t&)` exists, it registers as a raw route.
For a typed route with multiple overloads, argument form is checked
before return type. The form receiving both `http_request_t` and
`http_context_t` is selected first, then `http_request_t`,
`http_context_t`, and DTO-only form in that order.
Providing both a typed route and a raw route signature on one handler
at once makes route mode ambiguous, so it must fail via static
assertion or startup validation.

Handler signature is determined in the following order.

1. If a `request_type` alias exists, it's considered a typed route
   candidate.
2. If there's no `request_type` alias but `handle(const http_request_t&)`
   exists, it's considered a raw route.
3. A typed route must have one of `reply_type` or `http_response_t` as
   return form.
4. It fails if a typed route and raw route signature exist together on
   one handler.
5. If there are multiple signatures inside a typed route, only one is
   selected by the priority below.

Typed route call priority:

1. `handle(const request_type&, const http_request_t&, http_context_t&)`
2. `handle(const request_type&, const http_request_t&)`
3. `handle(const request_type&, http_context_t&)`
4. `handle(const request_type&)`

Each signature's return value must be one of `reply_type`,
`http_response_t`, `task_t<reply_type>`, `task_t<http_response_t>`.
This priority checks argument form before return type. If the same
handler provides both `reply_type handle(request, http, context)` and
`http_response_t handle(request, http)`, the first signature is
selected.
If the selected signature returns `http_response_t`, status/header/body
are controlled directly; if it returns `reply_type`, the serializer
converts the DTO to HTTP body.
The `http_request_t` argument means raw HTTP metadata is needed, so it
takes priority over `http_context_t`.

Raw route call priority:

1. `task_t<http_response_t> handle(const http_request_t&)`
2. `http_response_t handle(const http_request_t&)`

A raw route doesn't require `request_type`, `reply_type` aliases. It
fails if a raw route handler returns a `reply_type` DTO. This is
because a raw route can't have the framework infer a response
serializer.

Handler signatures that must fail:

| Condition | Reason for failure |
|------|-----------|
| `request_type` exists but there's no callable typed `handle(...)` | The route can't be executed |
| `request_type` exists but the DTO return signature has no `reply_type` | The DTO serializer can't be known |
| No `request_type` and no raw `handle(http_request_t)` | Route mode can't be decided |
| Both a typed signature and a raw signature provided at once | Typed/raw route mode is ambiguous |
| A raw handler returns `reply_type` or an arbitrary DTO | A raw response serializer can't be inferred |
| A handler takes a Beast/Asio/OpenSSL type | Violates the public dependency boundary |
| Two or more same-priority overloads are callable | Overload selection is ambiguous |

## 4. Route Builder

The support scope is typed JSON route and raw HTTP route. `GET`,
`POST`, `PUT`, `DELETE` must be registrable with the same convention.

```cpp
namespace zlink::framework {

class http_options_builder_t {
public:
    http_options_builder_t &listen(std::string endpoint);
    http_options_builder_t &configure_tls(
      std::function<void(http_tls_options_builder_t &)> configure);
    http_options_builder_t &configure_server(
      std::function<void(http_server_options_builder_t &)> configure);

    template <typename THandler>
    http_options_builder_t &map_get(std::string path);
    template <typename THandler>
    http_options_builder_t &map_post(std::string path);
    template <typename THandler>
    http_options_builder_t &map_put(std::string path);
    template <typename THandler>
    http_options_builder_t &map_delete(std::string path);

    template <typename TMiddleware>
    http_options_builder_t &use();

    http_options_builder_t &map_health(std::string path);
    http_options_builder_t &map_readiness(std::string path);
    http_options_builder_t &map_liveness(std::string path);
};

struct http_context_t {
    http_method_t method;
    std::string path;
    std::string correlation_id;
    std::map<std::string, std::string> request_headers;
    std::map<std::string, std::string> response_headers;
    std::optional<std::string> response_body;
    int response_status;

    http_context_t &response_header(std::string name, std::string value);
    http_context_t &json_response(int status, std::string body);
};

struct http_request_t {
    http_method_t method;
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

struct http_response_t {
    int status = 200;
    std::string body;
    std::string content_type = "application/json";
    std::map<std::string, std::string> headers;

    http_response_t &header(std::string name, std::string value);
};

} // namespace zlink::framework
```

`http_request_t` field contract:

| Field | Meaning |
|-------|------|
| `method` | The HTTP method used for route matching |
| `path` | Path with query string removed |
| `target` | The original request target. Includes path and query string |
| `query_string` | The query string after `?`. Empty string if absent |
| `correlation_id` | `X-Correlation-Id`, `X-Request-Id`, or a runtime-generated id |
| `headers` | HTTP header name/value. Header name uses the runtime's canonical form |
| `route_values` | `{name}` path segment binding result |
| `query_values` | Query string binding result |
| `body` | Request body that passed limit validation |
| `content_type` | The `Content-Type` header value. Empty string if absent |
| `remote_endpoint` | The client endpoint when known. Empty string if unknown |

`http_response_t` field contract:

| Field | Meaning |
|-------|------|
| `status` | HTTP status code. Default is `200` |
| `body` | Response body bytes. A string is treated as UTF-8 text or a binary-safe byte buffer |
| `content_type` | The `Content-Type` response header. Default is `application/json` |
| `headers` | Response header name/value |

`http_request_t` and `http_response_t` are copies of values the runtime
owns during request processing. A handler must not store a reference
to this object. Its lifetime isn't guaranteed after the request
completes.

The `listen(...)` endpoint uses the `http://host:port` and
`https://host:port` formats.

- `http://127.0.0.1:18080`
- `http://0.0.0.0:18080`
- `http://[::1]:18080`
- `https://127.0.0.1:18443`
- `https://0.0.0.0:18443`
- `https://[::1]:18443`

Using an `https://` endpoint requires setting the TLS server
certificate and private key together. TLS is an HTTP hosting transport
option, and isn't described mixed with the zlink channel transport
configuration.

```cpp
options.http()
  .listen("https://0.0.0.0:8443")
  .configure_tls([](auto &tls) {
      tls.certificate_file("certs/server.crt")
        .private_key_file("certs/server.key");
  })
  .map_post<create_game_http_handler_t>("/games");
```

`map_post<THandler>(path)` performs the following work at once for a
typed route.

- Registers `THandler` to the service collection.
- Registers the JSON serializer for `THandler::request_type` and
  `THandler::reply_type`.
- Registers method and path to the route table.
- Builds a DI scope per request and resolves `THandler`.
- Runs `handle(...)` on the framework handler coroutine executor.
- Serializes the result DTO to the JSON response body.
- If the handler provides `handle(request, http_context_t&)`, it can
  receive HTTP context, such as correlation id and header, together.
  An existing handler that only provides `handle(request)` still works
  as is.

A raw route doesn't require a `request_type` serializer. The runtime
builds `http_request_t` and hands it to the handler, and uses the
`http_response_t` the handler returns as the HTTP response as is. A
raw route also passes through the same middleware, correlation id,
timeout, limit, logging, and metrics policy.

Invoker construction pseudocode is below.

```cpp
template <typename THandler>
http_route_invoker_t make_invoker()
{
    if constexpr (has_request_type<THandler>) {
        static_assert(!has_raw_http_only_shape<THandler>);
        register_json_serializer<typename THandler::request_type>();
        if constexpr (returns_typed_dto<THandler>) {
            register_json_serializer<typename THandler::reply_type>();
        }
        return make_typed_invoker<THandler>();
    } else {
        static_assert(has_raw_http_shape<THandler>);
        return make_raw_invoker<THandler>();
    }
}
```

Typed invoker processing order:

1. Merges body, route value, and query value into one binding JSON.
2. Builds the DTO with the `request_type` serializer.
3. Builds `http_request_t` and `http_context_t`.
4. Calls the handler overload by priority.
5. If the result is `reply_type`, builds the JSON response together
   with `http_context_t`'s status/header.
6. If the result is `http_response_t`, builds the HTTP response based
   on the response object.

Raw invoker processing order:

1. Doesn't check whether content type is JSON.
2. Applies the same body/header/route/query limit.
3. Builds `http_request_t`.
4. Calls the raw handler.
5. Builds the HTTP response based on the returned `http_response_t`.

Response precedence:

| Handler result | Priority |
|----------------|----------|
| Returns `http_response_t` | `http_response_t`'s status/header/content type/body take top priority |
| Returns DTO + `http_context_t::json_response(...)` set | Uses the context's status/body/header |
| Returns DTO + only context header/status set | Uses context status/header + DTO JSON body |
| Only returns DTO | Uses `200 OK`, `application/json`, DTO JSON body |

Middleware `after(...)` runs after the handler result is built. If
`after(...)` adds a response header, it can overwrite an existing
header of the same name. However, `Content-Length` is computed by the
runtime based on the final body, so a handler or middleware doesn't fix
it directly.

Route parameter and query string binding follow a simplified version of
ASP.NET Core model binding.

```cpp
options.http()
  .listen("http://0.0.0.0:8080")
  .map_get<get_game_http_handler_t>("/games/{gameId}");
```

The handler request DTO merges values from body, route, and query. On
conflict, priority is route parameter, query string, body in that
order. This priority is fixed in the startup validation document and
tests.

`use<TMiddleware>()` calls `TMiddleware::before(http_context_t&)` and
`TMiddleware::after(http_context_t&)` before and after the route
handler. A middleware doesn't take a raw Beast request or socket — it
only uses `http_context_t`'s correlation id and framework header map.
If a request has `X-Correlation-Id` or `X-Request-Id`, that value is
sent back as the response's `X-Correlation-Id`; otherwise the runtime
generates a request correlation id.
If a middleware calls `json_response(status, body)` in `before(...)`,
the HTTP runtime returns that JSON response without calling the
handler. This short-circuit path also goes through `after(...)`
middleware, so logging/correlation processing can be put in one place.

`map_health(path)`, `map_readiness(path)`, `map_liveness(path)` expose
the `app.health()` report as an HTTP JSON endpoint. If readiness or
liveness is `unhealthy`, that endpoint returns
`503 Service Unavailable`. This route doesn't require the user to build
a separate handler, and the health aggregation rule is owned by
`contracts/eventing/health.hpp` and the runtime diagnostics
implementation.

## 5. Request / Response Contract

The default typed route's HTTP contract is below.

| Item | Contract |
|------|------|
| Method | `GET`, `POST`, `PUT`, `DELETE` |
| Scheme | `http` or `https` |
| Content type | `application/json` |
| Request body | `THandler::request_type` JSON for a method with a body |
| Route parameter | Binds a `{name}` segment to a request DTO field |
| Query string | Binds `?name=value` to a request DTO field |
| Response body | `THandler::reply_type` JSON |
| Success status | `200 OK` |
| Route not found | `404 Not Found` |
| Method mismatch | `405 Method Not Allowed` |
| Unsupported content type | `400 Bad Request` (`protocol_error`) |
| Invalid JSON | `400 Bad Request` |
| Body limit exceeded | `413 Payload Too Large` |
| Serializer registration | `map_*<THandler>` registers the request/reply JSON serializer |
| Handler failure | Error-kind-based status mapping |

The HTTP response defaults to `Content-Type: application/json`. An
error response is also returned as a JSON object.

```json
{
  "error": "protocol_error",
  "message": "payload deserialization failed"
}
```

## 6. Error Mapping

A framework error kind maps to HTTP status.

| Framework error | HTTP status | Reason |
|-----------------|-------------|------|
| `protocol_error` | `400 Bad Request` | The client body didn't convert to a DTO, or the request meaning doesn't match the framework contract |
| `not_found` | `404 Not Found` | The target route, channel, or service wasn't found |
| `framework_exception_t::code() == std::errc::timed_out` | `504 Gateway Timeout` | A zlink request behind HTTP hosting didn't finish in time |
| `framework_exception_t::code()` indicates a [shutdown](../../../01-glossary.en.md#shutdown) boundary and the host is shutting down | `503 Service Unavailable` | The host is shutting down |
| `internal_failure` | `500 Internal Server Error` | An internal handler or runtime failure |

If the HTTP server runtime detects a request that exceeded the body
size limit, it closes with `413 Payload Too Large`. If a JSON route
receives a content type incompatible with `application/json`, it
throws `protocol_error` and closes with `400 Bad Request`. Both
statuses are a server request validation failure, not a handler
failure.

An exception with an unclear error kind closes with
`500 Internal Server Error` and leaves the cause in log/monitoring. The
HTTP client isn't directly exposed the C++ exception type name.

## 7. Lifecycle

The HTTP server runs as a `hosted_service_t`.

- `app.run(...)` builds the service provider and then starts the HTTP
  hosted service.
- Once `app.stop()` or a signal shutdown comes in, it closes the
  acceptor and doesn't accept a new request.
- It waits for an in-progress request to complete within the graceful
  shutdown timeout.
- It doesn't indefinitely build a new zlink submit during shutdown.

An HTTP endpoint switches to [ready](../../../01-glossary.en.md#ready)
only after the framework client the handler is injected becomes ready.
On shutdown, it first rejects a new HTTP request and gives an
in-progress handler a chance to complete within the messaging drain
deadline.

## 7.1 Middleware

HTTP middleware projects ASP.NET Core's cross-cutting pipeline concept
into C++ form. The core scope is `options.http().use<TMiddleware>()`
and the `http_context_t`-based before/after hook. A per-route filter
type isn't put as a separate public API — middleware checks
method/path to handle it.

The required axes are below.

- Exception filter
- Logging filter
- Validation filter
- Auth filter
- Correlation id filter
- Custom middleware registration

Middleware can use DI. It doesn't expose a `boost::beast`
request/response in the public API — it uses an abstract type like the
framework's `http_context_t`. `http_context_t` provides request id,
method, path, header lookup, response status setting, and short-circuit
JSON response setting.

## 8. Installed Header Boundary

HTTP hosting's public contract is provided by the following installed
headers.

```text
zlink/framework/contracts/http/http.hpp
zlink/framework.hpp
```

The public header doesn't expose a Boost.Beast, Boost.Asio, OpenSSL/SSL
context, TCP socket, acceptor, or request parser type. The application
and extension don't include this implementation dependency — they only
use the framework's `http_request_t`, `http_response_t`, and
`http_context_t`.

## 9. C++ HTTP Hosting Surface

| Capability | C++ framework |
|------|---------------|
| Application creation | `app_t::create()` |
| HTTP endpoint | `options.http().listen(url)` |
| HTTPS certificate | `options.http().listen("https://...").configure_tls(...)` |
| Framework registration | `app.add_zlink_framework(...)` |
| POST route | `options.http().map_post<handler_t>("/games")` |
| GET route | `options.http().map_get<handler_t>("/games/{id}")` |
| Request body binding | Builds `request_type` with the JSON serializer |
| Route/query binding | Merges route parameter and query string into `request_type` |
| Dependency injection | Constructor injection based on `dependency_types` |
| Middleware/filter | `options.http().use<TMiddleware>()` and `http_context_t` |
| Execution interruption | Host shutdown/drain policy. A default cancellation argument isn't exposed on the public handler signature |
| Typed reply | The handler returns a `reply_type` DTO |

## 10. Public Contract Decision

| Item | Contract |
|------|------|
| Package location | HTTP hosting is provided by the framework server package. |
| Implementation dependency exposure | Beast, Asio, and SSL implementation types aren't exposed in the public header and signature. |
| HTTPS/TLS | Certificate and private key configuration are provided as a public option. |
| Route matching | Supports exact path and `{name}` path parameter. |
| Method | Supports `GET`, `POST`, `PUT`, `DELETE` with the same builder convention. |
| Cancellation | Doesn't add a separate cancellation token to the handler signature — applies the request timeout and host drain contract. |
| Response customization | A typed DTO defaults to `200 OK` status. Direct status and header control is provided by a handler that returns `http_response_t`. |
| Server hardening | Follows [Embedded HTTP Server](61-embedded-http-server.en.md)'s limit, timeout, and shutdown contract. |

## 11. Regression Test

The minimal test covers the axes below.

- Contract header compile: `#include <zlink/framework.hpp>` and
  `#include <zlink/framework/contracts/http/http.hpp>`
- Route registry: duplicate registration of the same method/path and
  system route conflict fail startup validation
- Per-method route: `GET`, `POST`, `PUT`, `DELETE`
- Per-scheme listen: `http://`, `https://`
- HTTPS TLS option: a missing certificate/private key fails startup
  validation
- HTTPS loopback: JSON request/response succeeds with a test
  certificate
- HTTP handler e2e: calls `GET`, `POST`, `PUT`, `DELETE` routes with a
  loopback client, verifying DTO binding, DI handler execution, JSON
  response, and status mapping
- Handler signature matrix: calls every sync/async signature of DTO,
  DTO+context, DTO+request, response return, and raw request
- Raw HTTP request: `http_request_t` includes method, target, header,
  route/query, body
- Raw HTTP response: `http_response_t`'s status, header, content type,
  body are returned as is
- Ambiguous handler signature: an ambiguous `handle(...)` combination
  fails via static assertion or startup validation
- Response precedence: fixes the priority of `http_response_t`,
  `json_response`, context header/status, and DTO default response
- Route parameter binding: assigns `/games/{gameId}` to a DTO field
- Query string binding: assigns `?page=1` to a DTO field
- Fixes body/route/query merge priority
- JSON binding: converts request body to DTO and returns reply DTO as
  JSON
- DI: an HTTP handler receives `request_client_t` through constructor
  injection
- Middleware: before hook registration order, after hook reverse-order
  execution, per-request state preservation, short-circuit
- Error mapping: invalid JSON is `400`, unknown route is `404`, timeout
  is `504`
- Server validation: unsupported content type is `400`, body limit
  exceeded is `413`
- Embedded server lifecycle: keep-alive, request timeout, graceful
  shutdown drain, and connection metrics are verified by
  [Embedded HTTP Server](61-embedded-http-server.en.md) regression
  tests
- Lifecycle: `app.stop()` stops accepting a new request and completes
  after draining an in-progress request

Handler signature regression matrix:

| Test | Expectation |
|--------|------|
| DTO sync | `reply_type handle(request)` returns `200` JSON |
| DTO async | `task_t<reply_type> handle(request)` returns JSON after await |
| DTO context sync | Context header/status is reflected in the response |
| DTO context async | The async handler and context change are reflected together |
| DTO request sync | Can read `http_request_t`'s header/query/body |
| DTO request async | The async handler receives `http_request_t` and completes normally |
| Response sync | `http_response_t` status/header/body are returned as is |
| Response context | `http_response_t` takes priority over context body |
| Response request | Reads `http_request_t` and responds with `http_response_t` |
| Raw request sync | Receives raw body with no serializer and responds |
| Raw request async | The raw request async handler completes normally |
| Raw content type | A non-JSON content type is also allowed on a raw route |
| Ambiguous route mode | Fails if a typed signature and raw signature exist on one handler |
| Invalid return type | Fails if a raw route returns a DTO |
| Content length | The `Content-Length` a handler gave is corrected to the runtime's final value |

---
<!-- framework-adapter-nav:bottom:start -->
[Spec table of contents](README.en.md) | [Previous: C++ exact interface](interfaces/README.en.md) | [Next: C++ Embedded HTTP Server](61-embedded-http-server.en.md)
<!-- framework-adapter-nav:bottom:end -->
