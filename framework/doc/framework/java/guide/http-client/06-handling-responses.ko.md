[← 목차](README.ko.md)

# 6. Response 다루기

## raw 응답

`submitRaw()`는 `CompletionStage<RawHttpResponse>`를 돌려준다.

```java
RawHttpResponse response = client.get("/players/7281").submitRaw().toCompletableFuture().join();
int status = response.status();
String body = response.body();
String contentType = response.headers().get("content-type");
```

응답 헤더 이름은 소문자다.

## typed JSON 응답

`submit(Type)`는 응답을 JSON으로 디코드해 `CompletionStage<HttpResponse<T>>`를 돌려준다.

```java
HttpResponse<PlayerProfile> response =
    client.get("/players/7281").submit(PlayerProfile.class).toCompletableFuture().join();
PlayerProfile profile = response.body();   // 디코드된 DTO
String raw = response.rawBody();            // 원본 응답 텍스트
```

- status가 **400 이상**이면 `ZLinkFrameworkException`을 던진다(stage가 예외 완료).
- 본문 JSON 디코드 실패도 `ZLinkFrameworkException`으로 보고된다.

## blocking 언래핑

```java
PlayerProfile profile = client.get("/players/7281").fetch(PlayerProfile.class);
```

`fetch(Type)`는 typed body를 직접 돌려주고 실패를 예외로 던진다. 테스트·CLI 전용이다.

## status 처리 정리

| 경로 | 4xx/5xx |
|------|---------|
| `submitRaw()` | status를 그대로 돌려준다(예외 없음) |
| `submit(Type)` / `fetch(Type)` | 예외 |

[다음: 비동기 →](07-async.ko.md)
