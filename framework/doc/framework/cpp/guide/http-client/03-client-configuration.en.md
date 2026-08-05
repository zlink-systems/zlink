[← Table Of Contents](README.en.md)

# 3. Client Configuration

All client-level settings are decided on the `client_builder_t` that `client_t::create()` returns.
The moment `build()` is called, the configuration is fixed and a runtime with a connection pool is
created.

## The Full Set Of Options

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal")
                .timeout (std::chrono::seconds (5))
                .max_response_body_size (32 * 1024 * 1024)
                .default_header ("x-service-name", "matchmaker")
                .bearer_token (session_token)
                .trust_certificate_file ("/etc/pki/internal-root-ca.pem")
                .follow_redirects (5)
                .retry (2)
                .cookies ()
                .compression ()
                .build ();
```

| Option | Meaning | Default |
|------|------|--------|
| `base_url(url)` / `create(url)` | The `http://` or `https://` endpoint. Can include a path prefix | required |
| `timeout(duration)` | The request's default timeout. [Overridable per request](04-making-requests.en.md) | 3000ms |
| `max_response_body_size(bytes)` | Max bytes allowed when reading the response body. Applies to both buffered responses and streaming downloads | 16 MiB |
| `default_header(name, value)` | A header carried on every request | none |
| `basic_auth(user, pw)` / `bearer_token(tok)` | `Authorization` header ([Chapter 9](09-authentication-tls.en.md)) | none |
| `trust_certificate_file(path)` | An additional server certificate to trust ([Chapter 9](09-authentication-tls.en.md)) | system CA |
| `client_certificate_file(cert, key)` | mTLS client certificate ([Chapter 9](09-authentication-tls.en.md)) | none |
| `follow_redirects(max = 5)` | Automatic redirect tracking — the limit is 5 if called with no argument ([Chapter 10](10-redirects-retries-cookies.en.md)) | off (enabled when called) |
| `retry(attempts)` | Retriable transport failure retry ([Chapter 10](10-redirects-retries-cookies.en.md)) | off |
| `cookies()` | In-memory cookie jar ([Chapter 10](10-redirects-retries-cookies.en.md)) | off |
| `proxy(url)` / `proxy_basic_auth(user, pw)` | HTTP proxy ([Chapter 11](11-proxy.en.md)) | none |
| `compression()` | gzip/deflate response decoding ([Chapter 12](12-compression.en.md)) | off |

An invalid value (empty base_url, `ftp://` scheme, timeout of 0 or below, 0-byte response body cap,
empty header name, etc.) is thrown immediately as `request_protocol_error` from `build()` or the
corresponding setter — it doesn't pass silently.

A header added via `default_header` is applied as-is even if the redirect target changes. Don't put
secret values directly in `default_header` — use `basic_auth` or `bearer_token` instead. The
`Authorization` header these two authentication APIs build is automatically removed on a
cross-origin redirect.

## base_url And The Path Prefix

If `base_url` includes a path, it's prepended to every request path.

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal/v2")
                .build ();
client.get ("/players/7281");   // actual target: /v2/players/7281
```

## Client Reuse And The Connection Pool

`client_t` is a lightweight handle that shares the internal runtime via `shared_ptr`. Even copies
share the same connection pool and cookie jar.

Requests to the same origin **automatically reuse a keep-alive connection**. If the server has
closed the connection in the meantime (stale), it automatically retries once with a fresh
connection, so the caller doesn't need to worry about it.

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal")
                .build ();

// All three requests reuse the same TCP connection
client.get ("/games/active").submit_raw ().result ();
client.get ("/players/7281").submit_raw ().result ();
client.get ("/leaderboard").submit_raw ().result ();
```

Recommended patterns:

- **Create one client per service and reuse it.** Repeating `create()...build()` for every request
  creates a fresh runtime (pool) each time, losing the keep-alive benefit.
- For a place that only makes one-off requests, the [build()-skipping shortcut](02-getting-started.en.md)
  is more concise.

## Thread Safety

- **It's safe to make requests from multiple threads at the same time** with `client_t` and its
  copies. The connection pool and cookie jar are internally synchronized.
- `client_builder_t`/`request_builder_t` are objects mid-construction, so they're not shared across
  threads.

[Next: Making Requests →](04-making-requests.en.md)
