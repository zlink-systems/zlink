# ZLink HTTP Client For Kotlin — User Guide

> **⚠️ This guide is not current.** The only guide that has finished review and upkeep right now is
> the [`.NET` guide](../../../dotnet/README.en.md). This document reflects an earlier state, and
> **once the `.NET` guide is finished, this document will be deleted and rewritten based on it.**
>
> **When confirming the contract, don't trust this document — check the [spec tree](../../../common/spec/README.en.md).**

`zlink-http-client-kotlin` is a client that calls HTTP APIs with Kotlin coroutines. It configures
the client/request with a zlink-style DSL and fluent builder, and every submit is a true `suspend`
function, so **no thread is parked** while waiting for a response.

```kotlin
import systems.zlink.httpclient.kotlin.*

suspend fun loadProfile(client: ZLinkHttpClient): PlayerProfile =
    client.get("/players/7281").await<PlayerProfile>().body()
```

## Table Of Contents

| Chapter | Document | Content |
|----|------|------|
| 1 | [Overview](01-overview.en.md) | Design philosophy, deliverable boundary, execution model |
| 2 | [Getting Started](02-getting-started.en.md) | Dependency, first request, DSL builder |
| 3 | [Client Configuration](03-client-configuration.en.md) | Builder options, client reuse |
| 4 | [Making Requests](04-making-requests.en.md) | HTTP methods, query, headers, request timeout |
| 5 | [Request Body](05-request-body.en.md) | JSON, raw, form, multipart, streaming upload |
| 6 | [Handling Responses](06-handling-responses.en.md) | Response structure, `await`/`fetch`, status handling |
| 7 | [Coroutines](07-coroutines.en.md) | `suspend`, non-blocking, concurrency, resume dispatcher |
| 8 | [Streaming](08-streaming.en.md) | `awaitDownload` download, chunked upload |
| 9 | [Authentication And TLS](09-authentication-tls.en.md) | Basic/Bearer, HTTPS verification, mTLS |
| 10 | [Redirect · Retry · Cookie](10-redirects-retries-cookies.en.md) | Redirect semantics, retries, cookie jar |
| 11 | [Proxy](11-proxy.en.md) | HTTP proxy, proxy authentication |
| 12 | [Compression](12-compression.en.md) | Transparent gzip/deflate decoding |
| 13 | [Error Handling](13-error-handling.en.md) | Exception model, retriable, exception paths |

The formal contract and regression test axes are owned by the spec document
[kotlin-http-client.ko.md](../../../common/spec/http-client/languages/kotlin/kotlin-http-client.en.md).
