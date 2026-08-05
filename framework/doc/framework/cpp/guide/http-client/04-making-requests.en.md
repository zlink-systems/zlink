[← Table Of Contents](README.en.md)

# 4. Making Requests

A method call like `client.get(path)` returns a `request_builder_t`, onto which you chain
query/headers/body, then send it with `submit`/`fetch`/`download`.

## HTTP Methods

```cpp
client.get ("/games/g-20260611-0042");
client.post ("/games");
client.put ("/games/g-20260611-0042/settings");
client.patch ("/games/g-20260611-0042");          // partial update
client.delete_ ("/games/g-20260611-0042");        // delete is a keyword, hence delete_
client.head ("/replays/r-99182.bin");             // headers only (no body)
client.options ("/games");                        // query the allowed methods
```

A `HEAD` response has an empty body — only status and headers come back.

```cpp
auto head = client.head ("/replays/r-99182.bin").submit_raw ().result ();
if (head && head.value ().status == 200) {
    const auto size = head.value ().headers.at ("content-length");
}
```

The path must start with `/`, or it throws `request_protocol_error`.

## Query Parameters

`query(name, value)` handles percent-encoding automatically. Use this instead of assembling the
string into the path directly.

```cpp
auto open_games = client.get ("/games")
                    .query ("status", "open")
                    .query ("mode", "ranked 2v2")     // space → %20
                    .query ("region", "kr&jp")        // & → %26
                    .fetch<game_list_t> ();
// actual target: /games?status=open&mode=ranked%202v2&region=kr%26jp
```

If the path already has a `?`, subsequent parameters are appended with `&`.

## Headers

The client-level `default_header` (every request) and the request-level `header` (this request
only) are combined. For the same name, the request side wins.

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal")
                .default_header ("x-service-name", "matchmaker")
                .build ();

client.post ("/games")
  .header ("x-idempotency-key", "9f2c1a77-58be-4d10-8d7e-3b1f0a44c2e9")
  .body (create_game_http_req_t{.game_name = "ranked-match-0611"})
  .submit<create_game_http_res_t> ();
```

## Per-Request Timeout

You can override the client's default timeout for a specific request. Use this for long-running
operations (report generation, etc.) or a health probe that should give up quickly.

```cpp
// The client default is 3 seconds; report generation alone allows 30
auto report = client.post ("/reports/season-2026q2")
                .timeout (std::chrono::seconds (30))
                .submit_raw ()
                .result ();

// The health probe fails if there's no answer within 200ms
auto ready = client.get ("/ready")
               .timeout (std::chrono::milliseconds (200))
               .submit_raw ()
               .result ();
```

A timeout exceeded is reported as `framework_error_kind_t::timeout` (retriable) —
[13. Error Handling](13-error-handling.en.md).

[Next: Request Body →](05-request-body.en.md)
