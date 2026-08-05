[← Table Of Contents](README.en.md)

# 12. Compression

Turning on `Compression()` attaches `Accept-Encoding: gzip, deflate` to the request and
transparently decodes the response if it's encoded as `gzip` or `deflate`.

```csharp
var report = await ZLinkHttpClient.Create("https://api.internal")
    .Compression()
    .Get("/large-report")
    .Fetch<Report>();
```

## Wrapper-Controlled Decoding

.NET's native `AutomaticDecompression` is turned off, and **the wrapper controls decoding**. The
reason is to align the semantics with the zlink contract:

- Handles **both gzip and deflate** (deflate detects both zlib-wrap and raw).
- **Removes** the `Content-Encoding` header after decoding.
- Enforces `MaxResponseBodySize` against the **decoded size**.
- `DownloadAsync(sink)` streaming chunks are **not decoded** (delivered as received).

If the body is corrupted, it's reported as `ProtocolError`; if the decoded size exceeds the limit,
as `CapacityExceeded`.

[Next: Error Handling →](13-error-handling.en.md)
