<!-- framework-adapter-nav:start -->
[Document list](../../../../../../README.en.md) | [Next: Spec -- ZLink Framework C++ HTTP Hosting](../../../server/languages/cpp/60-http-hosting.en.md)
<!-- framework-adapter-nav:end -->

[Spec table of contents](../../../../README.en.md)

[C++ Bundle](../../../../../cpp/README.en.md) | [Runtime Architecture](../../../../internals/README.en.md) | [Application Framework](../../../server/languages/cpp/01-system-structure.en.md) | [Framework Interfaces](../../../server/languages/cpp/interfaces/README.en.md) | [HTTP Hosting](../../../server/languages/cpp/60-http-hosting.en.md)

# Spec -- ZLink HTTP Client For C++

> See the [user guide](../../../../../cpp/guide/http-client/README.en.md)
> for a usage-focused document.
> **The language-neutral common contract is owned by the
> [common spec](../../README.en.md)**, and this document describes the
> C++-specific deviation (coroutine execution contract, `delete_`, the
> `result_t` envelope, an OpenSSL-optional build, a self-managed
> connection pool) and implementation detail against the common
> contract.
> The single standard for the actual contract is the common spec +
> `http-client/include/zlink/http_client/**`'s public header and the
> `test_cpp_http_client`, `test_cpp_framework_contract_headers`
> regression tests.

## 1. Purpose

`zlink::http_client` is a separate client-side deliverable for sending
an HTTP request in C++. It's not a JSON-only client — it's a
general-purpose HTTP client that absorbs the complexity of low-level
types and configuration in zlink's call object and fluent builder
style. The typed JSON path (`body(dto)`/`submit<T>()`/`fetch<T>()`) is
a convenience layer laid on top of it.

This client is a consumer that verifies framework HTTP hosting. It
isn't a default dependency of the `zlink::framework` core target, and
the framework public header doesn't include this client.

## 2. Deliverable Boundary

The public contract and runtime implementation split as below.

| Role | Location | Public? |
|------|------|-----------|
| Facade header | `http-client/include/zlink/http_client.hpp` | public |
| Contract header | `http-client/include/zlink/http_client/contracts/*` | public |
| Runtime implementation | `http-client/src/runtime/*` | private |
| Regression test | `http-client/tests/*` | private |
| CMake target | `zlink::http_client` | public target |

The public header doesn't expose a `Boost.Beast`, `Boost.Asio`,
OpenSSL, socket, resolver, request parser, response parser, SSL
stream, or SSL context type.

The currently implemented public deliverable is below.

- `zlink/http_client.hpp`
- `zlink/http_client/contracts/client.hpp`
- `zlink::http_client` CMake target
- `client_t::create(base_url)` or `create().base_url(...)` +
  `.timeout(...).default_header(...).max_response_body_size(...)`
  `.trust_certificate_file(...)`
  `.follow_redirects(...).retry(...).cookies().proxy(...).compression().build()`
- Coroutine execution configuration: `.coroutines()`,
  `.coroutines(resume_scheduler)`,
  `.coroutines(execute_scheduler, resume_scheduler)`
- Server/framework queue adapter: `framework_resume_scheduler_t`
- Auth: `basic_auth(user, password)`, `bearer_token(token)`,
  `proxy_basic_auth(user, password)`, mTLS
  `client_certificate_file(cert, key)`
- The `build()`-omission shortcut for a one-shot request:
  `client_t::create(url).post(...)`
- `get`, `post`, `put`, `delete_`, `patch`, `head`, `options` request
  builder
- `query(name, value)`: a percent-encoded query parameter
- Per-request `timeout(duration)` override
- Body sources (mutually exclusive): typed JSON `body(dto)`, raw
  `body(content, content_type)`, chunked streaming
  `body_stream(provider, content_type)`,
  `form(name, value)` (x-www-form-urlencoded), `multipart(...)`/
  `multipart_file(...)`
- `submit<T>()`, `submit_raw()`, blocking `fetch<T>()` (directly returns
  the typed body, throws on failure)
- The server request builder's one-way `submit()` returns
  `task_t<void>`. This task only delivers async completion and
  failure, not the transport result or admission status.
- `download(sink)`: streams the response body in chunks with no
  buffering
- Connection keep-alive pool: reuses a connection of the same origin
  (+proxy) and automatically retries a dead pooled connection once with
  a fresh connection

## 3. Public API Shape

The basic usage flow is below.

```cpp
auto client = zlink::http_client::client_t::create()
  .base_url(topology.api_http_endpoint)
  .timeout(std::chrono::seconds(3))
  .build();

auto created = co_await client
  .post("/games")
  .body(create_game_http_req_t { .game_name = game_name })
  .submit<create_game_http_res_t>();
```

Internally, typed submit doesn't wait for the raw submit result with
`.result()` — it awaits `task_t` completion. This rule is to keep the
sample handler from blocking the runtime thread when it uses the HTTP
client.

When reusing the same client for multiple requests, build the client
once with `build()` as above and keep it, since `build()` is the point
that creates the runtime (connection pool).

For a one-off request, `build()` can be omitted and
`get`/`post`/`put`/`delete_` called directly on the builder.

```cpp
auto created = zlink::http_client::client_t::create()
  .base_url(topology.api_http_endpoint)
  .post("/games")
  .body(create_game_http_req_t { .game_name = game_name })
  .submit<create_game_http_res_t>()
  .result();
```

This shortcut is safe even if the builder is a temporary object. Since
`request_builder_t` holds the client by value (not a raw pointer), the
on-demand-built client and its runtime are kept alive until the
request finishes.

`submit<T>()`'s result is `result_t<http_response_t<T>>`. That is, it
reaches the typed DTO through `.value().body`, passing through a
success/failure wrapper and the HTTP envelope
(`status`/`headers`/`body`). When only the typed body is immediately
needed, use `fetch<T>()`. `fetch<T>()` unwraps the result and envelope
to directly return the DTO, and throws an exception on failure.

```cpp
auto created = zlink::http_client::client_t::create(topology.api_http_endpoint)
  .post("/games")
  .body(create_game_http_req_t { .game_name = game_name })
  .fetch<create_game_http_res_t>();   // create_game_http_res_t (throws on failure)
```

`fetch<T>()` waits for the result blocking. So use it where blocking is
allowed, such as in tests and client scenarios. Runtime/handler code
`co_await`s `submit<T>()` to avoid blocking the runtime thread.

The general HTTP client capability supports the scope below.

- `GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `HEAD`, `OPTIONS`
- Query parameter (`query(name, value)`, automatic percent-encoding)
- Request body: typed JSON DTO, raw (arbitrary content-type), chunked
  streaming (`body_stream`), form-urlencoded, multipart/form-data
  (only one body source is allowed per request)
- Response: raw (`submit_raw`), typed JSON (`submit<T>`/`fetch<T>`),
  streaming (`download(sink)`)
- Timeout (client default + per-request override), default header,
  request header
- Auth: HTTP Basic (`basic_auth`), Bearer (`bearer_token`), proxy Basic
  (`proxy_basic_auth`), mTLS client certificate
  (`client_certificate_file`)
- Connection keep-alive pool: reuses an idle connection of the same
  origin (+proxy). If the server closed the connection (stale),
  automatically retries once with a fresh connection. A `body_stream`
  request always uses a fresh connection since the provider can't
  rewind.
- Coroutine scheduler: a client with no configuration keeps the
  existing blocking submit meaning. Specifying `.coroutines()`
  registers the HTTP work on the internal scheduler, and injecting a
  custom scheduler separates the HTTP execution location from the
  continuation resume location.
- Automatic redirect following: `follow_redirects(max)`. `301/302`'s
  `POST` and `303` change to `GET` and drop the body. `307/308`
  preserve method and body. Supports absolute URL and absolute-path
  Location, and closes with `protocol_error` when the bound is
  exceeded. Doesn't resend the `Authorization` header on a redirect hop
  different from the original request's origin. A differently-named
  secret header can't be distinguished from an ordinary header, so the
  caller manages it directly.
- retry: `retry(attempts)`. Within a configured operation, it only
  retries a transport failure (disconnection, timeout), not an HTTP
  status failure. A `body_stream`/`download` request that can't rewind
  is excluded from auto retry.
- cookie jar: `cookies()`. An in-memory jar reflecting `Set-Cookie`'s
  `Path`, `Secure`, `Max-Age` (0 or below = delete). Other attributes,
  such as `Domain`, `Expires`, are ignored. At most 128 per host (a
  common contract), and the oldest is evicted first when exceeded.
- proxy: `proxy("http://host:port")`. An http target is delivered in
  absolute-form, and an https target opens a `CONNECT` tunnel and then
  performs a TLS handshake. `proxy_basic_auth` carries
  `Proxy-Authorization` on both the absolute-form request and
  `CONNECT`.
- Compression: `compression()`. Sends `Accept-Encoding: gzip, deflate`
  and transparently decompresses a gzip/deflate response body (using
  Boost.Beast zlib, not verifying the trailer checksum). Doesn't apply
  to the `download(sink)` streaming path.
- Response body bound: `max_response_body_size(bytes)`. The default is
  16 MiB, and the same bound applies to both a buffered response and a
  `download(sink)` streaming response.
- HTTP and HTTPS endpoint, TLS server certificate verification,
  hostname verification, test certificate trust option
- HTTP status mapping

HTTP/2 and the common caller-cancellation model are currently outside
implementation scope. HTTP/2 isn't supported by Boost.Beast, and
cancellation needs a separate design since its meaning differs per
server runtime.

`base_url(...)`, `timeout(...)`, `max_response_body_size(...)`,
`default_header(...)`, `trust_certificate_file(...)`,
`follow_redirects(...)`, `retry(...)`, `proxy(...)`, request path,
request header name, and query/form/multipart field name are validated
before sending the call. If the URL scheme isn't `http://` or
`https://`, or timeout or the response body bound is 0 or below, or a
name is empty, it reports a configuration error with
`framework_error_kind_t::protocol_error`. This error isn't converted to
`unavailable`, which indicates a transport failure.

## 4. JSON Contract

JSON conversion is handled through `message_t` or a DTO serializer
hook. Sample application code doesn't pull a field directly with
`nlohmann::json::parse`.

A typed JSON request/response is written with the flow below.

| Operation | C++ Flow |
|------|----------|
| JSON request + typed response | `client.post(path).body(dto).submit<TReply>()` |
| Response JSON decode | Response decode based on `message_t::parse_json<T>()` |

## 5. HTTPS And TLS

`base_url(...)` takes both `http://` and `https://` endpoints.

An `https://` request performs the following.

- TLS handshake
- Server certificate verification
- Hostname verification

A configuration that trusts a test certificate or local development
certificate must be specified as an HTTP client option. TLS
verification isn't implicitly turned off.

## 6. Coroutine Execution Contract

`submit_raw()` and `submit<T>()` return `zlink::framework::task_t`. A
client with no coroutine configuration keeps the existing blocking
submit meaning. It synchronously runs the HTTP work during the call,
and if the caller calls `.result()` on the returned task, the current
thread waits until the result arrives.

A client with `.coroutines()` specified registers the HTTP work on the
HTTP client's internal scheduler. This scheduler doesn't expose a
Boost.Asio, Boost.Beast, or OpenSSL runtime type in the public header.
The HTTP client uses a default scheduler shared within the process, and
doesn't provide a public shutdown API.

When, like a server runtime, the location to re-run the continuation
must be decided directly, a resume scheduler is injected. The HTTP work
is run by the internal scheduler, and the completed coroutine and
callback continue on the caller-provided resume scheduler.

```cpp
auto resume_scheduler =
  std::make_shared<zlink::http_client::framework_resume_scheduler_t>(
    [] (std::function<void ()> continuation) {
      server_queue.post(std::move(continuation));
    });

auto client = zlink::http_client::client_t::create("https://matchmaking.internal")
  .coroutines(resume_scheduler)
  .build();
```

If the caller must decide both the HTTP work execution location and the
resume location, use `.coroutines(execute_scheduler, resume_scheduler)`.

| Client Configuration | `submit<T>()` Execution Meaning |
|-------------|-------------------------|
| No coroutine configuration | Runs the HTTP work synchronously during the call |
| `.coroutines()` | The internal scheduler handles both the HTTP work and resume |
| `.coroutines(resume)` | The internal scheduler runs the HTTP work, and the custom scheduler resumes |
| `.coroutines(execute, resume)` | Custom schedulers handle both the HTTP work and resume |

`coroutine_execute_scheduler_t::execute(...)` runs the HTTP work.
`coroutine_resume_scheduler_t::resume(...)` re-runs a completed
continuation. If the scheduler argument is `nullptr`, it fails with
`invalid_operation`.

Calling `submit_raw()` or `submit<T>()` on a client with coroutine
configuration has the request builder copy request state — method,
path, headers, body provider, timeout — by value, and then register the
work with the scheduler. Even if the temporary builder and client
object disappear, the registered work must complete with its own
request state and runtime shared ownership.

The request timeout starts at the point the work is registered in the
scheduler queue. If the deadline has passed before the worker starts
the HTTP work, it doesn't start the HTTP exchange and completes the
task with a timeout failure.

`submit<T>()` performs typed JSON decode after the raw HTTP work
finishes. On a coroutine client, decode and the caller continuation
follow the resume scheduler policy. `submit<T>(callback)` also runs the
callback at the location the resume scheduler decides, if coroutine
configuration is present. Even if an exception occurs in the callback,
it doesn't change the already-completed task result.

The provider of `body_stream(provider)` and the sink of
`download(sink)` are called on the execute scheduler worker that runs
the HTTP work. Don't directly touch server handler state inside this
callback — use a thread-safe queue or server scheduler post if needed.

## 7. Use In HTTP Hosting Tests

An HTTP handler e2e test calls a `GET`, `POST`, `PUT`, `DELETE` route
with `zlink::http_client`, not an external HTTP tool or a sample-local
client.

This rule is to guarantee two things.

- The HTTP hosting handler is verified with the actual public client.
- A sample and a test don't have different HTTP wrappers.

## 8. Regression Test

The minimal test covers the axes below.

- Contract header compile: `#include <zlink/http_client.hpp>`
- Public header boundary: the runtime implementation header and
  Beast/Asio/OpenSSL types aren't exposed in the public header
- JSON request/response: sends a typed DTO request as JSON and reads
  the reply DTO
- Coroutine submit: `co_await submit<T>()` returns the typed response
  and doesn't blocking-wait for the internal raw submit
- Coroutine scheduler: verifies `.coroutines()` default scheduler,
  custom resume scheduler, custom execute/resume scheduler, framework
  queue adapter, scheduler registration failure, queue timeout
- Streaming callback location: a coroutine client's
  `body_stream(provider)` and `download(sink)` are called on the
  execute scheduler worker
- build-omission shortcut: a request sent with `post(...)`, etc. with
  no `build()`, even from a temporary builder, completes with no
  use-after-free
- Typed body fetch: `fetch<T>()` directly returns the typed DTO and
  throws a failure status as an exception
- Method coverage: `PATCH`, `OPTIONS` are delivered, and `HEAD`
  receives status/header with no body
- Query encoding: `query(...)` is delivered as a percent-encoded query
  string
- Body source: raw content-type, form-urlencoded, multipart encoding
  are carried on the wire as is, and multiple body sources are
  rejected with `protocol_error`
- redirect: follow on/off, absolute URL Location, `POST`→`GET`
  conversion, redirect bound exceeded failure, cross-origin
  `Authorization` removal
- retry: a connection dropped with no response recovers via retry
- cookie: `Set-Cookie` is stored in the jar and carried on a subsequent
  request, and isn't sent once outside the `Path` scope
- proxy: http absolute-form delivery and https `CONNECT` tunnel reach
  the origin
- proxy auth: a request with no auth is rejected with `407`, and passes
  with `proxy_basic_auth`
- Compression: `compression()` decompresses a gzip/deflate response to
  plaintext and removes the `Content-Encoding` header
- Streaming download: `download(sink)` delivers the body in chunks, and
  a redirect intermediate response's body doesn't leak into the sink
- Response body limit: both a buffered response and a `download(sink)`
  response fail once the configured body bound is exceeded
- Streaming upload: `body_stream(provider)` is delivered with chunked
  transfer-encoding
- Auth: `basic_auth`/`bearer_token` are carried on the `Authorization`
  header, and an mTLS server's handshake only succeeds when
  `client_certificate_file` is configured
- Keep-alive: consecutive requests of the same client reuse a single
  connection
- Request timeout override: a per-request `timeout(...)` overrides the
  client default
- HTTP status mapping: `400`, `404`, `500` responses are fixed to a
  client result/error kind
- Timeout: a delayed response closes with a timeout error
- Fluent input validation: an invalid base URL, path, header name, or
  timeout closes with `protocol_error`
- HTTPS success: with a test certificate trust configuration, an
  `https://` JSON request/response succeeds
- TLS failure: an untrusted certificate and a hostname mismatch fail
  with an explicit client error

The current regression test is handled by `test_cpp_http_client` and
`test_cpp_framework_contract_headers`. In a build where OpenSSL is
found, a test certificate is generated at the configure stage, and
`test_cpp_http_client` verifies HTTPS success, untrusted certificate
failure, and hostname mismatch failure together.

The verification labels are below.

```bash
ctest --test-dir framework/languages/cpp/build -L http-client-contract
ctest --test-dir framework/languages/cpp/build -L http-client-unit
ctest --test-dir framework/languages/cpp/build -L http-client-e2e
ctest --test-dir framework/languages/cpp/build -L http-client-https
ctest --test-dir framework/languages/cpp/build -L http-client-regression
```

---
<!-- framework-adapter-nav:bottom:start -->
[Document list](../../../../../../README.en.md) | [Next: Spec -- ZLink Framework C++ HTTP Hosting](../../../server/languages/cpp/60-http-hosting.en.md)
<!-- framework-adapter-nav:bottom:end -->
