# Spec -- ZLink HTTP Client For Java

> See the [user guide](../../../../../java/guide/http-client/README.en.md)
> for a usage-focused document.
> **The language-neutral common contract is owned by the
> [common spec](../../README.en.md)**, and this document only
> describes the Java-specific deviation and implementation mapping for
> the common contract.
> The single standard for the actual contract is the common spec +
> `src/main/java/systems/zlink/httpclient/**`'s public types and the
> `src/test/java/...` regression test.

## 1. Purpose

`zlink-http-client` is a separate client-side deliverable for sending
an HTTP request in Java. It's not a JSON-only client — it's a
general-purpose HTTP client that absorbs `java.net.http`'s low-level
configuration in zlink fluent builder style. The typed JSON path
(`body(dto)`/`submit(Type)`) is a convenience layer laid on top of it.

It depends on `zlink-framework-core`'s error model
(`ZLinkFrameworkException`) but isn't a default dependency of framework
core (one-way dependency).

## 2. Deliverable Boundary

| Role | Location | Public? |
|------|------|-----------|
| Public contract | The client/server client/request builder/execution turn/callback/response types of `systems.zlink.httpclient` | public |
| Runtime implementation | `systems.zlink.httpclient.internal.*` | internal |
| Regression test | `src/test/java/...` | private |
| Gradle subproject | `zlink-http-client` | public |

The public surface doesn't expose `java.net.http`'s `HttpClient`/
`HttpRequest`/`HttpResponse` types.

## 3. Public Types

- `ZLinkHttpClient` — `create()` / `create(baseUrl)`, methods
  `get/post/put/delete/patch/head/options`, `AutoCloseable`.
- `ZLinkHttpServerClient` — the client injected into the framework
  server. Each verb returns a server request builder.
- `ZLinkHttpClientBuilder` — `baseUrl`, `timeout`, `defaultHeader`,
  `basicAuth`, `bearerToken`, `maxResponseBodySize`,
  `trustCertificateFile`, `clientCertificateFile`, `followRedirects`,
  `retry`, `cookies`, `proxy`, `proxyBasicAuth`, `compression`,
  `build`, `buildServer(executionTurn)`, and a one-shot verb shortcut.
- `ZLinkHttpRequestBuilder` — `header`, `query`, `timeout`,
  `body(Object)` (JSON), `body(String, String)` (raw),
  `bodyStream(Supplier<byte[]>, String)`, `form`, `multipart`,
  `multipartFile`, `submitRaw`, `download(Consumer<byte[]>)`,
  `submit(Class<T>)`, `fetch(Class<T>)`, callback. `fetch(Class<T>)`
  directly returns the decoded body as `CompletionStage<T>`, excluding
  status and header.
- `ZLinkHttpServerRequestBuilder` — provides the standalone surface and
  one-way `CompletionStage<Void> submit()`. One-way completion has no
  transport result or admission status, and there's no method that
  pulls the completion value synchronously.
- `ZLinkHttpExecutionTurn` — the injection point where the framework
  wires in the current Spot execution turn's preservation/return.
- `RawHttpResponse` (record) { `status`, `headers`, `body` }.
- `HttpResponse<T>` (record) { `status`, `headers`, `body`, `rawBody` }.
- `ZLinkHttpMethod` (enum).

## 4. Execution Model

- `submitRaw`/`submit`/`fetch`/`download` return a `CompletionStage`.
  With `java.net.http`'s NIO async I/O, the calling thread isn't
  occupied while waiting on the network. The redirect/retry loop is
  also composed as a `CompletionStage` chain.
- A `submit` that waits for a server request's response keeps the
  current Spot turn, and the callback enters the execution queue as a
  new turn. The HTTP request builder has no `yield`. To return the
  shared Spot gate, call `submit` inside `runIoWorker(...)` and wait
  with the Worker call's `yield()`.
- A handler path only uses `CompletionStage` composition — `.get()`/
  `.join()` are prohibited.
- The continuation resume location is specified with a
  `CompletableFuture.*Async(fn, executor)` combination.

The Spring starter provides a default `ZLinkHttpExecutionTurn` bean.
The application registers a server client built with
`buildServer(executionTurn)` under a different bean name per service.
The per-service base URL/auth/timeout/retry policy is owned by this
registration point, not a static factory inside a handler.

## 5. Transport Semantics

Default value/redirect/retry/cookie/compression/auth-scrubbing/body-
source-exclusion semantics follow the
[common spec Chapters 2-8](../../README.en.md). Java implementation
mapping:

- Redirect: since `java.net.http`'s `Redirect` enum has no count
  bound, it's left as `NEVER` and implemented with a wrapper loop.
- Cookie: doesn't use the JDK `CookieManager` — implemented with a
  wrapper jar.
- Decompression: `java.util.zip` (`java.net.http` doesn't
  auto-decompress).
- TLS: `trustCertificateFile`→TrustManager, mTLS→KeyManager
  (`SSLContext`).
- proxy: `ProxySelector` + `Proxy-Authorization` header.

## 6. Error Mapping

Follows [common spec Chapter 9](../../09-error-model.en.md). Every
failure is `ZLinkFrameworkException`, exposing the Framework common
`kind()`.

- Timeout uses `DEADLINE_EXCEEDED` and an `HttpTimeoutException` cause.
- Automatic retry within a configured operation only targets
  `IOException`, `UncheckedIOException`, `TimeoutException`.

## 7. Regression Test / Registration

- Regression test: `src/test/java` (JUnit 5). Chunked upload/retry is
  verified with a raw `ServerSocket`, and TLS/mTLS with
  `com.sun.net.httpserver.HttpsServer` + `src/test/resources/tls/`
  certificates.
- Registration: add `zlink-http-client` to `settings.gradle.kts`'s
  `include`.
- Coverage: connects JaCoCo's (`jacocoTestCoverageVerification`) LINE
  80%-exceeded gate to `check`.
