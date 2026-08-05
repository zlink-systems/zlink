# ZLink HTTP client 공통 계약

이 문서 세트는 C++, .NET, Java, Kotlin과 Node.js HTTP client가 공통으로
제공해야 하는 계약을 정의한다. 언어별 문서는 이 공통 동작을 해당 언어의 정확한
public type과 signature로 표현한다.

[10 개정 후보](10-revision-candidates.ko.md)는 아직 공개 계약이 아니다. 승격된
항목만 해당 계약 문서와 모든 언어의 interface에 반영한다.

## 목차

[12 HTTP client](12-http-client.ko.md)는 Framework에서 HTTP client를 등록하고
호출하는 전체 경계를 정의한다. 01~09는 builder, response, 실행, 인증과 오류의
세부 계약을 각각 소유하며, 11은 구현과 contract test가 검증할 항목을 정의한다.

| 장 | 문서 | 내용 |
| --- | --- | --- |
| **12** | [**HTTP client (framework 계약)**](12-http-client.ko.md) | **정본** — 정체성, fluent builder, terminator(`submit`/`async`/`yield` + callback), turn seam, DI 서버 표면 |
| 1 | [범위와 아키텍처](01-scope-and-architecture.ko.md) | 정체성, 산출물 경계, framework와의 관계 |
| 2 | [Client builder 계약](02-client-builder.ko.md) | builder 옵션 전체와 **기본값 표** |
| 3 | [Request 계약](03-request-builder.ko.md) | HTTP 메서드, 헤더/query, body 소스 5종과 배타 규칙 |
| 4 | [Response 계약](04-response-model.ko.md) | raw/typed/download/fetch, status ≥ 400 정책 |
| 5 | [실행 모델](05-execution-model.ko.md) | 비동기 계약, blocking 금지 규칙, client 수명 |
| 6 | [Redirect · Retry · Cookie](06-redirect-retry-cookie.ko.md) | rewrite 규칙 표, 재시도 계약, cookie 부분집합 |
| 7 | [인증 · TLS · Proxy](07-auth-tls-proxy.ko.md) | Basic/Bearer, PEM 신뢰/mTLS, CONNECT tunnel |
| 8 | [압축](08-compression.ko.md) | gzip/deflate 투명 해제 의미론 |
| 9 | [에러 모델](09-error-model.ko.md) | error kind 공통 집합, 언어별 매핑과 구현 갭 |
| 10 | [개정 후보](10-revision-candidates.ko.md) | **비계약** — 승격 전 검토 항목(R1~R14) |
| 11 | [회귀 테스트 계약](11-regression-tests.ko.md) | 공통 계약 케이스 매트릭스, 게이트, 커버리지 갭 |
| — | [언어별 인터페이스 대조표](language-interfaces.ko.md) | **비규범** — 5개 언어 표면을 나란히 보는 대조표. 계약을 고정하지 않는다 |

## 언어별 public API

각 언어의 정확한 타입과 signature는 다음 문서가 소유한다.

| 언어 | 문서 |
|------|------|
| C++ | [languages/cpp](languages/cpp/cpp-http-client.ko.md) |
| `.NET` | [languages/dotnet](languages/dotnet/dotnet-http-client.ko.md) |
| Java | [languages/java](languages/java/java-http-client.ko.md) |
| Kotlin | [languages/kotlin](languages/kotlin/kotlin-http-client.ko.md) |
| Node.js | [languages/node](languages/node/node-http-client.ko.md) |

## 계약 변경 절차

1. 새 동작/공개 API는 먼저 [10장 개정 후보](10-revision-candidates.ko.md)에
   R-항목으로 등재한다. 개정 후보는 계약이 아니며 구현 근거가 되지 않는다.
2. 승격이 결정되면 해당 장의 계약 본문으로 옮기고, 5개 언어 구현·계약 테스트·
   언어별 spec을 함께 갱신한다. 한 언어만 먼저 구현하는 것은 허용하지 않는다
   (저장소 public contract parity 정책).
3. 언어 고유 편차(키워드 회피 `delete_`, kotlin DSL 등)는 이 정본의 각 장에
   "언어 편차" 절로 명시된 것만 인정한다.

## 관련 문서

- 언어별 사용자 가이드: `framework/doc/framework/<lang>/guide/http-client/`
- [Codec extension 공유 계약](../06-framework-api.ko.md)
- [공통 E2E 계약](../../e2e/README.ko.md)
