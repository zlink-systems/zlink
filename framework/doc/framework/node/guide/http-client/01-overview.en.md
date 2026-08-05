[← Table Of Contents](README.en.md)

# 1. Overview

## What It Is

`@zlink-systems/http-client` is the client-side artifact Node applications use to call HTTP APIs.
Node has a global `fetch`, but it lacks automatic gzip decoding, a cookie jar, redirect count
limits, and fine-grained proxy/TLS control, which clashes with the zlink contract. This client puts
a fluent builder on top of undici's low-level layer, hiding that complexity and aligning it with the
framework's error/codec model.

```ts
const profile = await client.get('/players/7281').submit<PlayerProfile>();
```

It's not a JSON-only client. It's a general-purpose HTTP client, and the typed JSON path
(`body(dto)` / `submit<T>()`) is a convenience layer built on top of it.

## Design Principles

- **Fluent builder.** Both client configuration and request configuration are written as method
  chains.
- **No undici in the public surface.** The `Dispatcher`, `Agent`, and `request` types are not
  revealed in the public API. The undici dependency is confined inside the runtime implementation.
- **Native wrapping.** Transport is delegated to undici, but the parts whose contract and semantics
  differ (cookie jar, redirect loop, retry, compression control) are implemented directly in a thin
  wrapper.

## Backend Choice — undici, Not `fetch`

The parity implementation uses undici's low-level `request`. `fetch` has no numeric redirect limit
matching `follow_redirects(n)`, automatically decodes gzip which breaks streaming/header-removal/
decoded-limit semantics, and has limited per-client proxy/TLS control. undici's `request` does no
auto-redirect, auto-decompress, or cookie handling, letting the wrapper control the semantics.

## Execution Model

- `submitRaw()` / `submit<T>()` / `download(sink)` return a `Promise`. While `await`ing, HTTP I/O is
  handled on the libuv event loop's asynchronous sockets, and **the event loop thread is not
  occupied.**
- Node has no synchronous blocking HTTP access (no blocking path).

## Feature Overview

- Methods: `GET` `POST` `PUT` `DELETE` `PATCH` `HEAD` `OPTIONS`
- Body: typed JSON · raw · form-urlencoded · multipart/form-data · chunked streaming upload
- Response: raw · typed JSON · streaming download
- Connection pool (undici), redirect tracking, transport retry, cookie jar
- Authentication: Basic · Bearer · proxy Basic · mTLS client certificate
- HTTPS/TLS verification, test certificate trust
- HTTP proxy
- Transparent gzip/deflate response decoding

[Next: Getting Started →](02-getting-started.en.md)
