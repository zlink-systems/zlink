[← Table Of Contents](README.en.md)

# 10. Redirect · Retry · Cookie

All three features are **opt-in**. If not turned on, a redirect response is returned as-is, a
failure is reported with no retry, and cookies are ignored.

## Automatic Redirect Tracking

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal")
                .follow_redirects ()        // default limit of 5
                .build ();

// 302 → Location tracked → the final 200 response is returned
auto game = client.get ("/games/latest").fetch<game_t> ();
```

The semantics follow browser/general-client convention.

| status | method/body handling |
|--------|------------------|
| `301`, `302` | `POST` is rewritten to `GET`, dropping the body. Other methods are preserved |
| `303` | always rewritten to `GET`, dropping the body |
| `307`, `308` | both method and body are preserved |

- Location supports an absolute URL (`https://other-host/...`) and an absolute path (`/games/42`).
  A relative path (`../x`) is not supported and closes as `request_failed`.
- If the absolute URL points to a different origin from the original request, the `Authorization`
  header isn't sent again. A redirect within the same origin keeps the authentication header. Since a
  differently-named secret header can't be distinguished from an ordinary header, don't put one into
  `default_header` or a per-request `header`.
- Exceeding the limit (`follow_redirects(max)`) closes with `request_failed`
  ("exceeded the redirect limit").
- When turned off (the default), a 3xx response is returned as-is, so you can branch on it directly.

```cpp
auto raw = client.get ("/games/latest").submit_raw ().result ();
if (raw && raw.value ().status == 302) {
    const auto &location = raw.value ().headers.at ("location");
}
```

## Retry

`retry(attempts)` retries only **retriable transport failures** (a dropped connection, timeout).
The interval between attempts is exponential backoff + full jitter (default 50ms, doubling per
attempt, capped at 1 second, randomized between 0 and the cap).

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal")
                .retry (2)                  // 1 initial + 2 retries = 3 total at most
                .timeout (std::chrono::seconds (2))
                .build ();
```

What is **not** retried:

- **HTTP status failures (4xx/5xx)** — the exchange itself succeeded. If a status-based retry is
  needed for something like 503, the caller checks the status and repeats it directly.
- **`body_stream` uploads and `download`** — because they can't be rewound
  ([8. Streaming](08-streaming.en.md)).

When turning on retry for a non-idempotent request like POST, confirm the server can tolerate
duplicates, e.g., through an idempotency key.

```cpp
client.post ("/games")
  .header ("x-idempotency-key", request_id)
  .body (create_game_http_req_t{.game_name = "ranked-match-0611"})
  .submit<create_game_http_res_t> ();
```

## Cookie Jar

Turning on `cookies()` enables the in-memory cookie jar. It stores `Set-Cookie` and carries it on
subsequent requests as the `Cookie` header. Used when calling a session-cookie-based API.

```cpp
auto portal = zlink::http_client::client_t::create ("https://ops-portal.example.internal")
                .cookies ()
                .build ();

portal.post ("/login")
  .form ("username", "ops-bot")
  .form ("password", ops_password)
  .submit_raw ().result ();                  // Set-Cookie: session=... stored

auto dashboards = portal.get ("/api/dashboards").fetch<dashboard_list_t> ();
// Cookie: session=... is carried automatically
```

Attributes the jar reflects and ignores:

| Attribute | Handling |
|------|------|
| `Path` | sent only on requests whose path prefix matches |
| `Secure` | sent only on https requests |
| `Max-Age` | deleted immediately if 0 or below |
| `Domain`, `Expires`, `HttpOnly`, `SameSite` | ignored (stored per host, no persistence) |

The jar holds up to 128 per host, removing the oldest first once exceeded. It's kept only for the
client (runtime)'s lifetime — there's no disk persistence.

[Next: Proxy →](11-proxy.en.md)
