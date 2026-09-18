[← Table Of Contents](README.en.md)

# 13. Error Handling

Failures are reported as `ZLinkFrameworkException` (`systems.zlink.framework.errors`).

## Exception Model

`ZLinkFrameworkException` is `RuntimeException`-based and exposes a `ZLinkFrameworkErrorKind`
through `kind()`. It carries no `isRetriable`-style flag. The http-client uses `PROTOCOL_ERROR`
and `INTERNAL_FAILURE`.

| Situation | `kind()` |
|------|------|
| Configuration/request validation failure (base_url, path, single body source, proxy scheme, 0 timeout, etc.) | `PROTOCOL_ERROR` |
| status ≥ 400 (`submit`/`fetch`) | `INTERNAL_FAILURE` |
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
retried through asynchronous composition. The public surface does not expose that decision, so the
application decides what to do next by checking the operation's idempotency.

## Exception Path Summary

```java
client.post("/games").body(req).submit(CreateGameRes.class)
    .exceptionally(error -> {
        Throwable cause = error.getCause();   // unwrap CompletionException
        if (cause instanceof ZLinkFrameworkException) {
            // 4xx/5xx, transport, decode failure, etc.
        }
        throw new CompletionException(error);
    });
```

`fetch(Type)` delivers the same failure as an exceptional completion of its stage.

[← Table Of Contents](README.en.md)
