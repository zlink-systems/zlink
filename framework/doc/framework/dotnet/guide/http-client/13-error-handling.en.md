[← Table Of Contents](README.en.md)

# 13. Error Handling

Failures are reported as `ZLinkFrameworkException` (`Zlink.Framework.Contracts.Errors`). `Kind`
(`ZLinkFrameworkErrorKind`) indicates the failure category. Since the exception provides no retry
hint, the application checks the operation's idempotency and side effects to decide its next action.

## Error Kind Mapping

| Situation | `Kind` |
|------|--------|
| Request format, redirect, or response decode error | `ProtocolError` |
| DNS, proxy CONNECT, or target connection failure | `Unavailable` |
| Response body or decompressed size limit exceeded | `CapacityExceeded` |
| Request timeout | `DeadlineExceeded` |
| HTTP status 400 or above | `InternalFailure` |

## Timeout

A timeout is `DeadlineExceeded`. The HTTP client's `Retry(attempts)` is internally applied to
transport failures and timeouts where the request can be re-built, but the application exception
carries no retry hint.

## Retry Decision

Only retriable transport errors and timeouts are retried again, according to the `Retry(attempts)`
setting. HTTP status errors and protocol errors are not retried. Streaming requests are also
excluded from retry ([Chapter 10](10-redirects-retries-cookies.en.md)).

## Exception Path Summary

```csharp
try
{
    var res = await client.Post("/games").Body(req).Fetch<CreateGameRes>();
}
catch (ZLinkFrameworkException ex)
    when (ex.Kind == ZLinkFrameworkErrorKind.DeadlineExceeded)
{
    // It's a timeout. Whether to retry is decided by checking the request's idempotency and side effects.
}
catch (ZLinkFrameworkException ex)
    when (ex.Kind == ZLinkFrameworkErrorKind.Unavailable)
{
    // The current target can't be reached.
}
catch (ZLinkFrameworkException ex)
    when (ex.Kind == ZLinkFrameworkErrorKind.ProtocolError)
{
    // A request format, redirect, or response decode error.
}
```

[← Table Of Contents](README.en.md)
