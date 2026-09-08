[← Table Of Contents](README.en.md)

# 1. Overview

## What It Is

`Zlink.HttpClient` is the client package .NET applications use to call HTTP APIs. Client
configuration and request configuration are built with a fluent builder, so you don't need to
rewrite redirect/cookie/compression/retry policy at every call site.

```csharp
// Configure the client and the request in one flow.
var profile = await client.Get("/players/7281").Fetch<PlayerProfile>();
```

It's not a JSON-only client. It's a general-purpose HTTP client, and the typed path
(`Body(dto)` / `Fetch<T>()` / `Async<T>()`) is a convenience layer built on top of it.

## Usage Principles

- Client configuration and request configuration are both written as method chains.
- Create the client once and reuse it. Use the one-shot builder only for one-off requests.
- Typed body and response use the default JSON codec. If you need a different codec, register a
  `.NET` codec extension when building the client.

## Execution Model

Request execution runs on top of .NET's asynchronous primitives.

- `AsyncRaw()` / `Async<T>()` / `DownloadAsync(sink)` return a `ValueTask<T>`.
- No blocking terminator that synchronously pulls out the completion value is provided.
- A standalone client provides `Async` and a callback. A server client injected through DI also
  provides a one-way `Async()` with no normal completion value. The HTTP request builder has no
  `Yield` that gives back Spot execution authority.

There's one practical takeaway to remember from this model:

> Wait on an ordinary request with `Async<T>()`. If you need the Spot to make progress on other
> work, run the HTTP request inside `RunIoWorker(...)` and wait on the worker call's `Yield()`.

The detailed rules are covered in [7. Async](07-async.en.md).

## Feature Overview

- Methods: `GET` `POST` `PUT` `DELETE` `PATCH` `HEAD` `OPTIONS`
- Body: typed JSON DTO · raw (arbitrary content-type) · form-urlencoded ·
  multipart/form-data · chunked streaming upload
- Response: raw · typed JSON · streaming download
- Connection pool (native), redirect tracking, transport retry, cookie jar
- Authentication: Basic · Bearer · proxy Basic · mTLS client certificate
- HTTPS/TLS verification, test certificate trust
- HTTP proxy
- Transparent gzip/deflate response decoding

Out of scope: a common caller-cancellation model (the standard `CancellationToken` is supported).

[Next: Getting Started →](02-getting-started.en.md)
