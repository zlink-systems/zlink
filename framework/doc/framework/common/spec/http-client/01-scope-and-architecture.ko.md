# 1. 범위와 아키텍처

> [공통 계약 목차](README.ko.md)

## 1.1 정체성

zlink HTTP client는 framework handler와 관리 도구가 같은 framework 오류·codec·실행
계약으로 HTTP를 호출하도록 제공하는 companion client다. 각 언어의 대표 HTTP
전송 스택을 fluent builder 뒤로 감추지만, 일반 HTTP library를 대체하는 범용
client가 아니다. JSON 전용 client도 아니며, typed JSON 경로는 raw HTTP 경로 위에
얹은 framework codec 편의 계층이다.

바닥부터 만들지 않는다. 전송은 언어별 대표 스택에 위임하되, **의미론
(redirect, retry, cookie, 압축, 인증 스크럽)은 래퍼가 직접 소유**하여 5개
언어에서 동일하게 동작한다. 이를 위해 네이티브 스택의 자동 redirect ·
자동 압축 해제 · 자동 cookie는 전부 끄고 래퍼가 구현한다.

| 언어 | 전송 스택 | 산출물 |
| --- | --- | --- |
| cpp | Boost.Beast + Asio (+OpenSSL 선택) | `zlink::http_client` (CMake, static) |
| dotnet | `System.Net.Http` + `SocketsHttpHandler` | `Zlink.HttpClient` (NuGet) |
| java | `java.net.http.HttpClient` | `zlink-http-client` (Gradle) |
| kotlin | java 런타임 전이 재사용 + coroutine 확장 | `zlink-http-client-kotlin` (Gradle) |
| node | undici 저수준 `request` | `@zlink-systems/http-client` (npm) |

## 1.2 공개 표면 규칙

- 공개 계약(contracts)에는 **전송 스택 타입을 노출하지 않는다**
  (Beast/Asio, `SocketsHttpHandler`, `java.net.http.*`, undici 타입 금지).
- 공개 타입 이름은 `ZLinkHttpClient` / `ZLinkHttpClientBuilder` /
  `ZLinkHttpRequestBuilder` / `RawHttpResponse` / `HttpResponse<T>` /
  `ZLinkHttpMethod` 계열로 통일한다(언어 케이싱 관용 적용,
  [언어별 인터페이스 정의](language-interfaces.ko.md) 참조).
- runtime 구현은 언어별 internal 영역(`src/runtime`, `internal/`,
  `Runtime/`)에 갇히며 공개 API에서 도달할 수 없다.

## 1.3 framework와의 관계 — 단방향 의존

- HTTP client는 framework 공통 계약 package의 **에러 모델**(`ZLinkFrameworkException` 계열)과
  **codec extension**(typed body 직렬화)을 소비한다. `.NET`에서는 이 runtime 비의존 계약을
  `Zlink.Framework.Contracts`가 제공한다.
- framework core는 HTTP client에 의존하지 않는다. HTTP client는 framework
  runtime 없이 별도 배포 가능한 독립 산출물이되, 에러/코덱 계약은 framework 것을
  재사용한다(자체 예외 계층을 만들지 않는다).
- typed body encode/decode는 framework · stream-connector와 **같은 codec
  extension을 공유**한다. raw body는 extension을 경유하지 않는다.

## 1.4 kotlin의 위치

kotlin 산출물은 독립 구현이 아니라 java 런타임 위의 **얇은 idiom 레이어**다:
suspend 브리지, DSL 빌더, reified 제네릭, Kotlin data class 역직렬화만 더한다.
전송 의미론 계약의 검증 책임은 java 계약 테스트가 진다.
