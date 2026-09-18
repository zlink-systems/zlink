[← 목차](README.ko.md)

# 2. 시작하기

## 의존성

```kotlin
// build.gradle.kts
dependencies {
    implementation("systems.zlink:zlink-http-client:0.16.0")
}
```

```java
import systems.zlink.httpclient.ZLinkHttpClient;
```

## 첫 요청

```java
try (ZLinkHttpClient client = ZLinkHttpClient.create("http://127.0.0.1:18080").build()) {
    HttpResponse<PlayerProfile> player =
        client.get("/players/7281").submit(PlayerProfile.class).toCompletableFuture().join();
    System.out.println(player.body().name());
}
```

- `create(baseUrl)`로 builder를 시작하고 `.build()`로 client를 만든다.
- client는 재사용 가능하고 thread-safe하다. `AutoCloseable`이므로 try-with-resources로
  수명을 관리하면 내부 `HttpClient`/executor가 정리된다.

## 한 줄 요청

단발 요청은 `build()`를 생략하고 builder에서 바로 메서드를 호출할 수 있다.

```java
HttpResponse<CreateGameRes> res = ZLinkHttpClient.create("https://game-api.example.internal")
    .post("/games")
    .body(new CreateGameReq("ranked-match-0611"))
    .submit(CreateGameRes.class)
    .toCompletableFuture().join();
```

## body만 받기

```java
CompletionStage<Leaderboard> board = ZLinkHttpClient.create("http://127.0.0.1:18080")
    .get("/leaderboard").fetch(Leaderboard.class);
```

`fetch(Type)`는 `CompletionStage<T>`로 디코드된 body만 전달한다. 실패는 stage의 예외 완료로
보고된다. 테스트·CLI에서는 `.toCompletableFuture().join()`으로 값을 꺼내고, handler
스레드에서는 `thenCompose`로 합성한다([7장](07-async.ko.md)).

[다음: Client 구성 →](03-client-configuration.ko.md)
