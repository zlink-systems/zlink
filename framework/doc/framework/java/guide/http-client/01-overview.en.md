[← Table Of Contents](README.en.md)

# 1. Overview

## What It Is

`zlink-http-client` is the client-side artifact Java applications use to call HTTP APIs. The JDK has
`java.net.http.HttpClient`, but settings like the cookie jar, redirect count limit, and compression
control end up scattered across call sites. This client hides that complexity behind a fluent builder
and aligns it with the framework's error/codec model.

```java
PlayerProfile profile = client.get("/players/7281").fetch(PlayerProfile.class);
```

It's not a JSON-only client. It's a general-purpose HTTP client, and the typed JSON path
(`body(dto)` / `submit(Type)` / `fetch(Type)`) is a convenience layer built on top of it.

## Design Principles

- **Fluent builder.** Both client configuration and request configuration are written as method
  chains.
- **No java.net.http in the public surface.** The `HttpClient`, `HttpRequest`, `HttpResponse` types
  are not revealed in the public API. The dependency is confined inside the runtime implementation
  (internal).
- **Native wrapping.** Transport is delegated to `java.net.http`, but the parts whose contract and
  semantics differ (cookie jar, redirect loop, retry, compression control) are implemented directly
  in a thin wrapper.

## Deliverable Boundary

| Role | Location | Exposure |
|------|------|-----------|
| Public contract | `src/main/java/systems/zlink/httpclient/{ZLink*,RawHttpResponse,HttpResponse,ZLinkHttpMethod}.java` | public |
| Runtime implementation | `src/main/java/systems/zlink/httpclient/internal/*` | internal |
| Regression tests | `src/test/java/...` | private |
| Gradle subproject | `zlink-http-client` | public |

## Execution Model

- `submitRaw()` / `submit(Type)` / `download(sink)` return a `CompletionStage`. With
  `java.net.http`'s NIO-selector-based asynchronous I/O, the calling thread is not occupied while
  waiting on the network.
- `fetch(Type)` is a blocking access, for tests/CLI only.

The detailed rules are covered in [7. Async](07-async.en.md).

## Feature Overview

- Methods: `GET` `POST` `PUT` `DELETE` `PATCH` `HEAD` `OPTIONS`
- Body: typed JSON · raw · form-urlencoded · multipart/form-data · chunked streaming upload
- Response: raw · typed JSON · streaming download
- Connection pool (native), redirect tracking, transport retry, cookie jar
- Authentication: Basic · Bearer · proxy Basic · mTLS client certificate
- HTTPS/TLS verification, test certificate trust
- HTTP proxy
- Transparent gzip/deflate response decoding

[Next: Getting Started →](02-getting-started.en.md)
