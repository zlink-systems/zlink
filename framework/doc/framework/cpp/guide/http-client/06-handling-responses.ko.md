[← 목차](README.ko.md)

# 6. Response 다루기

응답을 받는 방법은 세 가지이고 풀어 주는 깊이가 다르다.

| 방법 | 반환 | 실패 보고 | 용도 |
|------|------|-----------|------|
| `submit_raw()` | `task_t<raw_http_response_t>` | `result_t` | 비-JSON, status 직접 분기 |
| `submit<T>()` | `task_t<http_response_t<T>>` | `result_t` 또는 `co_await` 예외 | typed JSON + status/헤더 접근 |
| `fetch<T>()` | `T` (DTO 직접) | 예외 | 테스트, client 시나리오 |

## 3겹 구조 이해하기

`submit<T>().result()`가 돌려주는 타입은 DTO가 아니라 3겹으로 감싸져 있다.

```text
result_t< http_response_t< T > >
   │           │             └─ 실제 DTO            ... .value().body
   │           └─ HTTP 봉투: status/headers/body    ... .value()
   └─ 성공/실패 래퍼: operator bool, error()        ... result
```

```cpp
auto result = client.get ("/players/7281").submit<player_profile_t> ().result ();

if (!result) {                                   // ① transport/디코딩/4xx·5xx 실패
    log_error (result.error ()->what ());
    return;
}
const auto &response = result.value ();          // ② HTTP 봉투
assert (response.status == 200);
const auto &etag = response.headers.at ("etag");
const auto &profile = response.body;             // ③ DTO
```

`http_response_t<T>`의 필드:

| 필드 | 타입 | 내용 |
|------|------|------|
| `status` | `int` | HTTP status code |
| `headers` | `std::map<std::string, std::string>` | 응답 헤더 |
| `body` | `T` | JSON 디코딩된 DTO |
| `raw_body` | `std::string` | 디코딩 전 원문 (디버깅용) |

## status 코드는 어떻게 처리되나

`submit<T>()`는 **4xx/5xx를 실패로 취급**한다 — `result_t`가
`request_failed`("HTTP request failed with status 404")로 닫히고 `value()`에
접근할 수 없다. typed 경로는 "성공 응답을 DTO로 받는" 경로이기 때문이다.

status를 직접 분기하고 싶으면 `submit_raw()`를 쓴다. raw 경로는 status가 몇이든
응답 자체를 성공으로 돌려준다.

```cpp
auto raw = client.get ("/games/g-20260611-0042").submit_raw ().result ();
if (!raw) {                                  // transport 실패만 여기로
    return retry_later (raw.error ());
}
switch (raw.value ().status) {
    case 200: return parse_game (raw.value ().body);
    case 404: return game_not_found ();
    case 409: return resolve_conflict (raw.value ().body);
    default:  return unexpected_status (raw.value ().status);
}
```

## DTO 디코딩

`from_json` ADL 함수가 있으면 `submit<T>`/`fetch<T>`가 응답 body를 그 타입으로
디코딩한다. 디코딩 실패는 `payload_decode_failed`로 보고된다.

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

## fetch: 전부 풀어서 받기

`fetch<T>()`는 result와 봉투를 모두 풀어 DTO만 돌려주고 모든 실패(transport,
4xx/5xx, 디코딩)를 `framework_exception_t`로 던진다. blocking이므로
테스트·client 시나리오 전용이다 — [7장](07-async-coroutines.ko.md)의 규칙 참고.

```cpp
auto created = client.post ("/games")
                 .body (create_game_http_req_t{.game_name = "ranked-match-0611"})
                 .fetch<create_game_http_res_t> ();
join_game (created.room_id, created.owner_play_endpoint);
```

[다음: 비동기와 코루틴 →](07-async-coroutines.ko.md)
