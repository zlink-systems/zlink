[← Table Of Contents](README.en.md)

# 2. Getting Started

## Dependency

```kotlin
// build.gradle.kts
dependencies {
    implementation(project(":zlink-http-client"))
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

## Blocking One-Liner (Test/CLI)

```java
Leaderboard board = ZLinkHttpClient.create("http://127.0.0.1:18080")
    .get("/leaderboard").fetch(Leaderboard.class);
```

`fetch(Type)` waits for the result, returning the typed body and throwing failures as an exception.
It's not used on a handler thread ([Chapter 7](07-async.en.md)).

[Next: Client Configuration →](03-client-configuration.en.md)
