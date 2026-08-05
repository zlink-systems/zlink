[← 목차](README.ko.md)

# 1. 개요

## 무엇인가

`zlink-http-client-kotlin`은 Kotlin 애플리케이션이 HTTP API를 호출할 때 쓰는 client-side
산출물이다. cookie jar·redirect 횟수 제한·압축 통제 같은 설정을 fluent builder와 DSL 뒤로
숨기고 framework의 에러·코덱 모델과 맞춘다. 모든 제출은 coroutine `suspend` 함수다.

```kotlin
val profile = client.get("/players/7281").fetch<PlayerProfile>()
```

JSON 전용 client가 아니다. 일반 HTTP client이며 typed JSON 경로
(`body(dto)` / `await<T>()` / `fetch<T>()`)는 그 위에 얹은 편의 계층이다.

## 설계 원칙

- **coroutine 우선.** 모든 제출은 `suspend` 함수이며 호출한 coroutine의 dispatcher에서
  재개된다. blocking 메서드는 없다.
- **DSL + fluent builder.** client 구성은 `zlinkHttpClient(url) { ... }` DSL로, request 구성은
  메서드 체인으로 쓴다.
- **공개 표면에 transport 타입 없음.** `ZLinkHttpClient`·`HttpResponse`·`RawHttpResponse`만
  노출하고 내부 transport 타입은 드러나지 않는다.

## 산출물 경계

| 역할 | 위치 | 공개 여부 |
|------|------|-----------|
| 공개 contract | `src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt` | public |
| Gradle 서브프로젝트 | `zlink-http-client-kotlin` | public |
| 회귀 테스트 | `src/test/kotlin/...` | private |

`zlink-http-client-kotlin`은 검증된 `zlink-http-client` 전송 런타임을 전이 의존으로 가져와
재사용하고 그 위에 coroutine 확장과 DSL만 얹는다.

## 실행 모델

- `awaitRaw()` / `await(type)` / `await<T>()` / `fetch<T>()` / `awaitDownload(sink)`는 모두
  `suspend` 함수다. 네이티브 비동기 I/O로 네트워크 대기 중 호출 스레드는 점유되지 않는다.
- 테스트·CLI에서는 `runBlocking { ... }` 안에서 호출한다.

자세한 규칙은 [7. 코루틴](07-coroutines.ko.md)에서 다룬다.

## 기능 한눈에 보기

- 메서드: `GET` `POST` `PUT` `DELETE` `PATCH` `HEAD` `OPTIONS`
- body: typed JSON · raw · form-urlencoded · multipart/form-data · chunked streaming 업로드
- 응답: raw · typed JSON · streaming 다운로드
- connection pool, redirect 추적, transport retry, cookie jar
- 인증: Basic · Bearer · proxy Basic · mTLS client certificate
- HTTPS/TLS 검증, test certificate trust
- HTTP proxy
- gzip/deflate 응답 투명 해제

[다음: 시작하기 →](02-getting-started.ko.md)
