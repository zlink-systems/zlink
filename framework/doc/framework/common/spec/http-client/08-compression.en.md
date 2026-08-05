# 8. Compression

> [Common contract table of contents](README.en.md)

The `compression()` opt-in contract:

- Attaches `Accept-Encoding: gzip, deflate` to the request (not
  attached before opt-in).
- **Transparently decompresses** a `Content-Encoding: gzip | deflate`
  response, and removes the `content-encoding` (and related length)
  header from the response map after decompressing. The user sees the
  body as if there was no compression.
- deflate accepts both the zlib-wrapped and raw forms (detected by
  leading byte).
- **Also enforces `maxResponseBodySize` on the size after
  decompression** (compression-bomb defense). Exceeding it is
  `CapacityExceeded`.
- A corrupted compressed body is `ProtocolError`.
- **Decompression doesn't apply to streaming download** — the sink
  receives the raw (compressed) bytes
  ([Chapter 4 §4.4](04-response-model.en.md)).
- Language mapping: cpp Boost.Beast zlib (parses the gzip header
  itself, doesn't verify the CRC32 trailer), dotnet
  `System.IO.Compression` (native `AutomaticDecompression` turned
  off), java `java.util.zip`, node `node:zlib`.
