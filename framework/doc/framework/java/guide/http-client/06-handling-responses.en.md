[← Table Of Contents](README.en.md)

# 6. Handling Responses

## Raw Response

`submitRaw()` returns a `CompletionStage<RawHttpResponse>`.

```java
RawHttpResponse response = client.get("/players/7281").submitRaw().toCompletableFuture().join();
int status = response.status();
String body = response.body();
String contentType = response.headers().get("content-type");
```

Response header names are lowercase.

## Typed JSON Response

`submit(Type)` decodes the response as JSON and returns a `CompletionStage<HttpResponse<T>>`.

```java
HttpResponse<PlayerProfile> response =
    client.get("/players/7281").submit(PlayerProfile.class).toCompletableFuture().join();
PlayerProfile profile = response.body();   // the decoded DTO
String raw = response.rawBody();            // the original response text
```

- If status is **400 or above**, it throws `ZLinkFrameworkException` (the stage completes
  exceptionally).
- A body JSON decode failure is also reported as `ZLinkFrameworkException`.

## Blocking Unwrapping

```java
PlayerProfile profile = client.get("/players/7281").fetch(PlayerProfile.class);
```

`fetch(Type)` directly returns the typed body and throws failures as an exception. It's for
test/CLI use only.

## Status Handling Summary

| Path | 4xx/5xx |
|------|---------|
| `submitRaw()` | returns the status as-is (no exception) |
| `submit(Type)` / `fetch(Type)` | exception |

[Next: Async →](07-async.en.md)
