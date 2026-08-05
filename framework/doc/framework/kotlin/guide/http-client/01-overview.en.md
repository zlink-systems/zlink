[← Table Of Contents](README.en.md)

# 1. Overview

## What It Is

`zlink-http-client-kotlin` is the client-side artifact Kotlin applications use to call HTTP APIs.
Settings like the cookie jar, redirect count limit, and compression control are hidden behind a
fluent builder and DSL, aligned with the framework's error/codec model. Every submit is a coroutine
`suspend` function.

```kotlin
val profile = client.get("/players/7281").fetch<PlayerProfile>()
```

It's not a JSON-only client. It's a general-purpose HTTP client, and the typed JSON path
(`body(dto)` / `await<T>()` / `fetch<T>()`) is a convenience layer built on top of it.

## Design Principles

- **Coroutine-first.** Every submit is a `suspend` function, resuming on the calling coroutine's
  dispatcher. There are no blocking methods.
- **DSL + fluent builder.** Client configuration is written with the `zlinkHttpClient(url) { ... }`
  DSL, and request configuration as a method chain.
- **No transport types in the public surface.** Only `ZLinkHttpClient`/`HttpResponse`/
  `RawHttpResponse` are exposed — internal transport types are not revealed.

## Deliverable Boundary

| Role | Location | Exposure |
|------|------|-----------|
| Public contract | `src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt` | public |
| Gradle subproject | `zlink-http-client-kotlin` | public |
| Regression tests | `src/test/kotlin/...` | private |

`zlink-http-client-kotlin` pulls in the verified `zlink-http-client` transport runtime as a
transitive dependency and reuses it, layering only the coroutine extension and DSL on top.

## Execution Model

- `awaitRaw()` / `await(type)` / `await<T>()` / `fetch<T>()` / `awaitDownload(sink)` are all
  `suspend` functions. With native asynchronous I/O, the calling thread is not occupied while
  waiting on the network.
- In tests/CLI, they're called inside `runBlocking { ... }`.

The detailed rules are covered in [7. Coroutines](07-coroutines.en.md).

## Feature Overview

- Methods: `GET` `POST` `PUT` `DELETE` `PATCH` `HEAD` `OPTIONS`
- Body: typed JSON · raw · form-urlencoded · multipart/form-data · chunked streaming upload
- Response: raw · typed JSON · streaming download
- Connection pool, redirect tracking, transport retry, cookie jar
- Authentication: Basic · Bearer · proxy Basic · mTLS client certificate
- HTTPS/TLS verification, test certificate trust
- HTTP proxy
- Transparent gzip/deflate response decoding

[Next: Getting Started →](02-getting-started.en.md)
