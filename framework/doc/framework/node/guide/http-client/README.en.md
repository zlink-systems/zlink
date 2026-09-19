# Node/TypeScript HTTP Client

The user guide to `@zlink-systems/http-client`. It lets handlers inside the server framework and tools outside the mesh
call external HTTP APIs in the zlink call-builder form. The five languages share one semantics, and
chapters 01–11 of this guide are generated from a common source shared by all five.

| Order | Document | Content |
|----|------|------|
| 1 | [HTTP Client Overview](01-overview.en.md) | When to use it and what it does not take on |
| 2 | [Installation and the First Request](02-getting-started.en.md) | Installing the package, one client, a first typed response |
| 3 | [Making Requests](03-making-requests.en.md) | Method, path, query, header, per-request timeout |
| 4 | [Request Body](04-request-body.en.md) | JSON, form, multipart and the exclusivity rule |
| 5 | [Handling Responses](05-handling-responses.en.md) | Typed, raw, body only, compressed responses |
| 6 | [Authentication, TLS and Proxy](06-auth-tls-proxy.en.md) | Basic/Bearer, trusted certificates, mTLS, proxy |
| 7 | [Streaming](07-streaming.en.md) | Download sink and chunked upload |
| 8 | [Client and Request Lifetime](08-client-lifecycle.en.md) | builder → client → terminator, option table, execution model |
| 9 | [Redirect, Retry and Cookie](09-redirect-retry-cookie.en.md) | Follow rules, backoff, cookie jar semantics |
| 10 | [Response Rules](10-response-rules.en.md) | Path per status, decode, size limit, decompression |
| 11 | [Error Handling](11-error-handling.en.md) | The five error kinds and the retry decision |

The file number identifies the same chapter regardless of language.

## Related Documents

- Server guide: [Node/TypeScript Server Guide](../server/README.en.md)
- Stream Connector guide: [Node/TypeScript Stream Connector](../stream-connector/README.en.md)
