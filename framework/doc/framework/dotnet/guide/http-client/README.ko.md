# ZLink HTTP Client For .NET — 사용자 가이드

`Zlink.HttpClient`는 .NET에서 HTTP request를 보내기 위한 범용 HTTP client다.
zlink 스타일 fluent builder로 사용하며 공개 표면은 `System.Net.Http` 핸들러 타입을
노출하지 않는다.

```csharp
using Zlink.HttpClient;

var game = await ZLinkHttpClient.Create("https://game-api.example.internal")
    .Post("/games")
    .Body(new CreateGameReq("ranked-match-0611"))
    .Fetch<CreateGameRes>();
```

## 목차

| 장 | 문서 | 내용 |
|----|------|------|
| 1 | [개요](01-overview.ko.md) | 설계 철학, 산출물 경계, 실행 모델 |
| 2 | [시작하기](02-getting-started.ko.md) | 프로젝트 참조, 첫 요청, 한 줄 요청 |
| 3 | [Client 구성](03-client-configuration.ko.md) | builder 옵션, client 재사용, 네이티브 핸들러 매핑 |
| 4 | [Request 만들기](04-making-requests.ko.md) | HTTP 메서드, query 파라미터, 헤더, request timeout |
| 5 | [Request Body](05-request-body.ko.md) | JSON DTO, raw, form, multipart, streaming 업로드 |
| 6 | [Response 다루기](06-handling-responses.ko.md) | 응답 구조, `Async`, status 처리 |
| 7 | [비동기](07-async.ko.md) | `Async`, callback, I/O worker와 Spot turn 규칙 |
| 8 | [Streaming](08-streaming.ko.md) | `DownloadAsync(sink)` 다운로드, chunked 업로드 |
| 9 | [인증과 TLS](09-authentication-tls.ko.md) | Basic/Bearer, HTTPS 검증, mTLS |
| 10 | [Redirect · Retry · Cookie](10-redirects-retries-cookies.ko.md) | redirect 의미론, 재시도 정책, cookie jar |
| 11 | [Proxy](11-proxy.ko.md) | HTTP proxy, proxy 인증 |
| 12 | [압축](12-compression.ko.md) | gzip/deflate 투명 해제 |
| 13 | [에러 처리](13-error-handling.ko.md) | error kind 매핑, retriable, 예외 경로 |

## 빠른 길잡이

- 처음이라면 → [2. 시작하기](02-getting-started.ko.md)
- 서버 핸들러 안에서 호출한다면 → [7. 비동기](07-async.ko.md)의 blocking 규칙 먼저
- 실패가 어떻게 보고되는지 → [13. 에러 처리](13-error-handling.ko.md)

정식 계약과 회귀 테스트 축은 spec 문서
[dotnet-http-client.ko.md](../../../common/spec/http-client/languages/dotnet/dotnet-http-client.ko.md)가 정본이다.
이 가이드는 사용법을 다룬다.
