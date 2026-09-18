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

## Taking Just The Body

```java
CompletionStage<PlayerProfile> profile = client.get("/players/7281").fetch(PlayerProfile.class);
```

`fetch(Type)` validates and decodes exactly as `submit(Type)` does, then strips the
`HttpResponse<T>` and hands over the body alone. Use `submit(Type)` only when you also need the
status or the headers.

## Status Handling Summary

| Path | 4xx/5xx |
|------|---------|
| `submitRaw()` | returns the status as-is (no exception) |
| `submit(Type)` / `fetch(Type)` | exception |

[Next: Async →](07-async.en.md)
