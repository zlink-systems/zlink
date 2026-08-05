[← 목차](README.ko.md)

# 4. Request 만들기

`client.get(path)` 같은 메서드 호출이 `request_builder_t`를 돌려주고 거기에
query·헤더·body를 체인으로 얹은 뒤 `submit`/`fetch`/`download`로 보낸다.

## HTTP 메서드

```cpp
client.get ("/games/g-20260611-0042");
client.post ("/games");
client.put ("/games/g-20260611-0042/settings");
client.patch ("/games/g-20260611-0042");          // 부분 수정
client.delete_ ("/games/g-20260611-0042");        // delete는 키워드라 delete_
client.head ("/replays/r-99182.bin");             // 헤더만 (body 없음)
client.options ("/games");                        // 허용 메서드 조회
```

`HEAD` 응답은 body가 비어 있고 status와 헤더만 온다.

```cpp
auto head = client.head ("/replays/r-99182.bin").submit_raw ().result ();
if (head && head.value ().status == 200) {
    const auto size = head.value ().headers.at ("content-length");
}
```

path는 반드시 `/`로 시작해야 하며 아니면 `request_protocol_error`로 던진다.

## Query 파라미터

`query(name, value)`는 percent-encoding을 자동 처리한다. path에 직접 문자열을
조립하지 말고 이쪽을 쓴다.

```cpp
auto open_games = client.get ("/games")
                    .query ("status", "open")
                    .query ("mode", "ranked 2v2")     // 공백 → %20
                    .query ("region", "kr&jp")        // & → %26
                    .fetch<game_list_t> ();
// 실제 target: /games?status=open&mode=ranked%202v2&region=kr%26jp
```

path에 이미 `?`가 있으면 `&`로 이어 붙는다.

## 헤더

client 수준 `default_header`(모든 요청)와 request 수준 `header`(이 요청만)를
조합한다. 같은 이름이면 request 쪽이 이긴다.

```cpp
auto client = zlink::http_client::client_t::create ("https://game-api.example.internal")
                .default_header ("x-service-name", "matchmaker")
                .build ();

client.post ("/games")
  .header ("x-idempotency-key", "9f2c1a77-58be-4d10-8d7e-3b1f0a44c2e9")
  .body (create_game_http_req_t{.game_name = "ranked-match-0611"})
  .submit<create_game_http_res_t> ();
```

## Request 단위 timeout

client 기본 timeout을 특정 요청에서만 바꿀 수 있다. 긴 작업(리포트 생성 등)이나
빨리 포기해야 하는 health probe에 쓴다.

```cpp
// client 기본은 3초, 리포트 생성만 30초 허용
auto report = client.post ("/reports/season-2026q2")
                .timeout (std::chrono::seconds (30))
                .submit_raw ()
                .result ();

// health probe는 200ms 안에 답이 없으면 실패 처리
auto ready = client.get ("/ready")
               .timeout (std::chrono::milliseconds (200))
               .submit_raw ()
               .result ();
```

timeout 초과는 `framework_error_kind_t::timeout`(retriable)으로 보고된다 —
[13. 에러 처리](13-error-handling.ko.md).

[다음: Request Body →](05-request-body.ko.md)
