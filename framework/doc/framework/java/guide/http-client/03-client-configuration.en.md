[← Table Of Contents](README.en.md)

# 3. Client Configuration

The builder gathers client-wide settings. They either map to `java.net.http` settings, or, when the
semantics differ, are handled directly in the wrapper.

## Builder Options

[Common Spec Chapter 2](../../../common/spec/http-client/02-client-builder.en.md) is authoritative
for defaults.

| Option | Effect | Default | Implementation |
| --- | --- | --- | --- |
| `baseUrl(url)` | Base URL for every request | none (required) | wrapper |
| `timeout(Duration)` | Timeout per attempt (overridable per request) | **3000ms** | `HttpRequest.timeout` |
| `defaultHeader(n, v)` | Default header attached to every request | none | wrapper |
| `basicAuth(u, p)` / `bearerToken(t)` | `Authorization` header | off | wrapper |
| `maxResponseBodySize(bytes)` | Response body cap (decoded basis) | **16 MiB** | wrapper |
| `trustCertificateFile(path)` | Add a trusted certificate | system root | `SSLContext` TrustManager |
| `clientCertificateFile(cert, key)` | mTLS client certificate | off | `SSLContext` KeyManager |
| `followRedirects(max)` | Redirect tracking (**5** if no argument given) | off | **wrapper redirect loop** |
| `retry(attempts)` | Transport failure retry (1+n total attempts) | off | **wrapper retry loop** |
| `cookies()` | Enable the cookie jar | off | **wrapper cookie jar** |
| `proxy(url)` / `proxyBasicAuth(u, p)` | HTTP proxy | off | `ProxySelector` |
| `compression()` | Transparent gzip/deflate decoding | off | **wrapper decoding** |

## Native Delegation Vs. Wrapper Implementation

`HttpClient` is kept at `Redirect.NEVER` (wrapper loop), and no cookie manager is used (wrapper
jar). `java.net.http` doesn't automatically decode the response, so the wrapper controls it with
`java.util.zip`. The connection pool, proxy, and TLS are delegated to the native layer. The
`Redirect` enum has no notion of a count, so the redirect loop is implemented in the wrapper.

## Client Reuse

The client wraps a single internal `HttpClient` and shares the connection pool. **Create it once and
reuse it**, cleaning up with try-with-resources or `close()`.

## Per-Request Timeout Override

```java
client.get("/slow-report").timeout(Duration.ofSeconds(30)).submitRaw();
```

[Next: Making Requests →](04-making-requests.en.md)
