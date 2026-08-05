[← Table Of Contents](README.en.md)

# 12. Compression

Turning on `compression()` attaches `Accept-Encoding: gzip, deflate` to the request and
transparently decodes the response if it's encoded as `gzip` or `deflate`.

```kotlin
val report = zlinkHttpClient("https://api.internal") {
    compression()
}.use { client ->
    client.get("/large-report").fetch<Report>()
}
```

## Decoding Rules

- Handles **both gzip and deflate** (deflate detects both zlib-wrap and raw).
- **Removes** the `content-encoding` header after decoding.
- Enforces `maxResponseBodySize` against the **decoded size**.
- `awaitDownload(sink)` streaming chunks are **not decoded** (delivered as received).

If the body is corrupted, it's reported as a decode-failure exception; if the decoded size exceeds
the limit, as a request-failure exception.

[Next: Error Handling →](13-error-handling.en.md)
