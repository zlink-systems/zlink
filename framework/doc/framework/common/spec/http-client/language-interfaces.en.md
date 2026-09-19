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

### 1.4 Terminators

Three layers share the terminator rules.

1. **Per-language stems and `Yield`.** The binding policy
   [async-coroutine-policy §6](../../../../../../bindings/doc/spec/async-coroutine-policy.en.md#6-per-language-terminal-interfaces)
   names the binding's per-language async stem (`.NET` `Async`, C++ `async`, Java/Node `submit`) and
   blocking stem (`.NET` `Submit`, C++ `submit`, Java/Node `submit_sync`). The framework's
   [Submit and completion §2](../server/01-execution/01-submit-and-completion.en.md#2-completion-meaning-per-terminator-and-per-language-names)
   projects them onto the framework surface and decides the Kotlin wrapper `await`, the gate-returning
   terminal `Yield`, the absence of a Node blocking terminal, and the rejection of blocking calls in a
   runtime execution context (`InvalidOperation`).
2. **Response-shape mapping.** The table in this section owns the **names and common execution form**
   of the HTTP terminators. HTTP maps the stems above onto response shapes (typed, raw, body only,
   download, callback); the `Raw` suffix and the `fetch` and `download` names are HTTP's own.
3. **Per-language signatures.** The exact interfaces under `languages/<lang>/` project this table onto
   that language's precise parameter and return types. They do not decide names again.

HTTP has no request without a response, so there is no one-way terminator. This sentence is the only
normative statement of that absence — other documents refer to this section. A call that does not need
the response value leaves the raw terminator's result unused.

| Response shape | Rule | cpp | dotnet | java | kotlin | node |
| --- | --- | --- | --- | --- | --- | --- |
| typed response `HttpResponse<T>` | async stem, generic | `async<T>()` → `task_t<http_response_t<T>>` | `Async<T>(ct?)` → `ValueTask<HttpResponse<T>>` | `submit(Class<T>)` → `CompletionStage<HttpResponse<T>>` | `await(type)` / `await<T>()` (suspend) | `submit<T>()` → `Promise<HttpResponse<T>>` |
| raw response | async stem + `Raw` | `async_raw()` → `task_t<raw_http_response_t>` | `AsyncRaw(ct?)` → `ValueTask<RawHttpResponse>` | `submitRaw()` → `CompletionStage<RawHttpResponse>` | `awaitRaw()` (suspend) | `submitRaw()` → `Promise<RawHttpResponse>` |
| decoded body `T` only | `fetch` — async in every language | `fetch<T>()` → `task_t<T>` | `Fetch<T>(ct?)` → `ValueTask<T>` | `fetch(Class<T>)` → `CompletionStage<T>` | `fetch<T>()` (suspend) | `fetch<T>()` → `Promise<T>` |
| streaming download | `download` | `download(sink)` → `task_t<raw_http_response_t>` | `DownloadAsync(sink, ct?)` | `download(Consumer<byte[]>)` | `awaitDownload(sink)` | `download(sink)` |
| callback completion | async stem + callback argument | `async<T>(callback)` | `Async<T>(callback)` | `submit(Class<T>, callback)` | (replaced by suspend) | `submit<T>(callback)` |
| gate return (DI server builder only) | `Yield` | `yield<T>()` | `Yield<T>(ct?)` → `ValueTask<HttpResponse<T>>` | `yield(Class<T>)` | `yield<T>()` (suspend) | `yield<T>()` → `Promise<HttpResponse<T>>` |
| blocking (CLI and client scenarios only) | blocking stem | `submit_raw()` → `result_t<raw_http_response_t>`, `submit<T>()` → `result_t<http_response_t<T>>` | not provided | not provided | not provided | not provided |

- The execution contexts that have `Yield` and the meaning of returning the gate are owned by
  [Submit and completion §2](../server/01-execution/01-submit-and-completion.en.md#2-completion-meaning-per-terminator-and-per-language-names);
  the two usage forms — the DI server builder's `Yield` and an I/O Worker with the Worker `Yield` — are
  owned by [05 §5.2](05-execution-model.en.md#52-external-http-wait-and-the-spot-execution-queue).
- `fetch<T>()` is asynchronous in C++ as well — the same name never carries a different execution
  meaning per language.

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
