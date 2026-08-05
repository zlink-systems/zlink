# 2. Client Builder Contract

> [Common contract table of contents](README.en.md)

## 2.1 Entry Point

- Static factory `create()` / `create(baseUrl)` → client builder →
  `build()` → client.
- kotlin provides a top-level DSL `zlinkHttpClient(baseUrl) { ... }` as
  a language deviation (the same builder internally).
- The client builder also has a verb shorthand (7 kinds, such as
  `get(path)`). This path is **one-shot**: it lazy-builds the client at
  submit time and closes it after completion. Since there's no
  connection pool reuse, it isn't for repeated calls
  ([Chapter 5](05-execution-model.en.md) §5.4).

## 2.2 Option And Default Value Table (Canonical)

Every language provides all the options below, and the listed default
is the 5-language common contract. Name casing follows language
convention (see
[Per-Language Interface Definition](language-interfaces.en.md)).

| Option (Common Concept Name) | Argument | Default | Meaning |
| --- | --- | --- | --- |
| `baseUrl` | URL string | None (required: specify as create argument or option) | The base URL for every request path |
| `timeout` | Time | **3000ms** | Timeout per attempt. Can be overridden per request |
| `defaultHeader` | name, value | None (accumulates) | Default header attached to every request. A per-request header wins |
| `basicAuth` | user, password | off | `Authorization: Basic` |
| `bearerToken` | token | off | `Authorization: Bearer` |
| `maxResponseBodySize` | bytes | **16 MiB** | Response body bound. Also applies to the size after decompression. Exceeding it is `CapacityExceeded` |
| `trustCertificateFile` | PEM path | System root | **Adds** a trust certificate (doesn't replace the default root) |
| `clientCertificateFile` | cert path, key path | off | mTLS client certificate (PEM) |
| `followRedirects` | max (**5** with no argument) | **off** | Automatic redirect follow bound ([Chapter 6](06-redirect-retry-cookie.en.md)) |
| `retry` | attempts | **off (0)** | Additional retry count. Total attempts = 1 + attempts |
| `cookies` | None | **off** | Enables the cookie jar |
| `proxy` | URL (`http://` only) | off | HTTP proxy ([Chapter 7](07-auth-tls-proxy.en.md)) |
| `proxyBasicAuth` | user, password | off | Proxy auth |
| `compression` | None | **off** | `Accept-Encoding: gzip, deflate` + transparent decompression ([Chapter 8](08-compression.en.md)) |

Validation rule: `timeout`/`maxResponseBodySize`/`followRedirects`/`retry`
must be positive, `proxy` must have an `http://` prefix, `baseUrl` must
start with `http://` or `https://`, and `build()` with none specified is
`ProtocolError`. A validation failure throws immediately at
builder-call time (eager).

## 2.3 Language Deviation (Recognized)

| Language | Deviation |
| --- | --- |
| cpp | `coroutines()` / `coroutines(resume)` / `coroutines(execute, resume)` — the execution model switch ([Chapter 5](05-execution-model.en.md)). Other languages have no such concept since the runtime is always async |
| dotnet | `Codecs(Action<IZLinkCodecRegistryBuilder>)` — framework codec extension registration. Other languages fix JSON (not a §10 R-item — dotnet is the only extension point fixed in spec) |
| node | `timeout` is an integer ms (other languages use a time type: `std::chrono`, `TimeSpan`, `Duration`) |
| kotlin | A DSL block instead of a builder. No `build()` call |

## 2.4 Client Lifetime Rule

- **Build one client per service and reuse it.** Creating/building per
  request loses the connection pool and keep-alive benefit.
- The client is closed explicitly: `close()` / `Dispose()` /
  `AutoCloseable` / a language-idiomatic resource construct (`use`,
  try-with-resources).
