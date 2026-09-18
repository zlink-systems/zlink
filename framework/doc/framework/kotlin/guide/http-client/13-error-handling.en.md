[← Table Of Contents](README.en.md)

# 13. Error Handling

Failures are reported as `ZLinkFrameworkException` (`systems.zlink.framework.errors`).

## Exception Model

`ZLinkFrameworkException` is `RuntimeException`-based and exposes a `ZLinkFrameworkErrorKind`
through `kind()`. It carries no `isRetriable`-style flag. The http-client uses `PROTOCOL_ERROR`
and `INTERNAL_FAILURE`. The suspend extensions unwrap the `CompletionException`
envelope and throw the original cause.

| Situation | `kind()` |
|------|------|
| Configuration/request validation failure (base url, path, single body source, proxy scheme, 0 timeout, etc.) | `PROTOCOL_ERROR` |
| status ≥ 400 (`await`/`fetch`) | `INTERNAL_FAILURE` |
| Redirect limit exceeded | `INTERNAL_FAILURE` |
| Response JSON decode failure | `PROTOCOL_ERROR` (with a cause) |
| Compressed body corruption | `PROTOCOL_ERROR` |
| Compression decoded size exceeded / body size exceeded | `INTERNAL_FAILURE` |
| Transport failure (connection error, timeout) | `INTERNAL_FAILURE` (cause is `IOException` or `TimeoutException`) |

## Deciding On A Retry

The retry decision is internally made on whether the cause is an **`IOException`**,
**`UncheckedIOException`**, or **`TimeoutException`**. Status-code failures (4xx/5xx) are not
retried. Streaming requests are excluded from retry
([Chapter 10](10-redirects-retries-cookies.en.md)). If `retry` is configured, such failures are
retried. The public surface does not expose that decision, so the application decides what to do
next by checking the operation's idempotency.

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
