[← Table Of Contents](README.en.md)

# 13. Error Handling

Failures are reported as `ZLinkFrameworkException` (`systems.zlink.framework.errors`).

## Exception Model

`ZLinkFrameworkException` doesn't expose a kind enum or `isRetriable` (it's `RuntimeException`-based,
distinguished by message/exception type).

| Situation | Reported As |
|------|------|
| Configuration/request validation failure (base url, path, single body source, proxy scheme, 0 timeout, etc.) | `ZLinkFrameworkException` |
| status ≥ 400 (`await`/`fetch`) | `ZLinkFrameworkException` |
| Redirect limit exceeded | `ZLinkFrameworkException` |
| Response JSON decode failure | `ZLinkFrameworkException` (with a cause) |
| Compressed body corruption | `ZLinkFrameworkException` |
| Compression decoded size exceeded / body size exceeded | `ZLinkFrameworkException` |
| Transport failure (connection error, timeout) | `ZLinkFrameworkException` (cause is `IOException`) |

## Retriable

The retry decision is internally made based on whether it's an **`IOException`** (transport error,
timeout). Status-code failures (4xx/5xx) are not retried. Streaming requests are excluded from
retry ([Chapter 10](10-redirects-retries-cookies.en.md)). If `retry` is configured, retriable
failures are retried.

## Exception Path

A suspend call is caught directly with `try`/`catch`.

```kotlin
try {
    val res = client.post("/games").body(req).await<CreateGameRes>()
    // success
} catch (e: ZLinkFrameworkException) {
    // 4xx/5xx, transport, decode failure, etc.
}
```

[← Table Of Contents](README.en.md)
