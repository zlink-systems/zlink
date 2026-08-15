# ZLink HTTP Client For Java — User Guide

> **⚠️ This guide is not current.** The only guide that has finished review and upkeep right now is
> the [`.NET` guide](../../../dotnet/README.en.md). This document reflects an earlier state, and
> **once the `.NET` guide is finished, this document will be deleted and rewritten based on it.**
>
> **When confirming the contract, don't trust this document — check the [spec tree](../../../common/spec/server/README.en.md).**

`zlink-http-client` is a general-purpose HTTP client for sending HTTP requests from Java. It is used
with a zlink-style fluent builder, and the public surface does not expose `java.net.http` types.

```java
HttpResponse<CreateGameRes> game = ZLinkHttpClient.create("https://game-api.example.internal")
    .post("/games")
    .body(new CreateGameReq("ranked-match-0611"))
    .submit(CreateGameRes.class)
    .toCompletableFuture().join();   // Compose with thenCompose instead of join inside a handler
```

## Table Of Contents

| Chapter | Document | Content |
|----|------|------|
| 1 | [Overview](01-overview.en.md) | Design philosophy, deliverable boundary, execution model |
| 2 | [Getting Started](02-getting-started.en.md) | Dependency, first request, one-line request |
| 3 | [Client Configuration](03-client-configuration.en.md) | Builder options, client reuse, java.net.http mapping |
| 4 | [Making Requests](04-making-requests.en.md) | HTTP methods, query, headers, request timeout |
| 5 | [Request Body](05-request-body.en.md) | JSON, raw, form, multipart, streaming upload |
| 6 | [Handling Responses](06-handling-responses.en.md) | Response structure, `submit`/`fetch`, status handling |
| 7 | [Async](07-async.en.md) | `CompletionStage`, non-blocking, blocking rules |
| 8 | [Streaming](08-streaming.en.md) | `download(sink)` download, chunked upload |
| 9 | [Authentication And TLS](09-authentication-tls.en.md) | Basic/Bearer, HTTPS verification, mTLS |
| 10 | [Redirect · Retry · Cookie](10-redirects-retries-cookies.en.md) | Redirect semantics, retries, cookie jar |
| 11 | [Proxy](11-proxy.en.md) | HTTP proxy, proxy authentication |
| 12 | [Compression](12-compression.en.md) | Transparent gzip/deflate decoding |
| 13 | [Error Handling](13-error-handling.en.md) | Exception model, retriable, exception paths |

The Kotlin coroutine (`suspend`) extension and DSL are covered in the
[Kotlin Coroutine Extension](../../../kotlin/guide/http-client/README.en.md) (a thin idiom layer
sharing the Java runtime).

The formal contract and regression test axes are owned by the spec document
[java-http-client.ko.md](../../../common/spec/http-client/languages/java/java-http-client.en.md).
