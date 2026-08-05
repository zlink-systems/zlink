[← Table Of Contents](README.en.md)

# 12. Compression

Turning on `compression()` attaches `Accept-Encoding: gzip, deflate` to the request and
transparently decodes the response if it's encoded as `gzip` or `deflate`.

```java
HttpResponse<Report> response = ZLinkHttpClient.create("https://api.internal")
    .compression()
    .get("/large-report")
    .submit(Report.class)
    .toCompletableFuture().join();
```

## Wrapper-Controlled Decoding

`java.net.http` doesn't automatically decode the response. So **the wrapper controls decoding with
`java.util.zip`**. The reason is to align the semantics with the zlink contract:

- Handles **both gzip and deflate** (deflate detects both zlib-wrap and raw).
- **Removes** the `content-encoding` header after decoding.
- Enforces `maxResponseBodySize` against the **decoded size**.
- `download(sink)` streaming chunks are **not decoded** (delivered as received).

If the body is corrupted, it's reported as a decode-failure exception; if the decoded size exceeds
the limit, as a request-failure exception.

[Next: Error Handling →](13-error-handling.en.md)
