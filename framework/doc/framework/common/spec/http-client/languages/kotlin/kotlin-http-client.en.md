# Spec -- ZLink HTTP Client For Kotlin

> See the [user guide](../../../../../kotlin/guide/http-client/README.en.md)
> for a usage-focused document.
> **The language-neutral common contract is owned by the
> [common spec](../../README.en.md)**, and this document only
> describes the deviation of the Kotlin idiom layer on top of the java
> runtime.
> The verification responsibility for transport semantics falls to the
> [java spec](../java/java-http-client.en.md).
> The single standard for the actual contract is the common spec +
> `src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt`'s
> public extension and the `src/test/kotlin/...` regression test.

## 1. Purpose

`zlink-http-client-kotlin` is a deliverable for sending an HTTP request
with Kotlin coroutine. It reuses the verified `zlink-http-client`
transport runtime as a transitive dependency and only adds a DSL and a
true `suspend` extension on top of it. Every submit is a non-blocking
coroutine and resumes on the calling coroutine's dispatcher.

## 2. Deliverable Boundary

| Role | Location | Public? |
|------|------|-----------|
| Public contract | Top-level extension of `systems.zlink.httpclient.kotlin.HttpClientCoroutines.kt` | public |
| Reused runtime | `zlink-http-client` (transitive dependency) | public |
| Regression test | `src/test/kotlin/...` | private |
| Gradle subproject | `zlink-http-client-kotlin` | public |

## 3. Public Surface

The DSL and extension are top-level functions of the
`systems.zlink.httpclient.kotlin` package.

- `zlinkHttpClient(baseUrl: String, configure: ZLinkHttpClientBuilder.() -> Unit = {}): ZLinkHttpClient`
  — applies the DSL block to the fluent builder to build a client.
  Inside the block, builder methods such as `timeout`/
  `basicAuth`/`bearerToken`/`maxResponseBodySize`/`trustCertificateFile`/
  `clientCertificateFile`/`followRedirects`/`retry`/`cookies`/`proxy`/`proxyBasicAuth`/
  `compression` are called as is.
- `suspend ZLinkHttpRequestBuilder.awaitRaw(): RawHttpResponse`
- `suspend ZLinkHttpRequestBuilder.await(type: Class<T>): HttpResponse<T>`
- `suspend inline fun <reified T> ZLinkHttpRequestBuilder.await(): HttpResponse<T>`
- `suspend inline fun <reified T> ZLinkHttpRequestBuilder.fetch(): T` — directly
  returns the decoded body, excluding status and header.
- `suspend ZLinkHttpRequestBuilder.awaitDownload(sink: (ByteArray) -> Unit): RawHttpResponse`
- `suspend ZLinkHttpServerRequestBuilder.await(type)` / `await<T>()` — keeps the
  current Spot turn.
- `suspend ZLinkHttpServerRequestBuilder.await(): Unit` — only delivers async
  completion and failure of a one-way submission. Doesn't return the
  transport result or admission status.
- `suspend inline ZLinkHttpServerRequestBuilder.yield<T>()` — returns the
  current Spot turn while waiting for the HTTP response.

Request configuration (`get/post/put/delete/patch/head/options`,
`header`, `query`, `timeout`, `body`, `bodyStream`, `form`, `multipart`,
`multipartFile`) and response types (`RawHttpResponse`,
`HttpResponse<T>`) use the reused runtime's public types as is.

## 4. Execution Model

- Every extension (`awaitRaw`/`await`/`fetch`/`awaitDownload`) is a
  `suspend` function. It bridges the internal `CompletionStage` to a
  non-blocking coroutine, and the calling coroutine's cancellation
  doesn't cancel an already-submitted HTTP operation. The thread isn't
  occupied while waiting on the network.
- A handler/actor/[spot](../../../01-glossary.en.md#spot) path calls it
  directly inside a suspend function. `runBlocking` is test/CLI-only.
- The continuation resumes on the calling coroutine's dispatcher. The
  resume location is changed with `withContext`.
- The server-only `await` keeps the Java server client's execution
  turn, and `yield<T>()` returns the current turn while waiting for the
  response. The HTTP request builder doesn't provide `yield`. To
  return the shared Spot gate, call `await` inside `runIoWorker(...)`
  and wait with the Worker call's `yield()`.

## 5. Transport Semantics

Transport semantics follow the
[common spec Chapters 2-8](../../README.en.md) and the
[java spec](../java/java-http-client.en.md) as is (transitive reuse —
the Kotlin layer doesn't change transport behavior).

## 6. Error Mapping

Same as [java spec §6](../java/java-http-client.en.md), exposing only
`kind()`. A suspend call is caught with `try`/`catch`.

- Kotlin-specific caution: coroutine cancellation doesn't propagate to
  the underlying request (a review target of the common spec's
  [R5/R9](../../10-revision-candidates.en.md)).

## 7. Regression Test / Registration

- Regression test: `src/test/kotlin` (JUnit 5 + `runBlocking`). Verifies
  `awaitRaw`/typed `await`/`awaitDownload`/streaming upload/
  concurrency (`async`+`awaitAll`).
- Registration: add `zlink-http-client-kotlin` to `settings.gradle.kts`'s
  `include`.
