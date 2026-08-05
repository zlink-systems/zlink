[← 목차](README.ko.md)

# 5. Request Body

body 소스는 다섯 가지다. **한 request에 하나만** 쓸 수 있고 둘 이상 섞으면
`request_protocol_error`("single body source")로 거부된다.

| 소스 | 메서드 | Content-Type |
|------|--------|--------------|
| typed JSON DTO | `body(dto)` | `application/json` (자동) |
| raw | `body(content, content_type)` | 지정값 |
| form | `form(name, value)` 반복 | `application/x-www-form-urlencoded` (자동) |
| multipart | `multipart(...)` / `multipart_file(...)` | `multipart/form-data; boundary=...` (자동) |
| streaming | `body_stream(provider, content_type)` | 지정값 + chunked |

## typed JSON DTO

DTO에 nlohmann ADL 함수(`to_json`)를 정의해 두면 `body(dto)`가 JSON으로
직렬화한다. application 코드가 `nlohmann::json`을 직접 조립하지 않는 것이 규약이다.

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

## raw body

JSON이 아닌 payload는 내용과 content-type을 함께 준다.

```cpp
// 외부 결제 게이트웨이가 XML을 요구하는 경우
auto receipt = client.post ("/billing/receipts")
                 .body (R"(<receipt order="ord-77231" amount="4900" currency="KRW"/>)",
                        "application/xml")
                 .submit_raw ()
                 .result ();
```

## form (x-www-form-urlencoded)

`form(name, value)`를 누적하면 urlencoded body가 만들어진다. 값은 자동으로
percent-encoding된다.

```cpp
// OAuth token endpoint처럼 form을 요구하는 API
auto token = client.post ("/oauth/token")
               .form ("grant_type", "client_credentials")
               .form ("client_id", "matchmaker-svc")
               .form ("client_secret", service_secret)
               .submit_raw ()
               .result ();
```

## multipart/form-data

텍스트 필드는 `multipart`, 파일은 `multipart_file`로 얹는다. boundary는 자동
생성된다.

```cpp
auto uploaded = client.post ("/players/7281/avatar")
                  .multipart ("visibility", "public")
                  .multipart_file ("file", "avatar.png", png_bytes, "image/png")
                  .submit_raw ()
                  .result ();
```

## streaming 업로드 (chunked)

body 전체를 메모리에 올릴 수 없는 대용량 전송은 `body_stream`을 쓴다. provider가
chunk를 돌려주다가 `std::nullopt`를 반환하면 끝이다. chunked transfer-encoding으로
전송된다.

```cpp
std::ifstream replay ("/var/games/replays/r-99182.bin", std::ios::binary);

auto result = client.post ("/replays")
                .body_stream (
                  [&replay] () -> std::optional<std::string> {
                      std::string chunk (64 * 1024, '\0');
                      replay.read (chunk.data (), chunk.size ());
                      chunk.resize (static_cast<std::size_t> (replay.gcount ()));
                      if (chunk.empty ()) {
                          return std::nullopt;   // 전송 종료
                      }
                      return chunk;
                  },
                  "application/octet-stream")
                .submit_raw ()
                .result ();
```

**제약** — provider는 한 번 소비되면 되감을 수 없으므로:

- streaming 업로드는 connection pool을 거치지 않고 항상 fresh 연결로 보낸다.
- [자동 retry](10-redirects-retries-cookies.ko.md) 대상에서 제외된다. 재시도가
  필요하면 호출자가 provider를 새로 만들어 다시 호출한다.

[다음: Response 다루기 →](06-handling-responses.ko.md)
