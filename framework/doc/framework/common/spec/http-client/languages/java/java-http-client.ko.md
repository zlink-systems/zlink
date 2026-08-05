# Spec -- ZLink HTTP Client For Java

> 사용법 중심 문서는 [사용자 가이드](../../../../../java/guide/http-client/README.ko.md)를 본다.
> **언어 중립 공통 계약은 [공통 spec](../../README.ko.md)이 정본**이며,
> 이 문서는 공통 계약에 대한 Java 고유 편차와 구현 매핑만 기술한다.
> 실제 계약의 단일 기준은 공통 spec + `src/main/java/systems/zlink/httpclient/**`
> 공개 타입과 `src/test/java/...` 회귀 테스트다.

## 1. 목적

`zlink-http-client`는 Java에서 HTTP request를 보내기 위한 별도 client-side 산출물이다.
JSON 전용 client가 아니라 일반 HTTP client이며 zlink fluent builder 스타일로
`java.net.http`의 낮은 수준 설정을 흡수한다. typed JSON 경로(`body(dto)`/`submit(Type)`)는
그 위에 얹은 편의 계층이다.

`zlink-framework-core`의 에러 모델(`ZLinkFrameworkException`)에 의존하지만 framework core의
기본 의존성은 아니다(단방향 의존).

## 2. 산출물 경계

| 역할 | 위치 | 공개 여부 |
|------|------|-----------|
| 공개 contract | `systems.zlink.httpclient`의 client·server client·request builder·execution turn·callback·response 타입 | public |
| runtime 구현 | `systems.zlink.httpclient.internal.*` | internal |
| 회귀 테스트 | `src/test/java/...` | private |
| Gradle 서브프로젝트 | `zlink-http-client` | public |

공개 표면에는 `java.net.http`의 `HttpClient`/`HttpRequest`/`HttpResponse` 타입을 노출하지
않는다.

## 3. 공개 타입

- `ZLinkHttpClient` — `create()` / `create(baseUrl)`, 메서드 `get/post/put/delete/
  patch/head/options`, `AutoCloseable`.
- `ZLinkHttpServerClient` — framework 서버에 주입하는 client. 각 verb는 서버 request builder를
  반환한다.
- `ZLinkHttpClientBuilder` — `baseUrl`, `timeout`, `defaultHeader`, `basicAuth`,
  `bearerToken`, `maxResponseBodySize`, `trustCertificateFile`, `clientCertificateFile`,
  `followRedirects`, `retry`, `cookies`, `proxy`, `proxyBasicAuth`, `compression`,
  `build`, `buildServer(executionTurn)`, 그리고 단발 verb shortcut.
- `ZLinkHttpRequestBuilder` — `header`, `query`, `timeout`, `body(Object)`(JSON),
  `body(String, String)`(raw), `bodyStream(Supplier<byte[]>, String)`, `form`, `multipart`,
  `multipartFile`, `submitRaw`, `download(Consumer<byte[]>)`, `submit(Class<T>)`,
  `fetch(Class<T>)`, callback. `fetch(Class<T>)`는 status와 header를 제외하고 decoded body를
  `CompletionStage<T>`로 직접 반환한다.
- `ZLinkHttpServerRequestBuilder` — standalone 표면과 one-way
  `CompletionStage<Void> submit()`을 제공한다. One-way 완료에는 전송 결과나 admission status가
  없으며, 완료 값을 동기로 꺼내는 method도 없다.
- `ZLinkHttpExecutionTurn` — framework가 현재 Spot 실행 turn의 유지·반납을 연결하는 주입점이다.
- `RawHttpResponse`(record) { `status`, `headers`, `body` }.
- `HttpResponse<T>`(record) { `status`, `headers`, `body`, `rawBody` }.
- `ZLinkHttpMethod`(enum).

## 4. 실행 모델

- `submitRaw`/`submit`/`fetch`/`download`는 `CompletionStage`를 돌려준다. `java.net.http`의 NIO
  비동기 I/O로 네트워크 대기 중 호출 스레드는 점유되지 않는다. redirect/retry 루프도
  `CompletionStage` 체인으로 합성된다.
- 서버 request의 응답을 기다리는 `submit`은 현재 Spot turn을 유지하고 callback은 새 turn으로 실행 queue에
  들어간다. HTTP request builder에는 `yield`가 없다. Shared Spot gate를 반납하려면
  `runIoWorker(...)` 안에서 `submit`을 호출하고 Worker call의 `yield()`로 기다린다.
- handler 경로는 `CompletionStage` 합성만 쓰고 `.get()`/`.join()`은 금지한다.
- continuation 재개 위치는 `CompletableFuture.*Async(fn, executor)` 조합으로 지정.

Spring starter는 기본 `ZLinkHttpExecutionTurn` bean을 제공한다. application은 서비스마다
`buildServer(executionTurn)`으로 만든 server client를 서로 다른 bean 이름으로 등록한다. 서비스별
base URL·인증·timeout·retry 정책은 handler 안의 정적 팩토리가 아니라 이 등록 지점이 소유한다.

## 5. 전송 의미론

기본값·redirect·retry·cookie·압축·인증 스크럽·body 소스 배타 의미론은
[공통 spec 2~8장](../../README.ko.md)을 따른다. Java 구현 매핑:

- redirect: `java.net.http`의 `Redirect` enum에 횟수 한도가 없어 `NEVER`로 두고
  래퍼 루프로 구현.
- cookie: JDK `CookieManager` 미사용, 래퍼 jar로 구현.
- 압축 해제: `java.util.zip`(`java.net.http`는 auto-decompress 안 함).
- TLS: `trustCertificateFile`→TrustManager, mTLS→KeyManager(`SSLContext`).
- proxy: `ProxySelector` + `Proxy-Authorization` 헤더.

## 6. 에러 매핑

[공통 spec 9장](../../09-error-model.ko.md)을 따른다. 모든 실패는
`ZLinkFrameworkException`이며 Framework 공통 `kind()`를 노출한다.

- timeout은 `DEADLINE_EXCEEDED`와 `HttpTimeoutException` cause를 사용한다.
- 설정된 operation 안의 자동 retry는 `IOException`, `UncheckedIOException`, `TimeoutException`만 대상으로 한다.

## 7. 회귀 테스트 / 등록

- 회귀 테스트: `src/test/java`(JUnit 5). chunked 업로드·retry는 raw `ServerSocket`, TLS/mTLS는
  `com.sun.net.httpserver.HttpsServer` + `src/test/resources/tls/` 인증서로 검증.
- 등록: `settings.gradle.kts` `include`에 `zlink-http-client` 추가.
- 커버리지: JaCoCo(`jacocoTestCoverageVerification`) LINE 80% 초과 게이트를 `check`에 연결.
