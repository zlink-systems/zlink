[← Table Of Contents](README.en.md)

# 8. Streaming

## Streaming Download

`awaitDownload(sink)` delivers the response body to the sink chunk by chunk, without buffering it.
The returned response holds the status and headers, with an empty body. Chunks are delivered as
received, and **content-encoding decoding is not applied** (regardless of the compression option).

```kotlin
Files.newOutputStream(Path.of("report.bin")).use { file ->
    val response = client.get("/reports/large").awaitDownload { chunk -> file.write(chunk) }
}
```

- The sink has type `(ByteArray) -> Unit` and is called on the transport thread reading the body.
- The accumulated size is limited by `maxResponseBodySize`.

## Streaming Upload

`bodyStream(provider, contentType)` sends the request body chunk by chunk with chunked
transfer-encoding. The body ends when the provider returns `null`.

```kotlin
client.post("/upload-stream")
    .bodyStream({ nextChunk() }, "application/octet-stream")
    .awaitRaw()
```

A streaming upload's provider can't be rewound, so it's **excluded from automatic retry**
([Chapter 10](10-redirects-retries-cookies.en.md)).

[Next: Authentication And TLS →](09-authentication-tls.en.md)
