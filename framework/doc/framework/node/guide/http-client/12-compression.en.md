[← Table Of Contents](README.en.md)

# 12. Compression

Turning on `compression()` attaches `accept-encoding: gzip, deflate` to the request and
transparently decodes the response if it's encoded as `gzip` or `deflate`.

```ts
const response = await ZLinkHttpClient.create('https://api.internal')
  .compression()
  .get('/large-report')
  .submit<Report>();
```

## Wrapper-Controlled Decoding

undici's `request` doesn't automatically decode the response. So **the wrapper controls decoding
with `node:zlib`**. The reason is to align the semantics with the zlink contract:

- Handles **both gzip and deflate** (deflate detects both zlib-wrap and raw).
- **Removes** the `content-encoding` header after decoding.
- Enforces `maxResponseBodySize` against the **decoded size**.
- `download(sink)` streaming chunks are **not decoded** (delivered as received).

If the body is corrupted, it's reported as `payloadDecodeFailed`; if the decoded size exceeds the
limit, as `requestFailed`.

[Next: Error Handling →](13-error-handling.en.md)
