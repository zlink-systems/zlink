---
title: "Response Rules · C#/.NET"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/http-client/10-response-rules.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Response Rules

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Redirect, Retry, and Cookies](09-redirect-retry-cookie.en.md) | [Next: Error Handling](11-error-handling.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/http-client/10-response-rules.en.md) · **C#/.NET** · [Java](../../../java/guide/http-client/10-response-rules.en.md) · [Kotlin](../../../kotlin/guide/http-client/10-response-rules.en.md) · [Node/TypeScript](../../../node/guide/http-client/10-response-rules.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can distinguish status handling by response shape, JSON decoding and body limits, and the boundary of decompression.

The processing path changes according to what a caller needs from the same HTTP response. A typed response is a JSON body decoded into an application type; a raw response preserves status, headers, and the original body. Use raw when an error payload or headers need direct handling.

## 1. Response paths by status

| Response shape | 2xx/3xx | 4xx/5xx | Body |
|---|---|---|---|
| Raw response | Returns successfully | Returns successfully | Provides the original body |
| Typed response | Returns successfully | `InternalFailure` | Provides the JSON-decoded value |
| Body-only response | Returns successfully | `InternalFailure` | Provides only the decoded body |
| Download sink | Returns successfully | Returns like raw | Delivers only final-response bytes to the sink |

Typed and body-only responses do not expose a body when status is 400 or higher. Select raw when an error payload is required. HEAD and 204 have an empty successful body; on typed paths this becomes the language's absent value.

## 2. JSON decoding failures and body limits

When a typed response cannot decode its JSON body into the target type, it fails with `ProtocolError`. This is independent of HTTP status: even a successful status cannot produce a typed result when the response shape is wrong.

The default limit for an ordinary response body is 16 MiB. Exceeding it while accumulating the body produces `Rejected`. A download sink has the same accumulated-size limit, but receives raw bytes and does not apply the decompression in the next section.

## 3. Reading a compressed response

Compression is off by default. The tutorial enables it and confirms that a typed response no longer exposes the `content-encoding` header.

```csharp
--8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-compressed-response"
```

```console
compressed response: 200 encoding-removed True
```

Enabling compression adds `Accept-Encoding: gzip, deflate`. `gzip` and `deflate` bodies are transparently decompressed, and `content-encoding` plus related length headers are removed. Typed and raw responses therefore read the body as if it had not been compressed. The 16 MiB limit applies after decompression, so a small compressed response that expands too far ends with `Rejected`. A damaged compressed body produces `ProtocolError`.

**A download sink does not decompress.** The sink receives exactly the compressed bytes sent by the server. Select a normal response instead when decompressed content is required.

## 4. When to select the raw body

A raw response preserves status, headers, and body, so it suits direct handling of an API error payload, content type, or a status that should not follow redirects. When JSON has a stable shape and only a successful body is needed, a typed or body-only response performs decoding and status checking together.

## 5. Common problems

| Symptom | Cause |
|---|---|
| A 404 or 500 payload is unavailable from a typed response. | Typed and body-only paths turn status 400 or higher into `InternalFailure`. |
| A typed call fails despite a successful status. | The JSON body could not decode into the target type, producing `ProtocolError`. |
| A small gzip response exceeds the body limit. | The limit applies to the decompressed body size. |
| A downloaded file looks like gzip bytes. | A download sink does not decompress. |

## 6. Next

- Downloading or uploading an uncompressed stream — [Streaming](07-streaming.en.md)
- Failure kinds and deciding on a new operation — [Error Handling](11-error-handling.en.md)
