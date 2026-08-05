[← Table Of Contents](README.en.md)

# 6. Handling Responses

## Raw Response

`AsyncRaw()` returns a `RawHttpResponse`.

```csharp
RawHttpResponse response = await client.Get("/players/7281").AsyncRaw();
int status = response.Status;
string body = response.Body;
string contentType = response.Headers["content-type"];
```

`Headers` supports case-insensitive lookup.

## Typed JSON Response

`Async<T>()` decodes the response with the client's codec and returns an `HttpResponse<T>`. Unless a
separate codec extension is registered, JSON is used.

```csharp
HttpResponse<PlayerProfile> response = await client.Get("/players/7281").Async<PlayerProfile>();
PlayerProfile profile = response.Body;     // the decoded DTO
string raw = response.RawBody;             // the original response text
```

- If status is **400 or above**, it throws `ZLinkFrameworkException(InternalFailure)`.
- A body decode failure is reported as `ZLinkFrameworkException(ProtocolError)`.

## When You Only Need The Body

`Fetch<T>()` performs the same decode and returns **only the decoded body**. For a call that doesn't
need to look at status or headers, this is shorter.

```csharp
PlayerProfile profile = await client.Get("/players/7281").Fetch<PlayerProfile>();
```

Use `Async<T>()` to get an `HttpResponse<T>` only when you also need to look at status or headers.

## Status Handling Summary

| Path | 4xx/5xx |
|------|---------|
| `AsyncRaw()` | returns the status as-is (no exception) |
| `Fetch<T>()` / `Async<T>()` / typed callback | `InternalFailure` error |

[Next: Async →](07-async.en.md)
