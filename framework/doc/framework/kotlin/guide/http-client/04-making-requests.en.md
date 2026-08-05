[← Table Of Contents](README.en.md)

# 4. Making Requests

## HTTP Methods

```kotlin
client.get("/players/7281")
client.post("/games")
client.put("/games/42")
client.delete("/games/42")
client.patch("/games/42")
client.head("/games/42")
client.options("/games")
```

The path must start with `/`. If `baseUrl` has a path prefix, it's combined with the prefix (e.g.,
base `http://h/api` + path `/games` → `/api/games`).

## Query Parameters

```kotlin
client.get("/search").query("q", "ranked match").query("limit", "20").awaitRaw()
// → /search?q=ranked%20match&limit=20
```

## Headers

```kotlin
client.get("/players/7281").header("x-trace-id", "abc-123").awaitRaw()
```

Merged with the client's default headers (`defaultHeader`), and for the same name, the per-request
value wins.

## Per-Request Timeout

```kotlin
client.get("/slow").timeout(Duration.ofSeconds(10)).awaitRaw()
```

[Next: Request Body →](05-request-body.en.md)
