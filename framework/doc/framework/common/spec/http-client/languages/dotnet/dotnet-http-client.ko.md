# Spec -- ZLink HTTP Client For .NET

> 사용법 중심 문서는 [사용자 가이드](../../../../../dotnet/guide/http-client/README.ko.md)를 본다.
> **언어 중립 공통 계약은 [공통 spec](../../README.ko.md)이 정본**이며,
> 이 문서는 공통 계약에 대한 .NET 고유 편차와 구현 매핑만 기술한다.
> 공개 계약의 기준은 공통 spec과 이 언어별 spec이다. `src/Zlink.HttpClient/**`의 공개 타입과
> `Zlink.HttpClient.UnitTests` 회귀 테스트는 구현이 그 계약을 지키는지 검증한다.

## 1. 목적

`Zlink.HttpClient`는 .NET에서 HTTP request를 보내기 위한 별도 client-side 산출물이다.
JSON 전용 client가 아니라 일반 HTTP client이며 zlink fluent builder 스타일로
`System.Net.Http`의 낮은 수준 설정을 흡수한다. typed 경로
(`Body(dto)`/`Async<T>()`/`Fetch<T>()`)는 그 위에 얹은 편의 계층이다.

이 client는 package-neutral 공유 artifact인 `Zlink.Framework.Contracts`의 에러 모델(`ZLinkFrameworkException`)과 codec 계약에
의존하지만 `Zlink.Framework` server runtime에는 의존하지 않는다. 따라서 standalone client가
server runtime assembly를 함께 배포하지 않아도 같은 오류·codec 계약을 사용한다.

HTTP codec registry는 serializer 등록만 처리한다. 같은 extension이
`IZlinkStreamCodecRegistration`도 구현하더라도 STREAM descriptor는 무시한다. HTTP client package는
Stream Connector runtime이나 compression package에 의존하지 않는다.

## 2. 산출물 경계

| 역할 | 위치 | 공개 여부 |
|------|------|-----------|
| 공개 contract | `src/Zlink.HttpClient/*.cs`, `Contracts/*` | public |
| 공유 오류·codec contract | `src/Zlink.Framework.Contracts/Codecs`, `Errors` | public dependency |
| runtime 구현 | `src/Zlink.HttpClient/Runtime/*` | internal |
| 회귀 테스트 | `tests/Zlink.HttpClient.UnitTests/*` | private |
| 프로젝트 | `Zlink.HttpClient` | public package |

공개 표면에는 `SocketsHttpHandler`, `HttpClientHandler`, `HttpRequestMessage`,
`HttpResponseMessage` 같은 `System.Net.Http` 타입을 노출하지 않는다.

## 3. 공개 타입

- `ZLinkHttpClient` — 정적 팩토리에서 만드는 standalone client. `Get/Post/Put/Delete/
  Patch/Head/Options`, `IDisposable`을 제공한다.
- `ZLinkHttpServerClient` — framework 서버가 DI로 제공하는 client. 각 verb는
  `ZLinkHttpServerRequestBuilder`를 반환한다.
- `ZLinkHttpClientBuilder` — `BaseUrl`, `Codecs`, `Timeout`, `DefaultHeader`,
  `BasicAuth`, `BearerToken`, `MaxResponseBodySize`, `TrustCertificateFile`,
  `ClientCertificateFile`, `FollowRedirects`, `Retry`, `Cookies`, `Proxy`,
  `ProxyBasicAuth`, `Compression`, `Build`, `BuildServer`, 그리고 단발 verb shortcut.
  (`Codecs`는 framework codec extension 등록 — .NET 고유 확장점,
  [공통 spec 2.3장](../../02-client-builder.ko.md) 언어 편차)
- `ZLinkHttpRequestBuilder` — standalone 표면. `Header`, `Query`, `Timeout`, `Body<T>`,
  `Body(content, contentType)`, `BodyStream`, `Form`, `Multipart`, `MultipartFile`,
  `AsyncRaw`, `DownloadAsync`, `Async<T>`, decoded body를 직접 반환하는
  `Fetch<T>`와 callback overload를 제공한다.
- `ZLinkHttpServerRequestBuilder` — standalone 표면을 포함하고 one-way
  `ValueTask Async(CancellationToken cancellationToken = default)`를 추가한다. 반환된
  `ValueTask`는 비동기 완료와 실패만 전달하며 전송 결과나 admission status를 포함하지 않는다.
  Shared Spot gate를 반납하고 새 turn에서 이어받는
  `ValueTask<HttpResponse<T>> Yield<T>(CancellationToken cancellationToken = default)`도
  추가한다. gate 반납이 허용되는 `SpotWide` User Spot과 Instance Spot에서만 사용한다.
- `IZLinkHttpExecutionScheduler` / `IZLinkHttpExecutionTurn` — DI 통합이 현재 Spot turn을
  캡처하고 callback 완료를 원래 실행 줄의 새 turn에 배치하는 공개 주입점이다.
- `RawHttpResponse` { `Status`, `Headers`, `Body` }.
- `HttpResponse<T>` { `Status`, `Headers`, `Body`, `RawBody` }.
- `ZLinkHttpMethod` enum.

callback overload가 받는 delegate도 공개 계약에 포함한다. 완료 시 `error`와 `response` 중 정확히
하나만 null이 아니다.

```csharp
public delegate void ZLinkHttpCallback<T>(
    Exception? error,
    HttpResponse<T>? response);
```

## 4. 실행 모델

- `Async<T>`는 `ValueTask<HttpResponse<T>>`를 반환하며 Spot turn을 유지한다.
- `Fetch<T>`는 `ValueTask<T>`를 반환한다. status와 header가 필요 없는 application
  sample은 response를 받은 뒤 `.Body`를 꺼내지 않고 이 terminal을 사용한다.

```csharp
public ValueTask<T> Fetch<T>(
    CancellationToken cancellationToken = default);
```
- HTTP request builder에는 `Yield<T>`를 제공하지 않는다. Shared Spot gate를 반납하려면
  `RunIoWorker(...)` 안에서 `Async<T>`를 호출하고 Worker call의 `Yield`로 기다린다.
- callback overload는 awaitable을 반환하지 않는다. 완료 callback은 요청을 만든 Spot turn의
  실행 줄에 새 turn으로 배치한다. standalone client에서는 비동기 완료 문맥에서 직접 호출한다.
- 완료 값을 동기로 꺼내는 blocking terminator는 제공하지 않는다.

## 5. 전송 의미론

기본값·redirect·retry·cookie·압축·인증 스크럽·body 소스 배타 의미론은
[공통 spec 2~8장](../../README.ko.md)을 따른다. .NET 구현 매핑:

- 네이티브 자동 기능 비활성: `SocketsHttpHandler`에서 `AllowAutoRedirect=false`,
  `AutomaticDecompression=None`, `UseCookies=false` — 의미론은 래퍼가 구현.
- per-attempt timeout은 `HttpClient.Timeout` 대신 linked
  `CancellationTokenSource.CancelAfter`로 강제(호출자 취소와 timeout을 구분).
- TLS: `SslClientAuthenticationOptions`(trust 추가 + mTLS). proxy: `WebProxy`.
- 압축 해제: 래퍼가 `System.IO.Compression`으로 수행.

## 6. 에러 매핑

[공통 spec 9장](../../09-error-model.ko.md)을 따른다. .NET은
`ZLinkFrameworkErrorKind`를 사용하며 public exception은 재시도 여부를 제공하지 않는다.

- timeout은 `DeadlineExceeded`와 inner `TimeoutException`으로
  보고한다. 호출자 취소는
  `OperationCanceledException` 그대로 전파된다.

## 7. 회귀 테스트 축

`Zlink.HttpClient.UnitTests`의 `HttpClientContractTests`가 전송 계약 시나리오를 검증한다. chunked 업로드는 managed Linux `HttpListener`가 chunked 요청
본문을 못 받으므로 raw-socket 서버(`RawCaptureServer`)로 검증한다.
