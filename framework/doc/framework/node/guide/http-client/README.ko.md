# ZLink HTTP Client For Node — 사용자 가이드

> **⚠️ 이 가이드는 최신이 아니다.** 현재 리뷰·정비가 끝난 가이드는
> [`.NET` 가이드](../../../dotnet/README.ko.md) 하나뿐이다. 이 문서는 그 이전 상태이며,
> **`.NET` 가이드가 완성되면 이 문서를 삭제하고 그것을 기준으로 다시 쓴다.**
>
> **계약을 확인할 때는 이 문서를 믿지 말고 [spec 트리](../../../common/spec/README.ko.md)를 본다.**

`@zlink-systems/http-client`는 Node에서 HTTP request를 보내기 위한 범용 HTTP client다.
zlink 스타일 fluent builder로 사용하며 공개 표면은 undici 타입을 노출하지 않는다.

```ts
import { ZLinkHttpClient } from '@zlink-systems/http-client';

const game = await ZLinkHttpClient.create('https://game-api.example.internal')
  .post('/games')
  .body({ name: 'ranked-match-0611' })
  .submit<CreateGameRes>();
```

## 목차

| 장 | 문서 | 내용 |
|----|------|------|
| 1 | [개요](01-overview.ko.md) | 설계 철학, 산출물 경계, 실행 모델 |
| 2 | [시작하기](02-getting-started.ko.md) | 패키지 참조, 첫 요청, 한 줄 요청 |
| 3 | [Client 구성](03-client-configuration.ko.md) | builder 옵션, client 재사용, undici 매핑 |
| 4 | [Request 만들기](04-making-requests.ko.md) | HTTP 메서드, query 파라미터, 헤더, request timeout |
| 5 | [Request Body](05-request-body.ko.md) | JSON, raw, form, multipart, streaming 업로드 |
| 6 | [Response 다루기](06-handling-responses.ko.md) | 응답 구조, `submit`, status 처리 |
| 7 | [비동기](07-async.ko.md) | `Promise`, `await`, non-blocking 보장 |
| 8 | [Streaming](08-streaming.ko.md) | `download(sink)` 다운로드, chunked 업로드 |
| 9 | [인증과 TLS](09-authentication-tls.ko.md) | Basic/Bearer, HTTPS 검증, mTLS |
| 10 | [Redirect · Retry · Cookie](10-redirects-retries-cookies.ko.md) | redirect 의미론, 재시도, cookie jar |
| 11 | [Proxy](11-proxy.ko.md) | HTTP proxy, proxy 인증 |
| 12 | [압축](12-compression.ko.md) | gzip/deflate 투명 해제 |
| 13 | [에러 처리](13-error-handling.ko.md) | error kind 매핑, retriable, 예외 경로 |

## 빠른 길잡이

- 처음이라면 → [2. 시작하기](02-getting-started.ko.md)
- 비동기 실행 모델 → [7. 비동기](07-async.ko.md)
- 실패가 어떻게 보고되는지 → [13. 에러 처리](13-error-handling.ko.md)

정식 계약과 회귀 테스트 축은 spec 문서
[node-http-client.ko.md](../../../common/spec/http-client/languages/node/node-http-client.ko.md)가 정본이다.
