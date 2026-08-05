[← 목차](README.ko.md)

# 10. Redirect · Retry · Cookie

세 기능 모두 **opt-in**이다. 켜지 않으면 redirect 응답은 그대로 반환되고,
실패는 재시도 없이 보고되며 cookie는 무시된다.

## Redirect 자동 추적

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal")
                .follow_redirects ()        // 기본 한도 5회
                .build ();

// 302 → Location 추적 → 최종 200 응답이 반환된다
auto game = client.get ("/games/latest").fetch<game_t> ();
```

의미론은 브라우저/일반 client 관행을 따른다.

| status | method/body 처리 |
|--------|------------------|
| `301`, `302` | `POST`는 `GET`으로 바뀌고 body를 버림. 그 외 method는 보존 |
| `303` | 항상 `GET`으로 바뀌고 body를 버림 |
| `307`, `308` | method와 body 모두 보존 |

- Location은 절대 URL(`https://other-host/...`)과 절대 경로(`/games/42`)를
  지원한다. 상대 경로(`../x`)는 지원하지 않으며 `request_failed`로 닫힌다.
- 절대 URL이 최초 요청과 다른 origin을 가리키면 `Authorization` 헤더를 다시 보내지
  않는다. 같은 origin 안의 redirect에는 인증 헤더를 유지한다. 다른 이름의 비밀 헤더는
  일반 헤더와 구분할 수 없으므로 `default_header`나 요청 단위 `header`에 넣지 않는다.
- 한도(`follow_redirects(max)`)를 넘으면 `request_failed`
  ("exceeded the redirect limit")로 닫힌다.
- 끈 상태(기본)에서는 3xx 응답이 그대로 반환되므로 직접 분기할 수 있다.

```cpp
auto raw = client.get ("/games/latest").submit_raw ().result ();
if (raw && raw.value ().status == 302) {
    const auto &location = raw.value ().headers.at ("location");
}
```

## Retry

`retry(attempts)`는 **retriable한 transport 실패**(연결 끊김, timeout)만
재시도한다. 시도 사이 간격은 지수 백오프 + full jitter다(기본 50ms, 시도마다 2배, 상한 1초, 0~상한 무작위).

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal")
                .retry (2)                  // 최초 1회 + 재시도 2회 = 최대 3회
                .timeout (std::chrono::seconds (2))
                .build ();
```

재시도하지 **않는** 것:

- **HTTP status 실패 (4xx/5xx)** — 교환 자체는 성공했으므로. 503 같은 상태 기반
  재시도가 필요하면 호출자가 status를 보고 직접 반복한다.
- **`body_stream` 업로드와 `download`** — 되감을 수 없으므로
  ([8. Streaming](08-streaming.ko.md)).

POST처럼 멱등이 아닌 요청에 retry를 켤 때는 서버가 idempotency key 등으로 중복을
견디는지 확인한다.

```cpp
client.post ("/games")
  .header ("x-idempotency-key", request_id)
  .body (create_game_http_req_t{.game_name = "ranked-match-0611"})
  .submit<create_game_http_res_t> ();
```

## Cookie jar

`cookies()`를 켜면 in-memory cookie jar가 활성화된다. `Set-Cookie`를 저장하고
이후 요청에 `Cookie` 헤더로 싣는다. 세션 쿠키 기반 API를 호출할 때 쓴다.

```cpp
auto portal = zlink::http_client::client_t::create ("https://ops-portal.example.internal")
                .cookies ()
                .build ();

portal.post ("/login")
  .form ("username", "ops-bot")
  .form ("password", ops_password)
  .submit_raw ().result ();                  // Set-Cookie: session=... 저장

auto dashboards = portal.get ("/api/dashboards").fetch<dashboard_list_t> ();
// Cookie: session=... 이 자동으로 실린다
```

jar가 반영하는 속성과 무시하는 속성:

| 속성 | 처리 |
|------|------|
| `Path` | path prefix가 일치하는 요청에만 전송 |
| `Secure` | https 요청에만 전송 |
| `Max-Age` | 0 이하면 즉시 삭제 |
| `Domain`, `Expires`, `HttpOnly`, `SameSite` | 무시 (host 단위 저장, 영속화 없음) |

jar는 host당 최대 128개를 보관하며 초과하면 가장 오래된 것부터 제거한다.
client(runtime) 수명 동안만 유지되고 디스크 영속화는 없다.

[다음: Proxy →](11-proxy.ko.md)
