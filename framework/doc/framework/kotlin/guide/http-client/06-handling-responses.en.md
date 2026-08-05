[← Table Of Contents](README.en.md)

# 6. Handling Responses

## Raw Response

`awaitRaw()` suspends all the way to a `RawHttpResponse`.

```kotlin
val response = client.get("/players/7281").awaitRaw()
val status = response.status()
val body = response.body()
val contentType = response.headers()["content-type"]
```

Response header names are lowercase.

## Typed JSON Response

`await(type)` / `await<T>()` decodes the response as JSON, suspending all the way to an
`HttpResponse<T>`.

```kotlin
val response = client.get("/players/7281").await<PlayerProfile>()
val profile = response.body()    // the decoded DTO
val raw = response.rawBody()     // the original response text
```

- If status is **400 or above**, it throws `ZLinkFrameworkException`.
- A body JSON decode failure is also reported as `ZLinkFrameworkException`.

## Getting The Body Directly

```kotlin
val profile = client.get("/players/7281").fetch<PlayerProfile>()
```

`fetch<T>()` is a suspend extension that directly returns the typed body (a convenience for
`await<T>().body()`). Since it's not blocking, it can also be used in a handler.

## Status Handling Summary

| Path | 4xx/5xx |
|------|---------|
| `awaitRaw()` | returns the status as-is (no exception) |
| `await<T>()` / `fetch<T>()` | exception |

[Next: Coroutines →](07-coroutines.en.md)
