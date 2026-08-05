[← Table Of Contents](README.en.md)

# 12. Compression

Turning on `compression()` carries `Accept-Encoding: gzip, deflate` on the request and
**transparently decodes** a response body the server sends compressed. The calling code doesn't know
whether it was compressed.

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal")
                .compression ()
                .build ();

// Even if the server sends gzip, the body is decoded to plain JSON
auto board = client.get ("/leaderboard")
               .query ("season", "2026q2")
               .fetch<leaderboard_t> ();
```

After decoding, the `Content-Encoding` header is removed from the response, so the response the
caller sees is the same as if it had been plain text from the start.

## Supported Encodings

| Content-Encoding | Handling |
|------------------|------|
| `gzip` | decoded (gzip header parsing + DEFLATE) |
| `deflate` | decoded (auto-detects a zlib wrapper, with a raw DEFLATE fallback) |
| others (`br`, etc.) | not decoded — returned as the original text |

The implementation uses Boost.Beast's built-in zlib, so there's no external zlib dependency.

## Constraints

- **Trailer checksum (CRC32) not verified** — the integrity check of the gzip/zlib trailer isn't
  done. The premise is that TCP/TLS guarantees transport integrity.
- **Not applied to `download(sink)`** — streaming download delivers raw bytes as-is
  ([8. Streaming](08-streaming.en.md)). For a compressed large file, the caller receives it and
  decodes it.
- A corrupted compressed body closes with `payload_decode_failed`
  ([13. Error Handling](13-error-handling.en.md)).

[Next: Error Handling →](13-error-handling.en.md)
