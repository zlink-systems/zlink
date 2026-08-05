# 6. Redirect · Retry · Cookie

> [Common contract table of contents](README.en.md)

All three capabilities turn off the native automatic feature and the
wrapper implements it. This is a core contract area where the 5
languages' behavior must match down to the byte level.

## 6.1 Redirect

With `followRedirects(max)` active (default 5 with no argument):

| Status | Method Rewrite | Body |
| --- | --- | --- |
| 301, 302 (GET/HEAD) | Preserved | Preserved |
| 301, 302 (POST) | **Changed to GET** | **Removed** |
| 303 | **Always GET** | **Removed** |
| 307, 308 | Preserved | Preserved (a streaming body is dropped since it can't rewind) |

- `Location` supports absolute/relative URL. An unparseable form is
  `ProtocolError`, and the original request isn't resent.
- **Moving cross-origin removes the `Authorization` header.** It's
  preserved for same-origin (same scheme+host+port).
- Exceeding the bound is `ProtocolError`, and the original request
  isn't resent.
- The body of a redirect intermediate response is consumed (drained)
  but not exposed to the user
  ([Chapter 4 §4.4](04-response-model.en.md)).

## 6.2 Retry And Timeout

- `retry(attempts)`: total attempts = 1 + attempts.
- **Only transport-layer failure and timeout are auto-retry targets.**
  HTTP status (4xx/5xx) isn't retried.
- If there's streaming (an upload provider or download sink), it isn't
  retried.
- The delay between attempts is **exponential backoff + full jitter**:
  bound = `min(1s, 50ms × 2^attempt)`, actual delay = uniform random in
  `[0, bound]` (2026-07-12 R3 promotion — revised from a fixed 50ms. A
  fixed delay causes a concurrent thundering-herd retry against a
  degraded server).
- Timeout applies **per attempt**. A timeout failure is an auto-retry
  target within the configured count. So the `retry(n)` + timeout
  combination is the standard expression of "retry n times if the
  response doesn't come within the timeout."
- There's no total deadline spanning the whole retry in the contract
  (worst-case wait ≈ attempt count × timeout + delay). Language
  deviation: only the cpp coroutine path additionally enforces a total
  deadline spanning retries. The cpp sync path blocking with no total
  deadline is tracked as an implementation defect in the plan document,
  and contract-izing the total deadline is reviewed together with
  [R3](10-revision-candidates.en.md).

## 6.3 Cookie Jar

With `cookies()` active, a deliberately narrowed RFC 6265 subset:

- The storage key is **exact host match** (`Domain` attribute
  ignored).
- Supported attributes: `Path` (default `/`, path-segment prefix
  match), `Secure` (sent only to https), `Max-Age` (`<= 0` deletes
  immediately). `Expires`/`HttpOnly`/`SameSite` are ignored.
- At most **128** per host. Once exceeded, the oldest is evicted
  first.
- A malformed `Set-Cookie` is silently ignored.

## 6.4 Connection Reuse

- keep-alive/pool is delegated to the transport stack (dotnet/java/node).
  cpp has its own pool (key: `scheme|host:port[|proxy]`), and a reused
  connection's exchange failure is retried once on a new connection,
  only for an idempotent method (GET/HEAD/OPTIONS).
- Streaming upload doesn't go through the pool and uses a new
  connection (an explicit cpp rule; other languages handle it inside
  the stack).
