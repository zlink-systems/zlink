[← Table Of Contents](README.en.md)

# 10. Redirect · Retry · Cookie

Since the .NET native handler's built-in semantics for these three features differ from the ZLink
contract, native behavior is turned off and they're **implemented directly in the wrapper**.

## Redirect

Enabled with `FollowRedirects(max)`. Native auto-redirect is turned off, and the wrapper runs the
redirect loop.

- Tracked statuses: `301`, `302`, `303`, `307`, `308` + `Location` header.
- Method rewrite: `303`, or `301`/`302` + `POST`, is rewritten to `GET` with the body removed.
- **`Authorization` preservation rule**: `Authorization` is preserved on a same-origin redirect
  (identical scheme+host+port) and removed cross-origin. .NET's native auto-redirect doesn't
  preserve `Authorization` even same-origin, which is why the wrapper loop is needed.
- Exceeding the `max` count fails with `InternalFailure`.
- Supported locations: absolute (`http(s)://...`) and path-absolute (`/...`). Other relative paths
  are not supported.

## Retry

`Retry(attempts)` retries transport failures (exponential backoff + full jitter interval — default
50ms, doubling per attempt, capped at 1 second, randomized between 0 and the cap).

- Retried target: **retriable transport failures** (connection errors, timeout, etc.). Status codes
  (4xx/5xx) themselves are not retried.
- **Streaming (a download sink or upload provider) can't be rewound, so it's excluded from retry.**

## Cookie Jar

Enabled with `Cookies()`. Instead of the native `CookieContainer` (full RFC 6265), a wrapper-owned
jar is used. It follows narrow semantics:

- Stored by exact host match (the `Domain` attribute is not supported).
- Default `Path=/`. Only the `Path`/`Secure`/`Max-Age` attributes are interpreted; `Domain`/`Expires`
  are ignored.
- Deleted if `Max-Age<=0`.
- A secure cookie is sent only on a secure (https) request.
- Up to 128 per host; oldest removed first once exceeded.

[Next: Proxy →](11-proxy.en.md)
