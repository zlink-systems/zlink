# 9. Error Model

> [Common contract table of contents](README.en.md)

The HTTP client doesn't build its own exception hierarchy — it uses
the Framework's common error.

## 9.1 Common Kind Set (Contract)

| Kind | Situation |
| --- | --- |
| `ProtocolError` | Builder format, duplicate body source, typed decode, decompression, or redirect format is invalid. |
| `Unavailable` | Network, DNS, proxy CONNECT, or target connection is currently unavailable. |
| `CapacityExceeded` | Exceeded the configured response body byte limit. |
| `DeadlineExceeded` | Exceeded the per-attempt timeout. |
| `InternalFailure` | A typed submit's HTTP status is 400 or above, or an execution failure that can't be classified into the kinds above. |

Caller cancellation isn't converted to a Framework error — it's
delivered as each language's cancelled awaitable. The automatic retry
policy in [Redirect And Retry](06-redirect-retry-cookie.en.md) is
behavior the HTTP client applies within one configured operation, not
a retry hint on the public error.

## 9.2 Per-Language Representation

| Language | Kind | Timeout Representation | Delivery Form |
| --- | --- | --- | --- |
| C++ | Framework common enum | `deadline_exceeded` | `result_t` or exception |
| .NET | `ZLinkFrameworkErrorKind` | `DeadlineExceeded` and inner `TimeoutException` | `ZLinkFrameworkException` |
| Node.js | Framework common kind | `DeadlineExceeded` and `TimeoutError` cause | Exception |
| Java | Framework common enum | `DEADLINE_EXCEEDED` and `HttpTimeoutException` cause | Exception |
| Kotlin | Projects the Java contract into Kotlin notation. | Same as Java. | Exception |

`closed` isn't an HTTP client error kind. A closed state of a response
body stream or transport handle is treated as that object's boundary
state, and converted to whichever kind above fits the actual failure
cause.

## 9.3 Separation Of Automatic Retry And The Error Surface

Whether a retry occurred isn't put into the public exception or
result. An operation configured with `retry(attempts)` only internally
retries a transport failure and timeout. Once every attempt ends, it
returns the last failure's `ErrorKind`, and whether the application
starts a new operation is judged per the common error model.
