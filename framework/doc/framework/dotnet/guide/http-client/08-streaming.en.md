[← Table Of Contents](README.en.md)

# 8. Streaming

## Streaming Download

`DownloadAsync(sink)` delivers the response body to the sink chunk by chunk, without buffering it.
The returned response holds the status and headers, with an empty body. Chunks are delivered as
received, and **content-encoding decoding is not applied** (regardless of the compression option).

```csharp
await using var file = File.Create("report.bin");
var response = await client.Get("/reports/large")
    .DownloadAsync(chunk => file.Write(chunk.Span));

Console.WriteLine(response.Status); // The body is empty
```

- The sink is called in the asynchronous context reading the response. Don't block the thread with
  heavy synchronous work ([Chapter 7](07-async.en.md)).
- The accumulated size is limited by `MaxResponseBodySize`.

## Streaming Upload

`BodyStream(provider, contentType)` sends the request body chunk by chunk with chunked
transfer-encoding. The body ends when the provider returns `null`.

```csharp
await client.Post("/upload-stream")
    .BodyStream(() => NextChunk(), "application/octet-stream")
    .AsyncRaw();
```

A streaming upload's provider can't be rewound, so it's **excluded from automatic retry**
([Chapter 10](10-redirects-retries-cookies.en.md)).

[Next: Authentication And TLS →](09-authentication-tls.en.md)
