[← Table Of Contents](README.en.md)

# 3. Client Configuration

`zlinkHttpClient(baseUrl) { ... }`'s DSL block gathers client-wide settings. Inside the block, fluent
builder methods are called directly.

## Builder Options

[Common Spec Chapter 2](../../../common/spec/http-client/02-client-builder.en.md) is authoritative
for defaults.

| Option | Effect | Default |
| --- | --- | --- |
| `timeout(Duration)` | Timeout per attempt (overridable per request) | **3000ms** |
| `defaultHeader(name, value)` | Default header attached to every request | none |
| `basicAuth(user, pass)` / `bearerToken(token)` | `Authorization` header | off |
| `maxResponseBodySize(bytes)` | Response body cap (decoded basis) | **16 MiB** |
| `trustCertificateFile(path)` | Add a trusted certificate | system root |
| `clientCertificateFile(cert, key)` | mTLS client certificate | off |
| `followRedirects(max)` | Redirect tracking (**5** if no argument given) | off |
| `retry(attempts)` | Transport failure retry (1+n total attempts) | off |
| `cookies()` | Enable the cookie jar | off |
| `proxy(url)` / `proxyBasicAuth(user, pass)` | HTTP proxy | off |
| `compression()` | Transparent gzip/deflate decoding | off |

```kotlin
val client = zlinkHttpClient("https://game-api.example.internal") {
    timeout(Duration.ofSeconds(5))
    bearerToken("eyJhbGci...")
    compression()
}
```

## Client Reuse

The client shares one connection pool. **Create it once and reuse it**, cleaning up with
`use { ... }` or `close()`. A long-lived client is kept for the application's lifetime, not
re-created per request.

```kotlin
class GameApi(baseUrl: String) : AutoCloseable {
    private val client = zlinkHttpClient(baseUrl)
    suspend fun profile(id: Long) = client.get("/players/$id").fetch<PlayerProfile>()
    override fun close() = client.close()
}
```

## Per-Request Timeout Override

```kotlin
client.get("/slow-report").timeout(Duration.ofSeconds(30)).awaitRaw()
```

[Next: Making Requests →](04-making-requests.en.md)
