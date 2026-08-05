# ZLink HTTP Client For .NET — User Guide

`Zlink.HttpClient` is a general-purpose HTTP client for sending HTTP requests from .NET. It is used
with a zlink-style fluent builder, and the public surface does not expose `System.Net.Http` handler
types.

```csharp
using Zlink.HttpClient;

var game = await ZLinkHttpClient.Create("https://game-api.example.internal")
    .Post("/games")
    .Body(new CreateGameReq("ranked-match-0611"))
    .Fetch<CreateGameRes>();
```

## Table Of Contents

| Chapter | Document | Content |
|----|------|------|
| 1 | [Overview](01-overview.en.md) | Design philosophy, deliverable boundary, execution model |
| 2 | [Getting Started](02-getting-started.en.md) | Project reference, first request, one-line request |
| 3 | [Client Configuration](03-client-configuration.en.md) | Builder options, client reuse, native handler mapping |
| 4 | [Making Requests](04-making-requests.en.md) | HTTP methods, query parameters, headers, request timeout |
| 5 | [Request Body](05-request-body.en.md) | JSON DTO, raw, form, multipart, streaming upload |
| 6 | [Handling Responses](06-handling-responses.en.md) | Response structure, `Async`, status handling |
| 7 | [Async](07-async.en.md) | `Async`, callback, I/O worker and Spot turn rules |
| 8 | [Streaming](08-streaming.en.md) | `DownloadAsync(sink)` download, chunked upload |
| 9 | [Authentication And TLS](09-authentication-tls.en.md) | Basic/Bearer, HTTPS verification, mTLS |
| 10 | [Redirect · Retry · Cookie](10-redirects-retries-cookies.en.md) | Redirect semantics, retry policy, cookie jar |
| 11 | [Proxy](11-proxy.en.md) | HTTP proxy, proxy authentication |
| 12 | [Compression](12-compression.en.md) | Transparent gzip/deflate decoding |
| 13 | [Error Handling](13-error-handling.en.md) | Error kind mapping, retriable, exception paths |

## Quick Guide

- If this is your first time → [2. Getting Started](02-getting-started.en.md)
- If you're calling this inside a server handler → check [7. Async](07-async.en.md)'s blocking rule
  first
- How failures are reported → [13. Error Handling](13-error-handling.en.md)

The formal contract and regression test axes are owned by the spec document
[dotnet-http-client.ko.md](../../../common/spec/http-client/languages/dotnet/dotnet-http-client.en.md).
This guide covers usage.
