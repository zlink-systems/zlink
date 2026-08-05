[← Table Of Contents](README.en.md)

# 10. Redirect · Retry · Cookie

These three features are **implemented directly in the wrapper**, because `java.net.http`'s built-in
semantics differ from the ZLink contract.

## Redirect

Enabled with `followRedirects(max)`. `HttpClient.Redirect.NEVER` is set, and the wrapper runs the
redirect loop (the `Redirect` enum has no notion of a count).

- Tracked statuses: `301`, `302`, `303`, `307`, `308` + `Location` header.
- Method rewrite: `303`, or `301`/`302` + `POST`, is rewritten to `GET` with the body removed.
- **`Authorization` preservation rule**: `Authorization` is preserved on a same-origin redirect
  (identical scheme+host+port) and removed cross-origin.
- Exceeding the `max` count fails with an exception.
- Supported locations: absolute (`http(s)://...`) and path-absolute (`/...`).
- The redirect loop is composed as a `CompletionStage` chain, not occupying a thread between hops.

## Retry

`retry(attempts)` retries transport failures (exponential backoff + full jitter interval — default
50ms, doubling per attempt, capped at 1 second, randomized between 0 and the cap; composed
asynchronously).

- Retried target: **retriable transport failures** (`IOException` — connection errors, timeout,
  etc.). Status codes (4xx/5xx) themselves are not retried.
- **Streaming (a download sink or upload provider) can't be rewound, so it's excluded from retry.**

## Cookie Jar

Enabled with `cookies()`. Instead of the JDK's `CookieManager` (full RFC 6265), a wrapper-owned jar
is used. It follows narrow semantics:

- Stored by exact host match (the `Domain` attribute is not supported).
- Default `Path=/`. Only the `Path`/`Secure`/`Max-Age` attributes are interpreted; `Domain`/`Expires`
  are ignored.
- Deleted if `Max-Age<=0`.
- A secure cookie is sent only on a secure (https) request.
- Up to 128 per host; oldest removed first once exceeded.

[Next: Proxy →](11-proxy.en.md)
