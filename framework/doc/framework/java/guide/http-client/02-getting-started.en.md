[← Table Of Contents](README.en.md)

# 2. Getting Started

## Dependency

```kotlin
// build.gradle.kts
dependencies {
    implementation("systems.zlink:zlink-http-client:0.16.0")
}
```

```java
import systems.zlink.httpclient.ZLinkHttpClient;
```

## First Request

```java
try (ZLinkHttpClient client = ZLinkHttpClient.create("http://127.0.0.1:18080").build()) {
    HttpResponse<PlayerProfile> player =
        client.get("/players/7281").submit(PlayerProfile.class).toCompletableFuture().join();
    System.out.println(player.body().name());
}
```

- Start a builder with `create(baseUrl)` and build the client with `.build()`.
- The client is reusable and thread-safe. Since it's `AutoCloseable`, managing its lifetime with
  try-with-resources cleans up the internal `HttpClient`/executor.

## One-Line Request

For a one-off request, you can skip `build()` and call methods directly on the builder.

```java
HttpResponse<CreateGameRes> res = ZLinkHttpClient.create("https://game-api.example.internal")
    .post("/games")
    .body(new CreateGameReq("ranked-match-0611"))
    .submit(CreateGameRes.class)
    .toCompletableFuture().join();
```

## Taking Just The Body

```java
CompletionStage<Leaderboard> board = ZLinkHttpClient.create("http://127.0.0.1:18080")
    .get("/leaderboard").fetch(Leaderboard.class);
```

`fetch(Type)` hands over only the decoded body as a `CompletionStage<T>`. A failure is reported as
an exceptional completion of the stage. In a test or CLI, take the value with
`.toCompletableFuture().join()`; on a handler thread, compose it with `thenCompose`
([Chapter 7](07-async.en.md)).

[Next: Client Configuration →](03-client-configuration.en.md)
