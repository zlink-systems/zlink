[← Table Of Contents](README.en.md)

# 13. Error Handling

Failures are reported as `ZLinkFrameworkException` (`systems.zlink.framework.errors`).

## Exception Model

**The Java framework's `ZLinkFrameworkException` doesn't expose a kind enum or `isRetriable`**
(it's `RuntimeException`-based, distinguished by message/exception type, the Java idiom).
http-client follows this Java idiom too.

| Situation | Reported As |
|------|------|
| Configuration/request validation failure (base_url, path, single body source, proxy scheme, 0 timeout, etc.) | `ZLinkFrameworkException` |
| status ≥ 400 (`submit`/`fetch`) | `ZLinkFrameworkException` |
| Redirect limit exceeded | `ZLinkFrameworkException` |
| Response JSON decode failure | `ZLinkFrameworkException` (with a cause) |
| Compressed body corruption | `ZLinkFrameworkException` |
| Compression decoded size exceeded / body size exceeded | `ZLinkFrameworkException` |
| Transport failure (connection error, timeout) | `ZLinkFrameworkException` (cause is `IOException`) |

## Retriable

The retry decision is internally made based on whether it's an **`IOException`** (transport error,
`HttpTimeoutException`). Status-code failures (4xx/5xx) are not retried. Streaming requests are
excluded from retry ([Chapter 10](10-redirects-retries-cookies.en.md)). If `retry` is configured,
retriable failures are retried through asynchronous composition.

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

In tests/CLI, `fetch(Type)` throws failures as a synchronous exception.

[← Table Of Contents](README.en.md)
