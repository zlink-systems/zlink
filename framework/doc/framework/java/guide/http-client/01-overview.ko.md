[← 목차](README.ko.md)

# 1. 개요

## 무엇인가

`zlink-http-client`는 Java 애플리케이션이 HTTP API를 호출할 때 쓰는 client-side 산출물이다.
JDK에는 `java.net.http.HttpClient`가 있지만 cookie jar·redirect 횟수 제한·압축 통제 같은
설정이 호출부에 흩어진다. 이 client는 그 복잡성을 fluent builder 뒤로 숨기고 framework의
에러·코덱 모델과 맞춘다.

```java
PlayerProfile profile = client.get("/players/7281").fetch(PlayerProfile.class);
```

JSON 전용 client가 아니다. 일반 HTTP client이며 typed JSON 경로
(`body(dto)` / `submit(Type)` / `fetch(Type)`)는 그 위에 얹은 편의 계층이다.

## 설계 원칙

- **fluent builder.** client 구성과 request 구성 모두 메서드 체인으로 쓴다.
- **공개 표면에 java.net.http 없음.** `HttpClient`, `HttpRequest`, `HttpResponse` 타입은
  공개 API에 드러나지 않는다. 의존은 runtime 구현(internal) 안에 갇힌다.
- **네이티브 래핑.** 전송은 `java.net.http`에 위임하되, 계약과 의미론이 다른 부분(cookie
  jar, redirect 루프, retry, 압축 통제)은 얇은 래퍼에서 직접 구현한다.

## 산출물 경계

| 역할 | 위치 | 공개 여부 |
|------|------|-----------|
| 공개 contract | `src/main/java/systems/zlink/httpclient/{ZLink*,RawHttpResponse,HttpResponse,ZLinkHttpMethod}.java` | public |
| runtime 구현 | `src/main/java/systems/zlink/httpclient/internal/*` | internal |
| 회귀 테스트 | `src/test/java/...` | private |
| Gradle 서브프로젝트 | `zlink-http-client` | public |

## 실행 모델

- `submitRaw()` / `submit(Type)` / `download(sink)`는 `CompletionStage`를 돌려준다.
  `java.net.http`의 NIO selector 기반 비동기 I/O로 네트워크 대기 중 호출 스레드는
  점유되지 않는다.
- `fetch(Type)`는 blocking 접근으로 테스트·CLI 전용이다.

자세한 규칙은 [7. 비동기](07-async.ko.md)에서 다룬다.

## 기능 한눈에 보기

- 메서드: `GET` `POST` `PUT` `DELETE` `PATCH` `HEAD` `OPTIONS`
- body: typed JSON · raw · form-urlencoded · multipart/form-data · chunked streaming 업로드
- 응답: raw · typed JSON · streaming 다운로드
- connection pool(네이티브), redirect 추적, transport retry, cookie jar
- 인증: Basic · Bearer · proxy Basic · mTLS client certificate
- HTTPS/TLS 검증, test certificate trust
- HTTP proxy
- gzip/deflate 응답 투명 해제

[다음: 시작하기 →](02-getting-started.ko.md)
