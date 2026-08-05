[← Table Of Contents](README.en.md)

# 2. Getting Started

## CMake Integration

Just link the `zlink::http_client` target. Since the public header doesn't expose Beast/OpenSSL, no
extra dependency setup is needed on the consumer side.

```cmake
find_package(zlink CONFIG REQUIRED)

add_executable(matchmaker_cli src/main.cpp)
target_link_libraries(matchmaker_cli PRIVATE zlink::http_client)
```

In source, include only the one facade header.

```cpp
#include <zlink/http_client.hpp>
```

## First Request

This is the most basic flow for reading a player profile from a game API server.

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

This splits into three steps.

1. `create(base_url)...build()` — creates the client. At this point, a runtime with a connection
   pool is created.
2. `client.get(path)...submit<T>()` — builds and sends the request.
3. `.result()` — waits for the result, received as a `result_t` (a success/failure wrapper).

JSON conversion is wired through [nlohmann ADL functions](05-request-body.en.md) (`to_json`/
`from_json`). The response structure (`result_t` → `http_response_t<T>` → DTO) is explained in
[6. Handling Responses](06-handling-responses.en.md).

## Shorter: fetch

Where you only need the typed body and it's fine to receive a failure as an exception (tests, client
scenarios), `fetch<T>()` unwraps everything and directly returns the DTO.

```cpp
auto profile = client.get ("/players/7281").fetch<player_profile_t> ();
// throws zlink::framework::framework_exception_t on failure
```

## One-Line Request: Skipping build()

If it's just one request, you can start the request directly on the builder, without `build()`.
Since the request keeps the client (and runtime) alive until the end, this is safe even with a
temporary builder.

```cpp
auto created = zlink::http_client::client_t::create (options.api_http_endpoint)
                 .post ("/games")
                 .body (create_game_http_req_t{.game_name = "ranked-match-0611"})
                 .fetch<create_game_http_res_t> ();
```

However, if you make several requests to the same server, you need to build a client with `build()`
and reuse it to get the connection pool's benefit (keep-alive reuse) — see
[3. Client Configuration](03-client-configuration.en.md).

[Next: Client Configuration →](03-client-configuration.en.md)
