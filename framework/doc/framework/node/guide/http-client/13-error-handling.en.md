[← Table Of Contents](README.en.md)

# 13. Error Handling

Failures are reported as `ZLinkFrameworkException` (`@zlink-systems/framework`). It exposes `kind`
(`ZLinkFrameworkErrorKind`) and `isRetriable`.

## Error Kind Mapping

| Situation | kind |
|------|------|
| Configuration/request validation failure (base_url, path, single body source, proxy scheme, 0 timeout, etc.) | `requestProtocolError` |
| status ≥ 400 (`submit<T>`) | `requestFailed` |
| Redirect limit exceeded | `requestFailed` |
| Response JSON decode failure | `payloadDecodeFailed` |
| Compressed body corruption | `payloadDecodeFailed` |
| Compression decoded size exceeded / body size exceeded | `requestFailed` |
| Transport failure (connection error, etc.) | `requestFailed` (`isRetriable = true`) |

## Timeout

Node framework's `ZLinkFrameworkErrorKind` has no timeout-specific kind. So in Node, a timeout is
reported as **`ZLinkFrameworkException(requestFailed, isRetriable: true)`**. If `retry` is
configured, it's retried.

## Retriable

Failures where `isRetriable` is `true` (transport errors, timeout) are retried when
`retry(attempts)` is configured. Status-code failures (4xx/5xx) are not retriable. Streaming
requests are excluded from retry ([Chapter 10](10-redirects-retries-cookies.en.md)).

## Exception Path Summary

```ts
try {
  const res = await client.post('/games').body(req).submit<CreateGameRes>();
} catch (error) {
  if (error instanceof ZLinkFrameworkException) {
    switch (error.kind) {
      case ZLinkFrameworkErrorKind.RequestFailed: /* 4xx/5xx or transport/timeout */ break;
      case ZLinkFrameworkErrorKind.PayloadDecodeFailed: /* response body decode failure */ break;
      default: break;
    }
  }
}
```

[← Table Of Contents](README.en.md)
