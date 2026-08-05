[← 목차](README.ko.md)

# 2. 시작하기

## CMake 연동

`zlink::http_client` target을 링크하면 끝이다. public header가 Beast/OpenSSL을
노출하지 않으므로 소비자 쪽에 추가 의존성 설정이 필요 없다.

```cmake
find_package(zlink CONFIG REQUIRED)

add_executable(matchmaker_cli src/main.cpp)
target_link_libraries(matchmaker_cli PRIVATE zlink::http_client)
```

소스에서는 facade header 하나만 include한다.

```cpp
#include <zlink/http_client.hpp>
```

## 첫 요청

게임 API 서버에서 플레이어 프로필을 읽어 오는 가장 기본적인 흐름이다.

```cpp
#include <zlink/http_client.hpp>
#include <iostream>

struct player_profile_t
{
    std::string nickname;
    int rating = 0;
};

void from_json (const nlohmann::json &json, player_profile_t &value)
{
    value.nickname = json.at ("nickname").get<std::string> ();
    value.rating = json.at ("rating").get<int> ();
}

int main ()
{
    auto client = zlink::http_client::client_t::create ("https://game-api.example.internal")
                    .timeout (std::chrono::seconds (3))
                    .build ();

    auto result = client.get ("/players/7281").submit<player_profile_t> ().result ();
    if (!result) {
        std::cerr << "request failed: " << result.error ()->what () << '\n';
        return 1;
    }

    const auto &profile = result.value ().body;
    std::cout << profile.nickname << " (rating " << profile.rating << ")\n";
    return 0;
}
```

세 단계로 나뉜다.

1. `create(base_url)...build()` — client를 만든다. 이 시점에 connection pool을
   가진 runtime이 생긴다.
2. `client.get(path)...submit<T>()` — request를 구성하고 보낸다.
3. `.result()` — 결과를 기다려 `result_t`(성공/실패 래퍼)로 받는다.

JSON 변환은 [nlohmann ADL 함수](05-request-body.ko.md)(`to_json`/`from_json`)로
연결된다. 응답 구조(`result_t` → `http_response_t<T>` → DTO)는
[6. Response 다루기](06-handling-responses.ko.md)에서 풀어 설명한다.

## 더 짧게: fetch

typed body만 필요하고 실패를 예외로 받아도 되는 곳(테스트, client 시나리오)에서는
`fetch<T>()`가 래퍼를 모두 풀어 DTO를 직접 돌려준다.

```cpp
auto profile = client.get ("/players/7281").fetch<player_profile_t> ();
// 실패 시 zlink::framework::framework_exception_t throw
```

## 한 줄 요청: build() 생략

요청이 한 번뿐이면 `build()` 없이 builder에서 바로 request를 시작할 수 있다.
요청이 client(와 runtime)를 끝까지 살려주므로 임시 builder여도 안전하다.

```cpp
auto created = zlink::http_client::client_t::create (options.api_http_endpoint)
                 .post ("/games")
                 .body (create_game_http_req_t{.game_name = "ranked-match-0611"})
                 .fetch<create_game_http_res_t> ();
```

단, 같은 서버에 여러 번 요청한다면 `build()`로 client를 만들어 재사용해야
connection pool의 이득(keep-alive 재사용)을 본다 —
[3. Client 구성](03-client-configuration.ko.md) 참고.

[다음: Client 구성 →](03-client-configuration.ko.md)
