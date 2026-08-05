[← Table Of Contents](README.en.md)

# 4. Making Requests

## HTTP Methods

```csharp
client.Get("/players/7281");
client.Post("/games");
client.Put("/games/42");
client.Delete("/games/42");
client.Patch("/games/42");
client.Head("/games/42");
client.Options("/games");
```

The path must start with `/`. If `BaseUrl` has a path prefix, it's combined with the prefix (e.g.,
base `http://h/api` + path `/games` → `/api/games`).

## Query Parameters

`Query(name, value)` adds a percent-encoded query parameter.

```csharp
await client.Get("/search")
    .Query("q", "ranked match")
    .Query("limit", "20")
    .AsyncRaw();
// → /search?q=ranked%20match&limit=20
```

## Headers

Per-request headers are added with `Header(name, value)`. They're merged with the client's default
headers (`DefaultHeader`), and for the same name, the per-request value wins.

```csharp
await client.Get("/players/7281")
    .Header("x-trace-id", "abc-123")
    .AsyncRaw();
```

## Per-Request Timeout

```csharp
await client.Get("/slow").Timeout(TimeSpan.FromSeconds(10)).AsyncRaw();
```

[Next: Request Body →](05-request-body.en.md)
