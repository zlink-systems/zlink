[← Table Of Contents](README.en.md)

# 8. Streaming

Use the streaming path when the response or request body is too large to fully hold in memory.

## Download: download(sink)

`download(sink)` delivers the response body to the sink chunk by chunk as it arrives, without
buffering it. The returned response has only status and headers, with an empty body. However, the
total body bytes still can't exceed the client's `max_response_body_size` cap. A client downloading
large files needs to explicitly raise this value.

```cpp
// Stream a multi-gigabyte replay file to disk
std::ofstream out ("/var/games/replays/r-99182.bin", std::ios::binary);

auto result = client.get ("/replays/r-99182.bin")
                .timeout (std::chrono::minutes (5))
                .download ([&out] (std::string_view chunk) {
                    out.write (chunk.data (),
                               static_cast<std::streamsize> (chunk.size ()));
                })
                .result ();

if (!result) {
    std::filesystem::remove ("/var/games/replays/r-99182.bin");   // clean up the partial file
    throw std::runtime_error (result.error ()->what ());
}
assert (result.value ().status == 200);
assert (result.value ().body.empty ());   // the body went to the sink
```

If you need progress, compute it from the `Content-Length` header and the accumulated bytes.

```cpp
std::uint64_t received = 0;
auto result = client.get ("/replays/r-99182.bin")
                .download ([&] (std::string_view chunk) {
                    received += chunk.size ();
                    progress_bar.update (received);
                    out.write (chunk.data (), chunk.size ());
                })
                .result ();
```

### Interaction With Redirect

When downloading on a client with `follow_redirects()` on, **an intermediate redirect response's
body doesn't leak into the sink** — only the final response's body is delivered.

## Upload: body_stream

A large request body is sent chunked via [Chapter 5's body_stream](05-request-body.en.md). The
provider supplies chunks and ends with `std::nullopt`.

## Constraints Of The Streaming Path

Because it's a data flow that can't be rewound, unlike the normal path, the following don't apply.

| Feature | On streaming |
|------|---------------|
| Automatic retry (`retry(n)`) | **excluded** — the sink could get duplicate chunks, or the provider isn't replayable |
| Connection pool reuse | `body_stream` always uses a fresh connection (download does reuse it) |
| Decompression (`compression()`) | The download sink receives **raw bytes as-is** (no decoding) |

If a retry is needed, the caller prepares a fresh sink/provider on failure and calls again.

```cpp
for (int attempt = 0; attempt < 3; ++attempt) {
    std::ofstream out (path, std::ios::binary | std::ios::trunc);   // fresh each attempt
    auto result = client.get ("/replays/r-99182.bin")
                    .download ([&out] (std::string_view c) { out.write (c.data (), c.size ()); })
                    .result ();
    if (result || !result.error ()->is_retriable ()) {
        break;
    }
}
```

[Next: Authentication And TLS →](09-authentication-tls.en.md)
