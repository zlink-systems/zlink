[← Table Of Contents](README.en.md)

# 6. Handling Responses

There are three ways to receive a response, unwrapped to different depths.

| Method | Returns | Failure Reporting | Use |
|------|------|-----------|------|
| `submit_raw()` | `task_t<raw_http_response_t>` | `result_t` | non-JSON, direct status branching |
| `submit<T>()` | `task_t<http_response_t<T>>` | `result_t` or a `co_await` exception | typed JSON + status/header access |
| `fetch<T>()` | `T` (the DTO directly) | exception | tests, client scenarios |

## Understanding The 3-Layer Structure

The type `submit<T>().result()` returns isn't a DTO — it's wrapped in 3 layers.

```text
result_t< http_response_t< T > >
   │           │             └─ the actual DTO        ... .value().body
   │           └─ the HTTP envelope: status/headers/body ... .value()
   └─ the success/failure wrapper: operator bool, error() ... result
```

```cpp
auto result = client.get ("/players/7281").submit<player_profile_t> ().result ();

if (!result) {                                   // ① transport/decode/4xx·5xx failure
    log_error (result.error ()->what ());
    return;
}
const auto &response = result.value ();          // ② the HTTP envelope
assert (response.status == 200);
const auto &etag = response.headers.at ("etag");
const auto &profile = response.body;             // ③ the DTO
```

`http_response_t<T>`'s fields:

| Field | Type | Content |
|------|------|------|
| `status` | `int` | HTTP status code |
| `headers` | `std::map<std::string, std::string>` | response headers |
| `body` | `T` | the JSON-decoded DTO |
| `raw_body` | `std::string` | the pre-decode original text (for debugging) |

## How Status Codes Are Handled

`submit<T>()` **treats 4xx/5xx as a failure** — the `result_t` closes as `request_failed`
("HTTP request failed with status 404"), and `value()` can't be accessed. That's because the typed
path is meant for "receiving a successful response as a DTO."

If you want to branch on status directly, use `submit_raw()`. The raw path returns the response
itself as a success regardless of the status.

```cpp
auto raw = client.get ("/games/g-20260611-0042").submit_raw ().result ();
if (!raw) {                                  // only a transport failure lands here
    return retry_later (raw.error ());
}
switch (raw.value ().status) {
    case 200: return parse_game (raw.value ().body);
    case 404: return game_not_found ();
    case 409: return resolve_conflict (raw.value ().body);
    default:  return unexpected_status (raw.value ().status);
}
```

## DTO Decoding

If a `from_json` ADL function exists, `submit<T>`/`fetch<T>` decodes the response body into that
type. A decode failure is reported as `payload_decode_failed`.

```cpp
struct create_game_http_res_t
{
    std::string room_id;
    std::string game_name;
    std::string owner_play_endpoint;
    std::vector<std::string> play_endpoints;
    std::vector<play_node_info_t> play_nodes;
    int required_level = 0;
};

void from_json (const nlohmann::json &json, create_game_http_res_t &value)
{
    value.room_id = json.at ("roomId").get<std::string> ();
    value.game_name = json.at ("gameName").get<std::string> ();
    value.owner_play_endpoint = json.at ("ownerPlayEndpoint").get<std::string> ();
    value.play_endpoints = json.at ("playEndpoints").get<std::vector<std::string>> ();
    value.play_nodes = json.at ("playNodes").get<std::vector<play_node_info_t>> ();
    value.required_level = json.at ("requiredLevel").get<int> ();
}
```

## fetch: Getting It Fully Unwrapped

`fetch<T>()` unwraps both the result and the envelope, returning only the DTO, and throws every
failure (transport, 4xx/5xx, decoding) as `framework_exception_t`. Since it's blocking, it's for
test/client-scenario use only — see the rules in [Chapter 7](07-async-coroutines.en.md).

```cpp
auto created = client.post ("/games")
                 .body (create_game_http_req_t{.game_name = "ranked-match-0611"})
                 .fetch<create_game_http_res_t> ();
join_game (created.room_id, created.owner_play_endpoint);
```

[Next: Async And Coroutines →](07-async-coroutines.en.md)
