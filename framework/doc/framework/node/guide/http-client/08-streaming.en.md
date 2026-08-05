[← Table Of Contents](README.en.md)

# 8. Streaming

## Streaming Download

`download(sink)` delivers the response body to the sink chunk by chunk, without buffering it. The
returned response holds the status and headers, with an empty body. Chunks are delivered as
received, and **content-encoding decoding is not applied** (regardless of the compression option).

```ts
const file = fs.createWriteStream('report.bin');
const response = await client.get('/reports/large').download((chunk) => file.write(chunk));
console.log(response.status); // The body is empty
```

- The sink has type `(chunk: Uint8Array) => void` and is called in the asynchronous context reading
  the response.
- The accumulated size is limited by `maxResponseBodySize`.

## Streaming Upload

`bodyStream(provider, contentType)` sends the request body chunk by chunk with chunked
transfer-encoding. The body ends when the provider returns `null`.

```ts
await client.post('/upload-stream')
  .bodyStream(() => nextChunk(), 'application/octet-stream')
  .submitRaw();
```

A streaming upload's provider can't be rewound, so it's **excluded from automatic retry**
([Chapter 10](10-redirects-retries-cookies.en.md)).

[Next: Authentication And TLS →](09-authentication-tls.en.md)
