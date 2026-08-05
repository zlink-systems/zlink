[← 목차](README.ko.md)

# 6. Response 다루기

## raw 응답

`awaitRaw()`는 `RawHttpResponse`까지 suspend한다.

```kotlin
val response = client.get("/players/7281").awaitRaw()
val status = response.status()
val body = response.body()
val contentType = response.headers()["content-type"]
```

응답 헤더 이름은 소문자다.

## typed JSON 응답

`await(type)` / `await<T>()`는 응답을 JSON으로 디코드해 `HttpResponse<T>`까지 suspend한다.

```kotlin
val response = client.get("/players/7281").await<PlayerProfile>()
val profile = response.body()    // 디코드된 DTO
val raw = response.rawBody()     // 원본 응답 텍스트
```

- status가 **400 이상**이면 `ZLinkFrameworkException`을 던진다.
- 본문 JSON 디코드 실패도 `ZLinkFrameworkException`으로 보고된다.

## body 바로 받기

```kotlin
val profile = client.get("/players/7281").fetch<PlayerProfile>()
```

`fetch<T>()`는 typed body를 직접 돌려주는 suspend 확장이다(`await<T>().body()` 편의).
blocking이 아니므로 handler에서도 쓸 수 있다.

## status 처리 정리

| 경로 | 4xx/5xx |
|------|---------|
| `awaitRaw()` | status를 그대로 돌려준다(예외 없음) |
| `await<T>()` / `fetch<T>()` | 예외 |

[다음: 코루틴 →](07-coroutines.ko.md)
