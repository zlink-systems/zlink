# Spec -- ZLink HTTP Client For Kotlin

> 사용법 중심 문서는 [사용자 가이드](../../../../../kotlin/guide/http-client/README.ko.md)를 본다.
> **언어 중립 공통 계약은 [공통 spec](../../README.ko.md)이 정본**이며,
> 이 문서는 java 런타임 위 Kotlin idiom 레이어의 편차만 기술한다.
> 전송 의미론의 검증 책임은 [java spec](../java/java-http-client.ko.md)이 진다.
> 실제 계약의 단일 기준은 공통 spec +
> `src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt` 공개 확장과
> `src/test/kotlin/...` 회귀 테스트다.

## 1. 목적

`zlink-http-client-kotlin`은 Kotlin coroutine으로 HTTP request를 보내기 위한 산출물이다.
검증된 `zlink-http-client` 전송 런타임을 전이 의존으로 재사용하고 그 위에 DSL과 진짜
`suspend` 확장만 얹는다. 모든 제출은 non-blocking coroutine이며 호출한 coroutine의
dispatcher에서 재개된다.

## 2. 산출물 경계

| 역할 | 위치 | 공개 여부 |
|------|------|-----------|
| 공개 contract | `systems.zlink.httpclient.kotlin.HttpClientCoroutines.kt`의 top-level 확장 | public |
| 재사용 런타임 | `zlink-http-client`(전이 의존) | public |
| 회귀 테스트 | `src/test/kotlin/...` | private |
| Gradle 서브프로젝트 | `zlink-http-client-kotlin` | public |

## 3. 공개 표면

DSL과 확장은 `systems.zlink.httpclient.kotlin` 패키지의 top-level 함수다.

- `zlinkHttpClient(baseUrl: String, configure: ZLinkHttpClientBuilder.() -> Unit = {}): ZLinkHttpClient`
  — DSL 블록을 fluent builder에 적용해 client를 만든다. 블록 안에서 `timeout`/
  `basicAuth`/`bearerToken`/`maxResponseBodySize`/`trustCertificateFile`/
  `clientCertificateFile`/`followRedirects`/`retry`/`cookies`/`proxy`/`proxyBasicAuth`/
  `compression` 등 builder 메서드를 그대로 호출한다.
- `suspend ZLinkHttpRequestBuilder.awaitRaw(): RawHttpResponse`
- `suspend ZLinkHttpRequestBuilder.await(type: Class<T>): HttpResponse<T>`
- `suspend inline fun <reified T> ZLinkHttpRequestBuilder.await(): HttpResponse<T>`
- `suspend inline fun <reified T> ZLinkHttpRequestBuilder.fetch(): T` — status와 header를 제외하고
  decoded body를 직접 반환한다.
- `suspend ZLinkHttpRequestBuilder.awaitDownload(sink: (ByteArray) -> Unit): RawHttpResponse`
- `suspend ZLinkHttpServerRequestBuilder.await(type)` / `await<T>()` — 현재 Spot turn을 유지한다.
- `suspend ZLinkHttpServerRequestBuilder.await(): Unit` — one-way 전송의 비동기 완료와 실패만
  전달한다. 전송 결과나 admission status는 반환하지 않는다.
- `suspend inline ZLinkHttpServerRequestBuilder.yield<T>()` — HTTP response를 기다리는 동안
  현재 Spot turn을 반납한다.

request 구성(`get/post/put/delete/patch/head/options`, `header`, `query`, `timeout`,
`body`, `bodyStream`, `form`, `multipart`, `multipartFile`)과 응답 타입
(`RawHttpResponse`, `HttpResponse<T>`)은 재사용 런타임의 공개 타입을 그대로 쓴다.

## 4. 실행 모델

- 모든 확장(`awaitRaw`/`await`/`fetch`/`awaitDownload`)은 `suspend` 함수다. 내부
  `CompletionStage`를 non-blocking coroutine bridge로 연결하며, 호출 coroutine의
  cancellation은 이미 제출한 HTTP operation을 취소하지 않는다. 네트워크 대기 중
  스레드는 점유되지 않는다.
- handler·actor·[spot](../../../01-glossary.ko.md#spot) 경로는 suspend 함수 안에서 직접 호출한다. `runBlocking`은 테스트·CLI
  전용이다.
- continuation은 호출한 coroutine의 dispatcher에서 재개된다. 재개 위치는 `withContext`로
  바꾼다.
- 서버 전용 `await`는 Java server client의 execution turn을 유지하고, `yield<T>()`는
  response 대기 중 현재 turn을 반납한다. HTTP request builder에는 `yield`를 제공하지
  않는다. Shared Spot gate를 반납하려면 `runIoWorker(...)` 안에서 `await`를 호출하고
  Worker call의 `yield()`로 기다린다.

## 5. 전송 의미론

전송 의미론은 [공통 spec 2~8장](../../README.ko.md)과
[java spec](../java/java-http-client.ko.md)을 그대로 따른다
(전이 재사용 — Kotlin 레이어는 전송 동작을 바꾸지 않는다).

## 6. 에러 매핑

[java spec 6절](../java/java-http-client.ko.md)과 동일하며 `kind()`만 노출한다. Suspend 호출은
`try`/`catch`로 잡는다.

- Kotlin 고유 주의: coroutine 취소가 하부 요청에 전파되지 않는다
  (공통 spec [R5/R9](../../10-revision-candidates.ko.md) 검토 대상).

## 7. 회귀 테스트 / 등록

- 회귀 테스트: `src/test/kotlin`(JUnit 5 + `runBlocking`). `awaitRaw`/typed `await`/
  `awaitDownload`/streaming 업로드/동시성(`async`+`awaitAll`)을 검증한다.
- 등록: `settings.gradle.kts` `include`에 `zlink-http-client-kotlin` 추가.
