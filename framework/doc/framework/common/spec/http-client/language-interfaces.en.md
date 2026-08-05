# Per-Language Interface Definition

> [Common contract table of contents](README.en.md)
>
> Defines the exact **name/signature** each concept of the common
> contract ([Chapter 2](02-client-builder.en.md)-[9](09-error-model.en.md))
> is exposed with per language. Adding a new symbol to a language's
> public surface must be registered in both this document and the
> common contract first.

## 1. Name Cross-Reference Table (Common Concept → Language)

### 1.1 Entry Point And Client

| Concept | cpp | dotnet | java | kotlin | node |
| --- | --- | --- | --- | --- | --- |
| Client type | `client_t` | `ZLinkHttpClient` | `ZLinkHttpClient` | (reuses java) | `ZLinkHttpClient` |
| Creation | `client_t::create(url)` | `ZLinkHttpClient.Create(url)` | `ZLinkHttpClient.create(url)` | `zlinkHttpClient(url) { }` | `ZLinkHttpClient.create(url)` |
| Builder type | `client_builder_t` | `ZLinkHttpClientBuilder` | `ZLinkHttpClientBuilder` | (DSL receiver = java builder) | `ZLinkHttpClientBuilder` |
| Completion | `.build()` | `.Build()` | `.build()` | (end of block) | `.build()` |
| Closing | Destructor | `Dispose()` | `close()` (AutoCloseable) | `use { }` | `close()` |

### 1.2 Builder Option (Common Concept Name → Language Notation)

Casing rule: cpp `snake_case`, dotnet `PascalCase`, java/kotlin/node
`camelCase`. Below only specifies what deviates from the rule or has a
different argument type. The remaining options (`defaultHeader`,
`basicAuth`, `bearerToken`, `maxResponseBodySize`,
`trustCertificateFile`, `clientCertificateFile`, `followRedirects`,
`retry`, `cookies`, `proxy`, `proxyBasicAuth`, `compression`) only
differ in casing conversion.

| Concept | cpp | dotnet | java/kotlin | node |
| --- | --- | --- | --- | --- |
| `timeout` argument | `std::chrono::milliseconds` | `TimeSpan` | `java.time.Duration` | Integer ms |
| Execution model switch | `coroutines()` 3 overloads | — | — | — |
| Codec registration | — | `Codecs(Action<IZLinkCodecRegistryBuilder>)` | — | — |

### 1.3 Verb And Request Builder

| Concept | cpp | dotnet | java/kotlin | node |
| --- | --- | --- | --- | --- |
| 7 verbs | `get/post/put/delete_/patch/head/options` | `Get/Post/Put/Delete/Patch/Head/Options` | `get/.../delete/...` | `get/.../delete/...` |
| Request builder | `request_builder_t` | `ZLinkHttpRequestBuilder` | `ZLinkHttpRequestBuilder` | `ZLinkHttpRequestBuilder` |
| Typed body | `body(const T&)` | `Body<T>(value)` | `body(Object)` | `body<T>(value)` |
| Raw body | `body(content, content_type)` | `Body(content, contentType)` | `body(content, contentType)` | `body(content, contentType)` |
| Streaming upload | `body_stream(provider, ct)` — `std::function<std::optional<std::string>()>` | `BodyStream(Func<byte[]?>, ct)` | `bodyStream(Supplier<byte[]>, ct)` / kotlin `() -> ByteArray?` | `bodyStream(provider, ct)` — `() => Uint8Array \| null` |
| form / multipart | `form` / `multipart` / `multipart_file` | `Form` / `Multipart` / `MultipartFile` | `form` / `multipart` / `multipartFile` | `form` / `multipart` / `multipartFile` |

### 1.4 Messaging Call Terminator (Target Contract)

**The HTTP request builder is a Messaging call builder.** The async
completion terminator uses `.NET`'s `Async`, Kotlin wrapper's `await`,
Java/C++'s `submit`. Node uses `submitRaw` for raw response, `async`
for typed response and callback, and `submit` for one-way.
A callback completion path is also provided together for a caller
that doesn't use an awaitable
([12 HTTP Client](12-http-client.en.md)). Below is the **target
contract**. The gap with the current implementation and fix evidence is
owned by each language's audit/execution ledger.

| Concept | cpp | dotnet | java | kotlin | node |
| --- | --- | --- | --- | --- | --- |
| **Async completion** (raw) | `submit_raw()` → `task_t<raw_http_response_t>` | `AsyncRaw(ct?)` → `ValueTask<RawHttpResponse>` | `submitRaw()` → `CompletionStage<RawHttpResponse>` | `awaitRaw()` (suspend) | `submitRaw()` → `Promise<RawHttpResponse>` |
| **Async completion** (typed response) | `submit<T>()` → `task_t<http_response_t<T>>` | `Async<T>(ct?)` | `submit(Class<T>)` | `await(type)` / `await<T>()` (reified) | `async<T>()` |
| **Async completion** (typed body) | `fetch<T>()` | `Fetch<T>(ct?)` → `ValueTask<T>` | `fetch(Class<T>)` | `fetch<T>()` (suspend) | `fetch<T>()` → `Promise<T>` |
| **Async completion** (download) | `download(sink)` | `DownloadAsync(sink, ct?)` | `download(Consumer<byte[]>)` | `awaitDownload(sink)` | `download(sink)` |
| **one-way** | `submit()` → `task_t<void>` | `Async(ct?)` → `ValueTask` | `submit()` → `CompletionStage<Void>` | `await()` → `Unit` (suspend) | `submit()` → `Promise<void>` |
| **callback** | `submit<T>(callback)` | `Async<T>(callback)` | `submit(Class<T>, callback)` | (replaced by suspend) | `async<T>(callback)` |
| **gate-returning completion** (server builder only) | `yield<T>()` | `Yield<T>(ct?)` → `ValueTask<HttpResponse<T>>` | `yield(Class<T>)` | `yield<T>()` (suspend) | `yield<T>()` → `Promise<HttpResponse<T>>` |
| Blocking unwrap | **not provided** | **not provided** | **not provided** | **not provided** | **not provided** |

- The HTTP request builder doesn't provide `Yield`/`yield`. An
  application that must return the Spot shared turn puts the HTTP call
  in `RunIoWorker(...)` and uses the Worker call's `Yield`.
- The one-way completion value doesn't include the transport result or
  admission status. The return type only carries async completion and
  failure.
- `.NET`'s async terminator is `Async`, Kotlin wrapper's is `await`,
  C++/Java's is `submit`. Node uses `submitRaw` for raw response,
  `async` for typed response and callback, and `submit` for one-way.
- The `fetch` family directly returns the decoded body to a caller
  that doesn't need status/header. It completes asynchronously in
  every language except C++. C++'s `fetch<T>()` is used only in a
  blocking client scenario.

### 1.5 Response/Auxiliary Type

| Concept | cpp | dotnet | java/kotlin | node |
| --- | --- | --- | --- | --- |
| raw response | `raw_http_response_t{status, headers, body}` | `RawHttpResponse{Status, Headers, Body}` | `RawHttpResponse(status, headers, body)` record | `RawHttpResponse{status, headers, body}` |
| typed response | `http_response_t<T>{status, headers, body, raw_body}` | `HttpResponse<T>{Status, Headers, Body, RawBody}` | `HttpResponse<T>(...)` record — method access | `HttpResponse<T>{...}` |
| method enum | `http_method_t` | `ZLinkHttpMethod` | `ZLinkHttpMethod` | `ZLinkHttpMethod` (union) |
| Result delivery | `result_t<...>` envelope + exception | Exception | Exception | Exception |

### 1.6 Error Surface ([Chapter 9](09-error-model.en.md) Mapping Summary)

| | Exception/Failure Type | Kind Access |
| --- | --- | --- |
| C++ | `framework_exception_t` / `result_t` | Framework common kind |
| .NET | `ZLinkFrameworkException` | `ZLinkFrameworkErrorKind` |
| Java/Kotlin | `ZLinkFrameworkException` | `kind()` |
| Node.js | `ZLinkFrameworkException` | Framework common kind |

## 2. Per-Language Public Surface Summary (Non-Normative)

The list below is a summary for reading the name cross-reference
between languages. The exact full count and signature of each
language's public symbol is owned by the formal interface document
under `languages/<lang>/`. This summary omitting an auxiliary type
isn't a reason to remove an implementation or change the public
contract.

- **cpp** `zlink::http_client`: `client_t`, `client_builder_t`,
  `request_builder_t`, `http_method_t`, `http_response_t<T>`,
  `raw_http_response_t`, `coroutine_execute_scheduler_t`,
  `coroutine_resume_scheduler_t`, `framework_resume_scheduler_t`.
  (`body_stream_provider_t` is a nested typedef inside
  `request_builder_t`, not a top-level symbol)
- **dotnet** `Zlink.HttpClient`: `ZLinkHttpClient`, `ZLinkHttpClientBuilder`,
  `ZLinkHttpRequestBuilder`, `ZLinkHttpMethod`, `RawHttpResponse`,
  `HttpResponse<T>`.
- **java** `systems.zlink.httpclient`: `ZLinkHttpClient`,
  `ZLinkHttpClientBuilder`, `ZLinkHttpRequestBuilder`, `ZLinkHttpMethod`,
  `RawHttpResponse`, `HttpResponse<T>`.
  (`ZLinkHttpTargetBuilder`, `ZLinkHttpRequestBodyEncoder` are
  package-private internal — not public)
- **kotlin** `systems.zlink.httpclient.kotlin`: `zlinkHttpClient`,
  `awaitRaw`, `await` (2 forms), `awaitDownload`, `fetch` extension
  function.
- **node** `@zlink-systems/http-client`: `ZLinkHttpClient`,
  `ZLinkHttpClientBuilder`, `ZLinkHttpRequestBuilder`, `ZLinkHttpMethod`,
  `RawHttpResponse`, `HttpResponse<T>`, `BodyChunkProvider`, `DownloadSink`.
