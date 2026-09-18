[← Table Of Contents](README.en.md)

# 6. Handling Responses

## Raw Response

`submitRaw()` returns a `RawHttpResponse`.

```ts
const response = await client.get('/players/7281').submitRaw();
const status = response.status;
const body = response.body;                 // string
const contentType = response.headers['content-type'];
```

Response header names are lowercase.

## Typed JSON Response

`async<T>()` decodes the response as JSON and returns an `HttpResponse<T>`.

```ts
const response = await client.get('/players/7281').async<PlayerProfile>();
const profile = response.body;     // the decoded object
const raw = response.rawBody;      // the original response text
```

- If status is **400 or above**, it throws `ZLinkFrameworkException(InternalFailure)`.
- A body JSON decode failure is reported as `ZLinkFrameworkException(ProtocolError)`.
- JSON parsing applies prototype-pollution (`__proto__`/`constructor`/`prototype`) defenses.

## Status Handling Summary

| Path | 4xx/5xx |
|------|---------|
| `submitRaw()` | returns the status as-is (no exception) |
| `async<T>()` | `InternalFailure` exception |
| `fetch<T>()` | validates the same as `async<T>()`, returning only the decoded body as `Promise<T>` |

```ts
const profile = await client.get('/players/7281').fetch<PlayerProfile>();
// An async convenience surface for when you just need the body, with no response wrapper.
```

[Next: Async →](07-async.en.md)
