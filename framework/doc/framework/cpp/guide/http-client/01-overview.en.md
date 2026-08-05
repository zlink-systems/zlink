[← Table Of Contents](README.en.md)

# 1. Overview

## What It Is

`zlink::http_client` is the client-side artifact C++ applications use to call HTTP APIs. The C++
standard library has no HTTP client, and using Boost.Beast directly lets low-level types like
socket/resolver/parser flow into application code. This client hides that complexity behind a
fluent builder.

```cpp
// Using Boost.Beast directly: resolver, stream, request<string_body>, flat_buffer ...
// zlink::http_client: the one line below
auto profile = client.get ("/players/7281").fetch<player_profile_t> ();
```

It's not a JSON-only client. It's a general-purpose HTTP client, and the typed JSON path
(`body(dto)` / `submit<T>()` / `fetch<T>()`) is a convenience layer built on top of it.

## Design Principles

- **Fluent builder.** Both client configuration and request configuration are written as method
  chains.
- **No Beast in the public header.** The `Boost.Beast`, `Boost.Asio`, OpenSSL, socket, resolver, and
  parser types are not revealed in public headers. The dependency is confined inside the (private)
  runtime implementation.
- **The request owns the client.** Since the request builder holds the client by value, even a
  one-off request built with a temporary client is safe, with no use-after-free.

## Deliverable Boundary

| Role | Location | Exposure |
|------|------|-----------|
| Facade header | `http-client/include/zlink/http_client.hpp` | public |
| Contract headers | `http-client/include/zlink/http_client/contracts/*` | public |
| Runtime implementation | `http-client/src/runtime/*` | private |
| Regression tests | `http-client/tests/*` | private |
| CMake target | `zlink::http_client` | public target |

## Execution Model

Request execution splits into two, depending on the client's configuration.

- By default, the client synchronously executes the HTTP exchange during the `submit_raw()`/
  `submit<T>()` call, as before. This behavior is the default so existing blocking code and tests
  don't break.
- A client with `.coroutines()` specified registers the HTTP work with an internal scheduler on
  `submit_raw()`/`submit<T>()` and returns a `task_t`. `co_await` suspends without occupying the
  calling thread until the response is ready.

There's one practical takeaway to remember from this model:

> Inside a framework runtime/handler thread, `co_await` `submit<T>()`, and use blocking accesses
> like `.result()`/`fetch<T>()` only where blocking is allowed, such as tests or client scenarios. To
> free up the thread while waiting on HTTP inside a handler, configure the client with
> `.coroutines()` or the server-provided resume scheduler.

The detailed rules are covered in [7. Async And Coroutines](07-async-coroutines.en.md).

## Feature Overview

- Methods: `GET` `POST` `PUT` `DELETE` `PATCH` `HEAD` `OPTIONS`
- Body: typed JSON DTO · raw (arbitrary content-type) · form-urlencoded ·
  multipart/form-data · chunked streaming upload
- Response: raw · typed JSON · streaming download
- Connection keep-alive pool, redirect tracking, transport retry, cookie jar
- Authentication: Basic · Bearer · proxy Basic · mTLS client certificate
- HTTPS/TLS verification, test certificate trust
- HTTP proxy (absolute-form + `CONNECT` tunnel)
- Transparent gzip/deflate response decoding

Out of scope: HTTP/2 (not supported by Boost.Beast), a common caller-cancellation model.

[Next: Getting Started →](02-getting-started.en.md)
