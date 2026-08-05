[← Table Of Contents](README.en.md)

# 3. Client Configuration

The builder gathers the URL, timeout, authentication, and transport policy the client uses in
common.

## Builder Options

[Common Spec Chapter 2](../../../common/spec/http-client/02-client-builder.en.md) is authoritative
for defaults.

| Option | Effect | Default |
| --- | --- | --- |
| `BaseUrl(url)` | Base URL for every request | none (required) |
| `Timeout(span)` | Timeout per attempt (overridable per request) | **3000ms** |
| `DefaultHeader(n, v)` | Default header attached to every request | none |
| `BasicAuth(u, p)` / `BearerToken(t)` | `Authorization` header | off |
| `MaxResponseBodySize(bytes)` | Response body cap (decoded basis) | **16 MiB** |
| `TrustCertificateFile(path)` | Add a trusted certificate | system root |
| `ClientCertificateFile(cert, key)` | mTLS client certificate | off |
| `FollowRedirects(max)` | Redirect tracking (**5** if no argument given) | off |
| `Retry(attempts)` | Transport failure retry (1+n total attempts) | off |
| `Cookies()` | Enable the cookie jar | off |
| `Proxy(url)` / `ProxyBasicAuth(u, p)` | HTTP proxy and authentication | off |
| `Compression()` | Transparent gzip/deflate decoding | off |
| `Codecs(configure)` | Register a `.NET` codec extension | JSON |

## Registering With The Framework Server

A client used in a Spot handler is registered in DI with a name. A server registration reuses the
connection pool and connects callback completion to a new turn on the current Spot's execution line.

```csharp
services.AddZLinkHttpClient("player-api", http => http
    .BaseUrl("https://player-api.internal") // Fixes the base URL of the client injected under this name.
    .Timeout(TimeSpan.FromSeconds(3)));      // Manages the external API's per-attempt timeout in one place.
```

A handler is injected the `ZLinkHttpServerClient` of the same name. `ZLinkHttpClient` created by the
static factory is for client-side code, so it doesn't provide a one-way `Async()`. Neither client
provides a `Yield` on the HTTP request builder.

## Client Reuse And The Connection Pool

Don't create a new client per request — **create it once and reuse it**. Don't use the one-shot
one-liner (§2) for repeated calls.

## Per-Request Timeout Override

```csharp
await client.Get("/slow-report").Timeout(TimeSpan.FromSeconds(30)).AsyncRaw();
```

[Next: Making Requests →](04-making-requests.en.md)
