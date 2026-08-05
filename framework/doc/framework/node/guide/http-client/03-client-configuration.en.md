[← Table Of Contents](README.en.md)

# 3. Client Configuration

The builder gathers client-wide settings. They either map to undici dispatcher settings, or, when
the semantics differ, are handled directly in the wrapper.

## Builder Options

[Common Spec Chapter 2](../../../common/spec/http-client/02-client-builder.en.md) is authoritative
for defaults.

| Option | Effect | Default | Implementation |
| --- | --- | --- | --- |
| `baseUrl(url)` | Base URL for every request | none (required) | wrapper |
| `timeout(ms)` | Timeout per attempt (overridable per request) | **3000ms** | `AbortController` |
| `defaultHeader(n, v)` | Default header attached to every request | none | wrapper |
| `basicAuth(u, p)` / `bearerToken(t)` | `Authorization` header | off | wrapper |
| `maxResponseBodySize(bytes)` | Response body cap (decoded basis) | **16 MiB** | wrapper |
| `trustCertificateFile(path)` | Add a trusted certificate | system root | undici `Agent` `connect.ca` |
| `clientCertificateFile(cert, key)` | mTLS client certificate | off | undici `Agent` `connect.cert/key` |
| `followRedirects(max)` | Redirect tracking (**5** if no argument given) | off | **wrapper redirect loop** |
| `retry(attempts)` | Transport failure retry (1+n total attempts) | off | **wrapper retry loop** |
| `cookies()` | Enable the cookie jar | off | **wrapper cookie jar** |
| `proxy(url)` / `proxyBasicAuth(u, p)` | HTTP proxy | off | undici `ProxyAgent` |
| `compression()` | Transparent gzip/deflate decoding | off | **wrapper decoding** |

## Native Delegation Vs. Wrapper Implementation

undici's `request` does no auto-redirect, auto-decompress, or cookie handling. So the wrapper
directly performs the redirect loop, cookie jar, and compression decoding, keeping the semantics
identical across the 4 languages. The connection pool, proxy, and TLS are delegated to the undici
dispatcher.

## Client Reuse

The client wraps a single internal dispatcher and shares the connection pool. **Create it once and
reuse it**, cleaning up with `close()` when done.

## Per-Request Timeout Override

```ts
await client.get('/slow-report').timeout(30000).submitRaw();
```

[Next: Making Requests →](04-making-requests.en.md)
