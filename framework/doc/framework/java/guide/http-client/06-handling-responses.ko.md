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

## body만 받기

```java
CompletionStage<PlayerProfile> profile = client.get("/players/7281").fetch(PlayerProfile.class);
```

`fetch(Type)`는 `submit(Type)`과 같이 검증·디코드하고 `HttpResponse<T>`를 벗겨 body만
전달한다. status나 header를 함께 봐야 할 때만 `submit(Type)`을 사용한다.

## status 처리 정리

| 경로 | 4xx/5xx |
|------|---------|
| `submitRaw()` | status를 그대로 돌려준다(예외 없음) |
| `submit(Type)` / `fetch(Type)` | 예외 |

[다음: 비동기 →](07-async.ko.md)
