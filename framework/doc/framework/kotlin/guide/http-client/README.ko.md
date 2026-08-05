# ZLink HTTP Client For Kotlin — 사용자 가이드

> **⚠️ 이 가이드는 최신이 아니다.** 현재 리뷰·정비가 끝난 가이드는
> [`.NET` 가이드](../../../dotnet/README.ko.md) 하나뿐이다. 이 문서는 그 이전 상태이며,
> **`.NET` 가이드가 완성되면 이 문서를 삭제하고 그것을 기준으로 다시 쓴다.**
>
> **계약을 확인할 때는 이 문서를 믿지 말고 [spec 트리](../../../common/spec/README.ko.md)를 본다.**

`zlink-http-client-kotlin`은 Kotlin coroutine으로 HTTP API를 호출하는 client다. zlink 스타일
DSL과 fluent builder로 client·request를 구성하고 모든 제출은 진짜 `suspend` 함수라 응답을
기다리는 동안 **어떤 스레드도 park되지 않는다**.

```kotlin
import systems.zlink.httpclient.kotlin.*

suspend fun loadProfile(client: ZLinkHttpClient): PlayerProfile =
    client.get("/players/7281").await<PlayerProfile>().body()
```

## 목차

| 장 | 문서 | 내용 |
|----|------|------|
| 1 | [개요](01-overview.ko.md) | 설계 철학, 산출물 경계, 실행 모델 |
| 2 | [시작하기](02-getting-started.ko.md) | 의존성, 첫 요청, DSL 빌더 |
| 3 | [Client 구성](03-client-configuration.ko.md) | builder 옵션, client 재사용 |
| 4 | [Request 만들기](04-making-requests.ko.md) | HTTP 메서드, query, 헤더, request timeout |
| 5 | [Request Body](05-request-body.ko.md) | JSON, raw, form, multipart, streaming 업로드 |
| 6 | [Response 다루기](06-handling-responses.ko.md) | 응답 구조, `await`/`fetch`, status 처리 |
| 7 | [코루틴](07-coroutines.ko.md) | `suspend`, non-blocking, 동시성, resume dispatcher |
| 8 | [Streaming](08-streaming.ko.md) | `awaitDownload` 다운로드, chunked 업로드 |
| 9 | [인증과 TLS](09-authentication-tls.ko.md) | Basic/Bearer, HTTPS 검증, mTLS |
| 10 | [Redirect · Retry · Cookie](10-redirects-retries-cookies.ko.md) | redirect 의미론, 재시도, cookie jar |
| 11 | [Proxy](11-proxy.ko.md) | HTTP proxy, proxy 인증 |
| 12 | [압축](12-compression.ko.md) | gzip/deflate 투명 해제 |
| 13 | [에러 처리](13-error-handling.ko.md) | 예외 모델, retriable, 예외 경로 |

정식 계약과 회귀 테스트 축은 spec 문서
[kotlin-http-client.ko.md](../../../common/spec/http-client/languages/kotlin/kotlin-http-client.ko.md)가 정본이다.
