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

### 1.4 종결자 (terminator)

종결자 규칙은 세 층이 나눠 소유한다.

1. **언어별 stem과 `Yield`.** binding 정책
   [async-coroutine-policy §6](../../../../../../bindings/doc/spec/async-coroutine-policy.ko.md#6-언어별-terminal-interface)은
   binding의 언어별 async stem(.NET `Async`, C++ `async`, Java·Node `submit`)과 blocking stem(.NET `Submit`,
   C++ `submit`, Java·Node `submit_sync`)을 정한다. framework
   [Submit과 완료 §2](../server/01-execution/01-submit-and-completion.ko.md#2-terminator별-완료-의미와-언어별-이름)는
   이를 framework 표면에 투영하고 Kotlin wrapper `await`, gate 반납 terminal `Yield`, Node blocking 제외,
   runtime 실행 문맥의 blocking 거부(`InvalidOperation`)를 정한다.
2. **응답 형태 대응.** 이 절의 표가 HTTP 종결자의 **이름과 공통 실행 형태**를 소유한다. HTTP는 위 stem에
   응답 형태(typed·raw·body만·download·callback)를 대응시키며, `Raw` 접미와 `fetch`·`download`는 HTTP가
   정한 이름이다.
3. **언어별 signature.** `languages/<lang>/`의 exact interface는 이 표를 그 언어의 정확한 parameter와
   return type으로 투영한다. 이름을 다시 결정하지 않는다.

HTTP는 응답 없는 요청이 없으므로 one-way 종결자를 두지 않는다. 이 문장이 그 부재의 유일한 규범이다 — 다른
문서는 이 절을 참조한다. 응답 값이 필요 없는 호출은 raw 종결자의 결과를 사용하지 않는다.

| 응답 형태 | 규칙 | cpp | dotnet | java | kotlin | node |
| --- | --- | --- | --- | --- | --- | --- |
| typed response `HttpResponse<T>` | 비동기 stem, generic | `async<T>()` → `task_t<http_response_t<T>>` | `Async<T>(ct?)` → `ValueTask<HttpResponse<T>>` | `submit(Class<T>)` → `CompletionStage<HttpResponse<T>>` | `await(type)` / `await<T>()` (suspend) | `submit<T>()` → `Promise<HttpResponse<T>>` |
| raw response | 비동기 stem + `Raw` | `async_raw()` → `task_t<raw_http_response_t>` | `AsyncRaw(ct?)` → `ValueTask<RawHttpResponse>` | `submitRaw()` → `CompletionStage<RawHttpResponse>` | `awaitRaw()` (suspend) | `submitRaw()` → `Promise<RawHttpResponse>` |
| decoded body `T`만 | `fetch` — 모든 언어에서 비동기 | `fetch<T>()` → `task_t<T>` | `Fetch<T>(ct?)` → `ValueTask<T>` | `fetch(Class<T>)` → `CompletionStage<T>` | `fetch<T>()` (suspend) | `fetch<T>()` → `Promise<T>` |
| streaming download | `download` | `download(sink)` → `task_t<raw_http_response_t>` | `DownloadAsync(sink, ct?)` | `download(Consumer<byte[]>)` | `awaitDownload(sink)` | `download(sink)` |
| callback 완료 | 비동기 stem + callback 인자 | `async<T>(callback)` | `Async<T>(callback)` | `submit(Class<T>, callback)` | (suspend로 대체) | `submit<T>(callback)` |
| gate 반납 (DI server builder 전용) | `Yield` | `yield<T>()` | `Yield<T>(ct?)` → `ValueTask<HttpResponse<T>>` | `yield(Class<T>)` | `yield<T>()` (suspend) | `yield<T>()` → `Promise<HttpResponse<T>>` |
| blocking (CLI·client 시나리오 전용) | blocking stem | `submit_raw()` → `result_t<raw_http_response_t>`, `submit<T>()` → `result_t<http_response_t<T>>` | 두지 않는다 | 두지 않는다 | 두지 않는다 | 두지 않는다 |

- `Yield`가 있는 실행 문맥과 gate 반납의 의미는 [Submit과 완료 §2](../server/01-execution/01-submit-and-completion.ko.md#2-terminator별-완료-의미와-언어별-이름)가,
  DI server builder의 `Yield`와 I/O Worker + Worker `Yield`의 두 사용 형태는 [05 §5.2](05-execution-model.ko.md#52-외부-http-대기와-spot-실행-줄)가 소유한다.
- `fetch<T>()`는 C++에서도 비동기다 — 같은 이름이 언어마다 다른 실행 의미를 갖지 않는다.

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
