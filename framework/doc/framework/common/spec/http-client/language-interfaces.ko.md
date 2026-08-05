# 언어별 인터페이스 정의

> [공통 계약 목차](README.ko.md)
>
> 공통 계약([2](02-client-builder.ko.md)~[9장](09-error-model.ko.md))의 각 개념이
> 언어별로 어떤 **정확한 이름/시그니처**로 노출되는지 정의한다.
> 언어별 공개 표면에 새 심볼을 추가하려면 이 문서와 공통 계약 양쪽에 먼저
> 등재되어야 한다.

## 1. 이름 대응표 (공통 개념 → 언어)

### 1.1 진입점과 client

| 개념 | cpp | dotnet | java | kotlin | node |
| --- | --- | --- | --- | --- | --- |
| client 타입 | `client_t` | `ZLinkHttpClient` | `ZLinkHttpClient` | (java 재사용) | `ZLinkHttpClient` |
| 생성 | `client_t::create(url)` | `ZLinkHttpClient.Create(url)` | `ZLinkHttpClient.create(url)` | `zlinkHttpClient(url) { }` | `ZLinkHttpClient.create(url)` |
| builder 타입 | `client_builder_t` | `ZLinkHttpClientBuilder` | `ZLinkHttpClientBuilder` | (DSL 리시버 = java builder) | `ZLinkHttpClientBuilder` |
| 완성 | `.build()` | `.Build()` | `.build()` | (블록 종료) | `.build()` |
| 종료 | 소멸자 | `Dispose()` | `close()` (AutoCloseable) | `use { }` | `close()` |

### 1.2 builder 옵션 (공통 개념명 → 언어 표기)

케이싱 규칙: cpp `snake_case`, dotnet `PascalCase`, java/kotlin/node `camelCase`.
아래는 규칙에서 벗어나거나 인자형이 다른 것만 명시한다. 나머지 옵션
(`defaultHeader`, `basicAuth`, `bearerToken`, `maxResponseBodySize`,
`trustCertificateFile`, `clientCertificateFile`, `followRedirects`, `retry`,
`cookies`, `proxy`, `proxyBasicAuth`, `compression`)은 케이싱 변환만 다르다.

| 개념 | cpp | dotnet | java/kotlin | node |
| --- | --- | --- | --- | --- |
| `timeout` 인자 | `std::chrono::milliseconds` | `TimeSpan` | `java.time.Duration` | 정수 ms |
| 실행 모델 스위치 | `coroutines()` 3오버로드 | — | — | — |
| codec 등록 | — | `Codecs(Action<IZLinkCodecRegistryBuilder>)` | — | — |

### 1.3 verb와 request builder

| 개념 | cpp | dotnet | java/kotlin | node |
| --- | --- | --- | --- | --- |
| verb 7종 | `get/post/put/delete_/patch/head/options` | `Get/Post/Put/Delete/Patch/Head/Options` | `get/.../delete/...` | `get/.../delete/...` |
| request builder | `request_builder_t` | `ZLinkHttpRequestBuilder` | `ZLinkHttpRequestBuilder` | `ZLinkHttpRequestBuilder` |
| typed body | `body(const T&)` | `Body<T>(value)` | `body(Object)` | `body<T>(value)` |
| raw body | `body(content, content_type)` | `Body(content, contentType)` | `body(content, contentType)` | `body(content, contentType)` |
| streaming 업로드 | `body_stream(provider, ct)` — `std::function<std::optional<std::string>()>` | `BodyStream(Func<byte[]?>, ct)` | `bodyStream(Supplier<byte[]>, ct)` / kotlin `() -> ByteArray?` | `bodyStream(provider, ct)` — `() => Uint8Array \| null` |
| form / multipart | `form` / `multipart` / `multipart_file` | `Form` / `Multipart` / `MultipartFile` | `form` / `multipart` / `multipartFile` | `form` / `multipart` / `multipartFile` |

### 1.4 Messaging call terminator (목표 계약)

**HTTP request builder는 Messaging call builder다.** 비동기 완료 종결자는 .NET `Async`, Kotlin
wrapper `await`, Java·C++ `submit`을 사용한다. Node는 raw response에 `submitRaw`, typed response와
callback에 `async`, one-way에 `submit`을 사용한다.
Awaitable을 쓰지 않는 호출자를 위한 callback
완료 경로도 함께 제공한다([12 HTTP client](12-http-client.ko.md)).
아래는 **목표 계약**이다. 현재 구현과의 차이와 수정 증거는 언어별 audit·실행 ledger가 소유한다.

| 개념 | cpp | dotnet | java | kotlin | node |
| --- | --- | --- | --- | --- | --- |
| **비동기 완료** (raw) | `submit_raw()` → `task_t<raw_http_response_t>` | `AsyncRaw(ct?)` → `ValueTask<RawHttpResponse>` | `submitRaw()` → `CompletionStage<RawHttpResponse>` | `awaitRaw()` (suspend) | `submitRaw()` → `Promise<RawHttpResponse>` |
| **비동기 완료** (typed response) | `submit<T>()` → `task_t<http_response_t<T>>` | `Async<T>(ct?)` | `submit(Class<T>)` | `await(type)` / `await<T>()` (reified) | `async<T>()` |
| **비동기 완료** (typed body) | `fetch<T>()` | `Fetch<T>(ct?)` → `ValueTask<T>` | `fetch(Class<T>)` | `fetch<T>()` (suspend) | `fetch<T>()` → `Promise<T>` |
| **비동기 완료** (download) | `download(sink)` | `DownloadAsync(sink, ct?)` | `download(Consumer<byte[]>)` | `awaitDownload(sink)` | `download(sink)` |
| **one-way** | `submit()` → `task_t<void>` | `Async(ct?)` → `ValueTask` | `submit()` → `CompletionStage<Void>` | `await()` → `Unit` (suspend) | `submit()` → `Promise<void>` |
| **callback** | `submit<T>(callback)` | `Async<T>(callback)` | `submit(Class<T>, callback)` | (suspend로 대체) | `async<T>(callback)` |
| **gate 반납 완료** (서버 builder 전용) | `yield<T>()` | `Yield<T>(ct?)` → `ValueTask<HttpResponse<T>>` | `yield(Class<T>)` | `yield<T>()` (suspend) | `yield<T>()` → `Promise<HttpResponse<T>>` |
| blocking 언래핑 | **두지 않는다** | **두지 않는다** | **두지 않는다** | **두지 않는다** | **두지 않는다** |

- HTTP request builder에는 `Yield`·`yield`를 제공하지 않는다. Spot shared turn을 반납해야 하는
  application은 HTTP call을 `RunIoWorker(...)`에 넣고 Worker call의 `Yield`를 사용한다.
- one-way 완료 값은 전송 결과나 admission status를 포함하지 않는다. 반환형은 비동기 완료와 실패만 전달한다.
- `.NET`의 비동기 종결자는 `Async`, Kotlin wrapper는 `await`, C++·Java는 `submit`을 사용한다.
  Node는 raw response에 `submitRaw`, typed response와 callback에 `async`, one-way에 `submit`을 사용한다.
- `fetch` 계열은 status·header가 필요 없는 호출자에게 decoded body를 직접 반환한다.
  C++를 제외한 언어에서는 비동기로 완료된다. C++ `fetch<T>()`는 blocking client
  시나리오에서만 사용한다.

### 1.5 응답/보조 타입

| 개념 | cpp | dotnet | java/kotlin | node |
| --- | --- | --- | --- | --- |
| raw 응답 | `raw_http_response_t{status, headers, body}` | `RawHttpResponse{Status, Headers, Body}` | `RawHttpResponse(status, headers, body)` record | `RawHttpResponse{status, headers, body}` |
| typed 응답 | `http_response_t<T>{status, headers, body, raw_body}` | `HttpResponse<T>{Status, Headers, Body, RawBody}` | `HttpResponse<T>(...)` record — 메서드 접근 | `HttpResponse<T>{...}` |
| 메서드 enum | `http_method_t` | `ZLinkHttpMethod` | `ZLinkHttpMethod` | `ZLinkHttpMethod` (union) |
| 결과 전달 | `result_t<...>` 봉투 + 예외 | 예외 | 예외 | 예외 |

### 1.6 에러 표면 ([9장](09-error-model.ko.md) 매핑 요약)

| | 예외/실패 타입 | kind 접근 |
| --- | --- | --- |
| C++ | `framework_exception_t` / `result_t` | Framework 공통 kind |
| .NET | `ZLinkFrameworkException` | `ZLinkFrameworkErrorKind` |
| Java/Kotlin | `ZLinkFrameworkException` | `kind()` |
| Node.js | `ZLinkFrameworkException` | Framework 공통 kind |

## 2. 언어별 공개 표면 요약 (비규범)

아래 목록은 언어 간 이름 대응을 읽기 위한 요약이다. 언어별 public 심볼의 정확한
전량과 시그니처는 `languages/<lang>/`의 정식 interface 문서가 소유한다. 이 요약에
보조 타입이 빠져 있다는 이유만으로 구현을 제거하거나 public 계약을 바꾸지 않는다.

- **cpp** `zlink::http_client`: `client_t`, `client_builder_t`,
  `request_builder_t`, `http_method_t`, `http_response_t<T>`,
  `raw_http_response_t`, `coroutine_execute_scheduler_t`,
  `coroutine_resume_scheduler_t`, `framework_resume_scheduler_t`.
  (`body_stream_provider_t`는 `request_builder_t` 안의 중첩 typedef이며 최상위 심볼이 아니다)
- **dotnet** `Zlink.HttpClient`: `ZLinkHttpClient`, `ZLinkHttpClientBuilder`,
  `ZLinkHttpRequestBuilder`, `ZLinkHttpMethod`, `RawHttpResponse`,
  `HttpResponse<T>`.
- **java** `systems.zlink.httpclient`: `ZLinkHttpClient`,
  `ZLinkHttpClientBuilder`, `ZLinkHttpRequestBuilder`, `ZLinkHttpMethod`,
  `RawHttpResponse`, `HttpResponse<T>`.
  (`ZLinkHttpTargetBuilder`, `ZLinkHttpRequestBodyEncoder`는 package-private
  내부 — 공개 아님)
- **kotlin** `systems.zlink.httpclient.kotlin`: `zlinkHttpClient`,
  `awaitRaw`, `await`(2형), `awaitDownload`, `fetch` 확장 함수.
- **node** `@zlink-systems/http-client`: `ZLinkHttpClient`,
  `ZLinkHttpClientBuilder`, `ZLinkHttpRequestBuilder`, `ZLinkHttpMethod`,
  `RawHttpResponse`, `HttpResponse<T>`, `BodyChunkProvider`, `DownloadSink`.
