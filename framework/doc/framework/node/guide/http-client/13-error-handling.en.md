[← Table Of Contents](README.en.md)

# 13. Error Handling

Failures are reported as `ZLinkFrameworkException` (`@zlink-systems/framework`). It exposes `kind`
(`ZLinkFrameworkErrorKind`) to distinguish the failure. The exception itself carries no retry hint.

## Error Kind Mapping

| Situation | kind |
|------|------|
| Configuration/request validation failure (base_url, path, single body source, proxy scheme, 0 timeout, etc.) | `ProtocolError` |
| status ≥ 400 (`async<T>`) | `InternalFailure` |
| Redirect limit exceeded | `Unavailable` |
| Response JSON decode failure | `ProtocolError` |
| Compressed body corruption | `ProtocolError` |
| Compression decoded size exceeded / body size exceeded | `Unavailable` |
| Transport failure (connection error, etc.) | `Unavailable` |
| Request timeout | `DeadlineExceeded` |

## Timeout

A timeout is reported as `DeadlineExceeded`. If `retry` is configured, it's retried.

## Deciding On A Retry

`retry(attempts)` automatically retries `Unavailable` and `DeadlineExceeded`.
Status-code failures (4xx/5xx) and `ProtocolError` are not retried. Streaming requests are excluded
from retry ([Chapter 10](10-redirects-retries-cookies.en.md)). The exception carries no
`isRetriable` flag, so the application decides what to do next by checking the operation's
idempotency.

## Exception Path Summary

```ts
try {
  const res = await client.post('/games').body(req).async<CreateGameRes>();
} catch (error) {
  if (error instanceof ZLinkFrameworkException) {
    switch (error.kind) {
      case ZLinkFrameworkErrorKind.InternalFailure: /* HTTP status 400 or above */ break;
      case ZLinkFrameworkErrorKind.Unavailable: /* transport failure */ break;
      case ZLinkFrameworkErrorKind.DeadlineExceeded: /* request timeout */ break;
      case ZLinkFrameworkErrorKind.ProtocolError: /* request validation or response body decode failure */ break;
      default: break;
    }
  }
}
```

[← Table Of Contents](README.en.md)
