[← Table Of Contents](README.en.md)

# 5. Request Body

There are five body sources. **Only one per request** can be used — mixing two or more is rejected
with `request_protocol_error` ("single body source").

| Source | Method | Content-Type |
|------|--------|--------------|
| typed JSON DTO | `body(dto)` | `application/json` (automatic) |
| raw | `body(content, content_type)` | as specified |
| form | repeated `form(name, value)` | `application/x-www-form-urlencoded` (automatic) |
| multipart | `multipart(...)` / `multipart_file(...)` | `multipart/form-data; boundary=...` (automatic) |
| streaming | `body_stream(provider, content_type)` | as specified + chunked |

## Typed JSON DTO

Once you define an nlohmann ADL function (`to_json`) for a DTO, `body(dto)` serializes it as JSON.
The convention is that application code doesn't assemble `nlohmann::json` directly.

```cpp
struct create_game_http_req_t
{
    std::string game_name;
};

void to_json (nlohmann::json &json, const create_game_http_req_t &value)
{
    json = nlohmann::json{{"gameName", value.game_name}};
}

auto created = client.post ("/games")
                 .body (create_game_http_req_t{.game_name = "ranked-match-0611"})
                 .fetch<create_game_http_res_t> ();
```

## Raw Body

For a non-JSON payload, provide the content and content-type together.

```cpp
// When an external payment gateway requires XML
auto receipt = client.post ("/billing/receipts")
                 .body (R"(<receipt order="ord-77231" amount="4900" currency="KRW"/>)",
                        "application/xml")
                 .submit_raw ()
                 .result ();
```

## Form (x-www-form-urlencoded)

Accumulating `form(name, value)` builds an urlencoded body. Values are automatically
percent-encoded.

```cpp
// An API that requires a form, like an OAuth token endpoint
auto token = client.post ("/oauth/token")
               .form ("grant_type", "client_credentials")
               .form ("client_id", "matchmaker-svc")
               .form ("client_secret", service_secret)
               .submit_raw ()
               .result ();
```

## Multipart/Form-Data

Text fields go on with `multipart`, and files with `multipart_file`. The boundary is generated
automatically.

```cpp
auto uploaded = client.post ("/players/7281/avatar")
                  .multipart ("visibility", "public")
                  .multipart_file ("file", "avatar.png", png_bytes, "image/png")
                  .submit_raw ()
                  .result ();
```

## Streaming Upload (Chunked)

For large transfers where the whole body can't fit in memory, use `body_stream`. The provider keeps
returning chunks until it returns `std::nullopt`, which ends it. It's sent with chunked
transfer-encoding.

```cpp
std::ifstream replay ("/var/games/replays/r-99182.bin", std::ios::binary);

auto result = client.post ("/replays")
                .body_stream (
                  [&replay] () -> std::optional<std::string> {
                      std::string chunk (64 * 1024, '\0');
                      replay.read (chunk.data (), chunk.size ());
                      chunk.resize (static_cast<std::size_t> (replay.gcount ()));
                      if (chunk.empty ()) {
                          return std::nullopt;   // end of transfer
                      }
                      return chunk;
                  },
                  "application/octet-stream")
                .submit_raw ()
                .result ();
```

**Constraint** — since the provider can't be rewound once consumed:

- A streaming upload always goes over a fresh connection, bypassing the connection pool.
- It's excluded from [automatic retry](10-redirects-retries-cookies.en.md). If a retry is needed,
  the caller builds a new provider and calls it again.

[Next: Handling Responses →](06-handling-responses.en.md)
