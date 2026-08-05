<!-- framework-adapter-nav:start -->
[스펙 목차](README.ko.md) | [이전: C++ exact interface](interfaces/README.ko.md) | [다음: C++ 내장 HTTP 서버](61-embedded-http-server.ko.md)
<!-- framework-adapter-nav:end -->

[Framework 공통 문서](../../../../README.ko.md)


# C++ HTTP hosting 공개 계약

> 이 문서는 C++ HTTP hosting이 제공해야 하는 정식 계약이다.

## 1. 요청 처리 계약

C++ HTTP hosting은 route 등록, JSON binding, DI handler 실행과 HTTP response 생성을 하나의 request
처리 흐름으로 제공한다. 아래 흐름은 public hosting 계약을 설명하기 위한 예이며 특정 sample을 계약의
근거로 사용하지 않는다.

```text
HTTP client
  POST /games
      |
      v
C++ HTTP hosting
  options.http().map_post<create_game_http_handler_t>("/games")
      |
      v
DI handler
  create_game_http_handler_t::handle(
    create_game_http_req_t request)
      |
      v
request_client_t.request(play_channel, ...).submit<create_game_res_t>()
      |
      v
HTTP JSON response
  create_game_http_res_t {
    room_id, game_name, owner_play_endpoint,
    play_endpoints, play_nodes, required_level
  }
```

C++ framework는 이 흐름을 아래 의미로 맞춘다.

- HTTP server는 app lifecycle에 묶인 hosted service다.
- HTTP route는 `map_get`, `map_post`, `map_put`, `map_delete`로 method와 path를 등록한다
  (ASP.NET Core Minimal API의 `MapGet` 계열과 같은 개념이며, C++ public 표면은 §7의 규약대로
  `snake_case`를 쓴다).
- HTTP endpoint는 `http://`와 `https://`를 모두 지원한다.
- request body는 JSON DTO로 변환한다.
- route handler는 DI에서 resolve한다.
- handler는 `request_client_t`를 주입받아 zlink channel에 요청한다.
- handler 반환 DTO는 JSON response body가 된다.
- 정상 응답은 기본 `200 OK`다.
- payload decode 실패는 `400 Bad Request`다.
- route 없음은 `404 Not Found`다.
- handler 또는 zlink request 실패는 error kind에 따라 HTTP status로 매핑한다.
- app shutdown은 HTTP accept loop와 진행 중 request dispatch를 닫는다.

HTTP hosting은 framework server package가 제공하는 JSON API 기능이다. MVC/view, static file,
template engine과 ORM은 이 계약의 지원 범위가 아니다.

## 2. Public API

일반 application은 `app.add_zlink_framework(...)` 안에서 HTTP endpoint와 route를 등록한다.

```cpp
auto app = zlink::framework::app_t::create();

app.add_zlink_framework([&](auto &options) {
    auto mesh = options.add_route_mesh(sample_names_t::application_mesh)
      .listen(7300) // 이 RouteMesh가 peer message를 받을 endpoint를 연다.
      .set_routing_id(topology.application_rid); // 같은 mesh 안에서 이 node를 식별한다.
    mesh.channel(sample_names_t::api_channel)
      .server() // 이 node를 api_channel의 요청 처리 후보로 게시한다.
      .add_handler_group("api"); // 아래 DI handler group을 Channel handler와 연결한다.
    mesh.channel(sample_names_t::play_channel)
      .client(); // Server membership 없이 play_channel 호출 경로만 등록한다.

    options.http()
      .listen(topology.api_http_endpoint) // HTTP client가 연결할 listener를 연다.
      .map_post<create_game_http_handler_t>("/games"); // POST /games를 DI handler에 연결한다.

    options.handlers()
      .group ("api")
      .add<authenticate_player_handler_t> (); // api group에서 resolve할 handler를 등록한다.
});
```

`options.http()`는 낮은 수준 `Boost.Beast`, socket, acceptor, thread, executor 타입을
노출하지 않는다.

## 3. Handler signature 형식

HTTP handler는 channel handler와 같은 방식으로 DTO와 DI 의존성을 선언한다. 다만 HTTP는
transport metadata, raw body, 직접 response 제어가 필요하므로 `handle(...)`의 인자와 반환값을
여러 형식으로 선언할 수 있다. 아래 handler signature 형식 전체가 HTTP hosting 공개 계약이다.

```cpp
struct create_game_http_req_t {
    static constexpr const char *packet_name = "CreateGameHttpReq";
    std::string game_name;
};

struct create_game_http_res_t {
    static constexpr const char *packet_name = "CreateGameHttpRes";
    std::string room_id;
    std::string game_name;
    std::string owner_play_endpoint;
    std::vector<std::string> play_endpoints;
    std::vector<play_node_info_t> play_nodes;
    int required_level = 0;
};

class create_game_http_handler_t {
public:
    using request_type = create_game_http_req_t;
    using reply_type = create_game_http_res_t;
    using dependency_types =
      zlink::framework::dependency_list_t<
        zlink::framework::request_client_t,
        zlink::framework::logger_t<create_game_http_handler_t>>;

    explicit create_game_http_handler_t(
      zlink::framework::request_client_t &client,
      zlink::framework::logger_t<create_game_http_handler_t> &logger);

    task_t<create_game_http_res_t> handle(const create_game_http_req_t &request);
};
```

handler의 `handle(...)`은 sync 반환과 `task_t<T>` 반환을 모두 허용한다. C++ HTTP handler는
`.NET`의 `Task<IResult>` 또는 `Task<T>` 의미를 `task_t<T>`로 투영한다.

```cpp
task_t<create_game_http_res_t>
create_game_http_handler_t::handle(const create_game_http_req_t &request)
{
    auto room = co_await _client
      .request (sample_names_t::play_channel,
                create_game_req_t{request.game_name})
      .submit<create_game_res_t> ();
    co_return create_game_http_res_t {room.room_id,
                                      room.game_name,
                                      room.owner_play_endpoint,
                                      room.play_endpoints,
                                      room.play_nodes,
                                      room.required_level};
}
```

`request_type`, `reply_type`, `dependency_types`, `handle(...)` 규칙은 message handler와
같게 유지한다. HTTP만 별도 생성자 주입 규칙을 만들지 않는다.

지원해야 하는 HTTP handler signature 형식은 아래와 같다.

- typed DTO: `reply_type handle(const request_type &request)`
- typed DTO async: `task_t<reply_type> handle(const request_type &request)`
- typed DTO + context:
  `reply_type handle(const request_type &request, http_context_t &context)`
- typed DTO + context async:
  `task_t<reply_type> handle(const request_type &request, http_context_t &context)`
- typed DTO + request:
  `reply_type handle(const request_type &request, const http_request_t &http)`
- typed DTO + request async:
  `task_t<reply_type> handle(const request_type &request, const http_request_t &http)`
- typed DTO + request + context:
  `reply_type handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- typed DTO + request + context async:
  `task_t<reply_type> handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- typed response: `http_response_t handle(const request_type &request)`
- typed response + context:
  `http_response_t handle(const request_type &request, http_context_t &context)`
- typed response async: `task_t<http_response_t> handle(const request_type &request)`
- typed response + context async:
  `task_t<http_response_t> handle(const request_type &request, http_context_t &context)`
- typed response + request:
  `http_response_t handle(const request_type &request, const http_request_t &http)`
- typed response + request async:
  `task_t<http_response_t> handle(const request_type &request, const http_request_t &http)`
- typed response + request + context:
  `http_response_t handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- typed response + request + context async:
  `task_t<http_response_t> handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- raw HTTP request: `http_response_t handle(const http_request_t &request)`
- raw HTTP request async: `task_t<http_response_t> handle(const http_request_t &request)`

`http_request_t`와 `http_response_t`는 zlink framework public 타입이다. `Boost.Beast`,
`Boost.Asio`, OpenSSL stream, socket 타입을 handler signature에 직접 쓰면 public contract
위반이다.

`map_*<THandler>(...)`는 위 signature 형식을 compile-time으로 판별한다. `request_type`이 있으면
typed route로 등록하고, `handle(const http_request_t&)`만 있으면 raw route로 등록한다.
typed route에서 여러 overload가 있으면 반환 타입보다 인자 형식을 먼저 본다.
`http_request_t`와 `http_context_t`를 모두 받는 형식이 가장 먼저 선택되고, 그 다음
`http_request_t`, `http_context_t`, DTO-only 형식 순서로 선택된다.
typed route와 raw route signature를 한 handler에 동시에 제공하면 route mode가 모호하므로 static
assertion 또는 startup validation으로 실패시킨다.

Handler signature는 아래 순서로 판별한다.

1. `request_type` alias가 있으면 typed route 후보로 본다.
2. `request_type` alias가 없고 `handle(const http_request_t&)`가 있으면 raw route로 본다.
3. typed route는 `reply_type` 또는 `http_response_t` 반환 형식 중 하나를 가져야 한다.
4. typed route와 raw route signature가 한 handler에 같이 있으면 실패한다.
5. typed route 안에서 여러 signature가 있으면 아래 우선순위로 하나만 선택한다.

Typed route 호출 우선순위:

1. `handle(const request_type&, const http_request_t&, http_context_t&)`
2. `handle(const request_type&, const http_request_t&)`
3. `handle(const request_type&, http_context_t&)`
4. `handle(const request_type&)`

각 signature의 반환값은 `reply_type`, `http_response_t`, `task_t<reply_type>`,
`task_t<http_response_t>` 중 하나여야 한다. 이 우선순위는 반환 타입보다 인자 형식을 먼저 본다.
같은 handler가 `reply_type handle(request, http, context)`와
`http_response_t handle(request, http)`를 모두 제공하면 첫 번째 signature가 선택된다.
선택된 signature가 `http_response_t`를 반환하면 status/header/body를 직접 제어하고,
`reply_type`을 반환하면 serializer가 DTO를 HTTP body로 변환한다.
`http_request_t` 인자는 raw HTTP metadata가 필요하다는 뜻이므로 `http_context_t`보다 우선한다.

Raw route 호출 우선순위:

1. `task_t<http_response_t> handle(const http_request_t&)`
2. `http_response_t handle(const http_request_t&)`

Raw route는 `request_type`, `reply_type` alias를 요구하지 않는다. raw route handler가
`reply_type` DTO를 반환하면 실패한다. raw route는 framework가 response serializer를 추론할 수
없기 때문이다.

실패해야 하는 handler signature:

| 조건 | 실패 이유 |
|------|-----------|
| `request_type`이 있으나 호출 가능한 typed `handle(...)`이 없음 | route를 실행할 수 없다 |
| `request_type`이 있으나 DTO 반환 signature에 `reply_type`이 없음 | DTO serializer를 알 수 없다 |
| `request_type`이 없고 raw `handle(http_request_t)`도 없음 | route mode를 정할 수 없다 |
| typed signature와 raw signature를 동시에 제공 | typed/raw route mode가 모호하다 |
| raw handler가 `reply_type` 또는 임의 DTO를 반환 | raw response serializer를 추론할 수 없다 |
| handler가 Beast/Asio/OpenSSL 타입을 받음 | public dependency 경계를 위반한다 |
| 둘 이상의 같은 우선순위 overload가 호출 가능 | overload 선택이 모호하다 |

## 4. Route Builder

지원 범위는 typed JSON route와 raw HTTP route다. `GET`, `POST`, `PUT`, `DELETE`를 같은
규칙으로 등록할 수 있어야 한다.

```cpp
namespace zlink::framework {

class http_options_builder_t {
public:
    http_options_builder_t &listen(std::string endpoint);
    http_options_builder_t &configure_tls(
      std::function<void(http_tls_options_builder_t &)> configure);
    http_options_builder_t &configure_server(
      std::function<void(http_server_options_builder_t &)> configure);

    template <typename THandler>
    http_options_builder_t &map_get(std::string path);
    template <typename THandler>
    http_options_builder_t &map_post(std::string path);
    template <typename THandler>
    http_options_builder_t &map_put(std::string path);
    template <typename THandler>
    http_options_builder_t &map_delete(std::string path);

    template <typename TMiddleware>
    http_options_builder_t &use();

    http_options_builder_t &map_health(std::string path);
    http_options_builder_t &map_readiness(std::string path);
    http_options_builder_t &map_liveness(std::string path);
};

struct http_context_t {
    http_method_t method;
    std::string path;
    std::string correlation_id;
    std::map<std::string, std::string> request_headers;
    std::map<std::string, std::string> response_headers;
    std::optional<std::string> response_body;
    int response_status;

    http_context_t &response_header(std::string name, std::string value);
    http_context_t &json_response(int status, std::string body);
};

struct http_request_t {
    http_method_t method;
    std::string path;
    std::string target;
    std::string query_string;
    std::string correlation_id;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> route_values;
    std::map<std::string, std::string> query_values;
    std::string body;
    std::string content_type;
    std::string remote_endpoint;
};

struct http_response_t {
    int status = 200;
    std::string body;
    std::string content_type = "application/json";
    std::map<std::string, std::string> headers;

    http_response_t &header(std::string name, std::string value);
};

} // namespace zlink::framework
```

`http_request_t` field 계약:

| field | 의미 |
|-------|------|
| `method` | route matching에 사용한 HTTP method |
| `path` | query string을 제거한 path |
| `target` | 원본 request target. path와 query string을 포함한다 |
| `query_string` | `?` 뒤 query 문자열. 없으면 빈 문자열 |
| `correlation_id` | `X-Correlation-Id`, `X-Request-Id`, 또는 runtime 생성 id |
| `headers` | HTTP header name/value. header name은 runtime의 canonical form을 사용한다 |
| `route_values` | `{name}` path segment binding 결과 |
| `query_values` | query string binding 결과 |
| `body` | limit 검증이 끝난 request body |
| `content_type` | `Content-Type` header 값. 없으면 빈 문자열 |
| `remote_endpoint` | 가능한 경우 client endpoint. 알 수 없으면 빈 문자열 |

`http_response_t` field 계약:

| field | 의미 |
|-------|------|
| `status` | HTTP status code. 기본값은 `200` |
| `body` | response body bytes. string은 UTF-8 text 또는 binary-safe byte buffer로 취급한다 |
| `content_type` | `Content-Type` response header. 기본값은 `application/json` |
| `headers` | response header name/value |

`http_request_t`와 `http_response_t`는 request 처리 중 runtime이 소유한 값의 복사본이다.
handler는 이 객체의 reference를 저장하면 안 된다. request 완료 뒤 lifetime은 보장하지 않는다.

`listen(...)` endpoint는 `http://host:port`와 `https://host:port` 형식을 사용한다.

- `http://127.0.0.1:18080`
- `http://0.0.0.0:18080`
- `http://[::1]:18080`
- `https://127.0.0.1:18443`
- `https://0.0.0.0:18443`
- `https://[::1]:18443`

`https://` endpoint를 사용할 때는 TLS server certificate와 private key를 함께 설정해야
한다. TLS는 HTTP hosting의 transport option이고, zlink channel transport 설정과 섞어
설명하지 않는다.

```cpp
options.http()
  .listen("https://0.0.0.0:8443")
  .configure_tls([](auto &tls) {
      tls.certificate_file("certs/server.crt")
        .private_key_file("certs/server.key");
  })
  .map_post<create_game_http_handler_t>("/games");
```

`map_post<THandler>(path)`는 typed route에서 아래 작업을 한 번에 수행한다.

- `THandler`를 service collection에 등록한다.
- `THandler::request_type`과 `THandler::reply_type`의 JSON serializer를 등록한다.
- method와 path를 route table에 등록한다.
- request마다 DI scope를 만들고 `THandler`를 resolve한다.
- `handle(...)`을 framework handler coroutine executor에서 실행한다.
- 결과 DTO를 JSON response body로 직렬화한다.
- handler가 `handle(request, http_context_t&)`를 제공하면 correlation id와 header 같은 HTTP
  문맥을 함께 받을 수 있다. `handle(request)`만 제공하는 기존 handler도 그대로 동작한다.

raw route는 `request_type` serializer를 요구하지 않는다. runtime은 `http_request_t`를 만들어
handler에 넘기고, handler가 반환한 `http_response_t`를 그대로 HTTP response로 쓴다. raw route도
middleware, correlation id, timeout, limit, logging, metrics 정책을 똑같이 통과한다.

Invoker 생성 의사 코드는 아래와 같다.

```cpp
template <typename THandler>
http_route_invoker_t make_invoker()
{
    if constexpr (has_request_type<THandler>) {
        static_assert(!has_raw_http_only_shape<THandler>);
        register_json_serializer<typename THandler::request_type>();
        if constexpr (returns_typed_dto<THandler>) {
            register_json_serializer<typename THandler::reply_type>();
        }
        return make_typed_invoker<THandler>();
    } else {
        static_assert(has_raw_http_shape<THandler>);
        return make_raw_invoker<THandler>();
    }
}
```

Typed invoker 처리 순서:

1. body, route value, query value를 하나의 binding JSON으로 합친다.
2. `request_type` serializer로 DTO를 만든다.
3. `http_request_t`와 `http_context_t`를 만든다.
4. 우선순위에 따라 handler overload를 호출한다.
5. 결과가 `reply_type`이면 `http_context_t`의 status/header와 함께 JSON response를 만든다.
6. 결과가 `http_response_t`이면 response object를 기준으로 HTTP response를 만든다.

Raw invoker 처리 순서:

1. content type이 JSON인지 검사하지 않는다.
2. body/header/route/query limit은 동일하게 적용한다.
3. `http_request_t`를 만든다.
4. raw handler를 호출한다.
5. 반환된 `http_response_t`를 기준으로 HTTP response를 만든다.

Response precedence:

| handler result | 우선순위 |
|----------------|----------|
| `http_response_t` 반환 | `http_response_t`의 status/header/content type/body가 최우선 |
| DTO 반환 + `http_context_t::json_response(...)` 설정 | context의 status/body/header를 사용 |
| DTO 반환 + context header/status만 설정 | context status/header + DTO JSON body 사용 |
| DTO 반환만 있음 | `200 OK`, `application/json`, DTO JSON body 사용 |

middleware `after(...)`는 handler result가 만들어진 뒤 실행된다. `after(...)`가 response header를
추가하면 기존 header를 같은 이름으로 덮어쓸 수 있다. 단, `Content-Length`는 runtime이 최종 body
기준으로 계산하므로 handler나 middleware가 직접 고정하지 않는다.

route parameter와 query string binding은 ASP.NET Core model binding을 단순화해서 따른다.

```cpp
options.http()
  .listen("http://0.0.0.0:8080")
  .map_get<get_game_http_handler_t>("/games/{gameId}");
```

handler request DTO에는 body, route, query에서 온 값이 합쳐진다. 충돌이 있으면
route parameter, query string, body 순서로 우선순위를 둔다. 이 우선순위는 startup
validation 문서와 테스트에서 고정한다.

`use<TMiddleware>()`는 `TMiddleware::before(http_context_t&)`와
`TMiddleware::after(http_context_t&)`를 route handler 앞뒤로 호출한다. middleware는 raw
Beast request나 socket을 받지 않고, `http_context_t`의 correlation id와 framework header
map만 사용한다. request에 `X-Correlation-Id` 또는 `X-Request-Id`가 있으면 그 값을 response
`X-Correlation-Id`로 되돌려 보내고, 없으면 runtime이 request correlation id를 만든다.
middleware가 `before(...)`에서 `json_response(status, body)`를 호출하면 HTTP runtime은
handler를 호출하지 않고 해당 JSON response를 반환한다. 이 short-circuit 경로도 `after(...)`
middleware를 거치므로 logging/correlation 처리를 한 곳에 둘 수 있다.

`map_health(path)`, `map_readiness(path)`, `map_liveness(path)`는 `app.health()` report를
HTTP JSON endpoint로 노출한다. readiness 또는 liveness가 `unhealthy`면 해당 endpoint는
`503 Service Unavailable`을 반환한다. 이 route는 사용자가 별도 handler를 만들지 않아도 되고,
health 집계 규칙은 `contracts/eventing/health.hpp`와 runtime diagnostics 구현이 소유한다.

## 5. Request / Response 계약

기본 typed route의 HTTP 계약은 아래와 같다.

| 항목 | 계약 |
|------|------|
| method | `GET`, `POST`, `PUT`, `DELETE` |
| scheme | `http` 또는 `https` |
| content type | `application/json` |
| request body | body가 있는 method에서는 `THandler::request_type` JSON |
| route parameter | `{name}` segment를 request DTO field에 binding |
| query string | `?name=value`를 request DTO field에 binding |
| response body | `THandler::reply_type` JSON |
| success status | `200 OK` |
| route not found | `404 Not Found` |
| method mismatch | `405 Method Not Allowed` |
| unsupported content type | `400 Bad Request`(`protocol_error`) |
| invalid JSON | `400 Bad Request` |
| body limit exceeded | `413 Payload Too Large` |
| serializer registration | `map_*<THandler>`가 request/reply JSON serializer를 등록한다 |
| handler failure | error kind 기반 status mapping |

HTTP response에는 `Content-Type: application/json`을 기본으로 둔다. error response도 JSON
object로 반환한다.

```json
{
  "error": "protocol_error",
  "message": "payload deserialization failed"
}
```

## 6. Error Mapping

framework error kind는 HTTP status로 매핑한다.

| framework error | HTTP status | 이유 |
|-----------------|-------------|------|
| `protocol_error` | `400 Bad Request` | client body가 DTO로 변환되지 않았거나 request 의미가 framework 계약과 맞지 않는다 |
| `not_found` | `404 Not Found` | 대상 route, channel, service를 찾지 못했다 |
| `framework_exception_t::code() == std::errc::timed_out` | `504 Gateway Timeout` | HTTP hosting 뒤의 zlink request가 시간 안에 끝나지 않았다 |
| `framework_exception_t::code()`가 [shutdown](../../../01-glossary.ko.md#shutdown) 경계를 나타내고 host가 종료 중임 | `503 Service Unavailable` | host가 종료 중이다 |
| `internal_failure` | `500 Internal Server Error` | 내부 handler 또는 runtime 실패다 |

HTTP server runtime이 body size limit을 초과한 request를 감지하면 `413 Payload Too Large`로
닫는다. JSON route에 `application/json`과 호환되지 않는 content type이 들어오면
`protocol_error`를 던지고 `400 Bad Request`로 닫는다. 두 status는 handler failure가 아니라 server
request validation 실패다.

error kind가 명확하지 않은 예외는 `500 Internal Server Error`로 닫고 log/monitoring에
원인을 남긴다. HTTP client에는 C++ exception type 이름을 그대로 노출하지 않는다.

## 7. Lifecycle

HTTP server는 `hosted_service_t`로 실행한다.

- `app.run(...)`이 service provider를 만든 뒤 HTTP hosted service를 시작한다.
- `app.stop()` 또는 signal shutdown이 들어오면 acceptor를 닫고 새 request를 받지 않는다.
- 진행 중 request는 graceful shutdown timeout 안에서 완료되도록 기다린다.
- shutdown 중 새 zlink submit을 무기한 만들지 않는다.

HTTP endpoint는 handler가 주입받는 framework client가 ready 상태가 된 뒤에만 [ready](../../../01-glossary.ko.md#ready)로 전환한다.
종료 시에는 새 HTTP request를 먼저 거부하고 진행 중 handler가 messaging drain 기한 안에서 완료될
기회를 제공한다.

## 7.1 Middleware

HTTP middleware는 ASP.NET Core의 cross-cutting pipeline 개념을 C++ 형태로 투영한다.
core 범위는 `options.http().use<TMiddleware>()`와 `http_context_t` 기반 before/after hook이다.
route별 filter 타입은 별도 public API로 두지 않고, middleware에서 method/path를 확인해 처리한다.

필수 축은 아래와 같다.

- exception filter
- logging filter
- validation filter
- auth filter
- correlation id filter
- custom middleware registration

middleware는 DI를 사용할 수 있다. public API에 `boost::beast` request/response를 노출하지
않고 framework의 `http_context_t` 같은 추상 타입을 사용한다. `http_context_t`는 request id,
method, path, header 조회, response status 설정, short-circuit JSON response 설정을 제공한다.

## 8. 설치 header 경계

HTTP hosting의 public contract는 다음 설치 header에서 제공한다.

```text
zlink/framework/contracts/http/http.hpp
zlink/framework.hpp
```

Public header에는 Boost.Beast, Boost.Asio, OpenSSL/SSL context, TCP socket, acceptor와 request
parser 타입을 노출하지 않는다. Application과 extension은 이 구현 dependency를 include하지 않고
framework의 `http_request_t`, `http_response_t`와 `http_context_t`만 사용한다.

## 9. C++ HTTP hosting 표면

| 기능 | C++ framework |
|------|---------------|
| application 생성 | `app_t::create()` |
| HTTP endpoint | `options.http().listen(url)` |
| HTTPS certificate | `options.http().listen("https://...").configure_tls(...)` |
| framework 등록 | `app.add_zlink_framework(...)` |
| POST route | `options.http().map_post<handler_t>("/games")` |
| GET route | `options.http().map_get<handler_t>("/games/{id}")` |
| request body binding | JSON serializer로 `request_type` 생성 |
| route/query binding | route parameter와 query string을 `request_type`에 병합 |
| dependency injection | `dependency_types` 기반 생성자 주입 |
| middleware/filter | `options.http().use<TMiddleware>()`와 `http_context_t` |
| 실행 중단 | host shutdown/drain 정책. public handler signature에는 기본 취소 인자를 노출하지 않음 |
| typed reply | handler가 `reply_type` DTO 반환 |

## 10. 공개 계약 결정

| 항목 | 계약 |
|------|------|
| package 위치 | HTTP hosting은 framework server package가 제공한다. |
| 구현 dependency 노출 | public header와 signature에 Beast, Asio와 SSL 구현 타입을 노출하지 않는다. |
| HTTPS/TLS | certificate와 private key 설정을 public option으로 제공한다. |
| route matching | exact path와 `{name}` path parameter를 지원한다. |
| method | `GET`, `POST`, `PUT`, `DELETE`를 같은 builder 규칙으로 지원한다. |
| cancellation | handler signature에 별도 cancellation token을 추가하지 않고 request timeout과 host drain 계약을 적용한다. |
| response customization | typed DTO의 기본 status는 `200 OK`다. status와 header 직접 제어는 `http_response_t` 반환 handler가 제공한다. |
| server hardening | [Embedded HTTP Server](61-embedded-http-server.ko.md)의 limit, timeout과 shutdown 계약을 따른다. |

## 11. 회귀 테스트

최소 테스트는 아래 축으로 둔다.

- contract header compile: `#include <zlink/framework.hpp>`와
  `#include <zlink/framework/contracts/http/http.hpp>`
- route registry: 같은 method/path 중복 등록과 system route 충돌은 startup validation 실패
- method별 route: `GET`, `POST`, `PUT`, `DELETE`
- scheme별 listen: `http://`, `https://`
- HTTPS TLS option: certificate/private key 누락은 startup validation 실패
- HTTPS loopback: test certificate로 JSON request/response 성공
- HTTP handler e2e: loopback client로 `GET`, `POST`, `PUT`, `DELETE` route를 호출해
  DTO binding, DI handler 실행, JSON response, status mapping을 검증한다
- handler signature matrix: DTO, DTO+context, DTO+request, response 반환, raw request의 sync/async
  signature를 모두 호출한다
- raw HTTP request: `http_request_t`가 method, target, header, route/query, body를 포함한다
- raw HTTP response: `http_response_t`의 status, header, content type, body가 그대로 반환된다
- ambiguous handler signature: 모호한 `handle(...)` 조합은 static assertion 또는 startup validation 실패
- response precedence: `http_response_t`, `json_response`, context header/status, DTO 기본 응답
  우선순위를 고정한다
- route parameter binding: `/games/{gameId}`를 DTO field에 할당한다
- query string binding: `?page=1`을 DTO field에 할당한다
- body/route/query merge 우선순위 고정
- JSON binding: request body를 DTO로 변환하고 reply DTO를 JSON으로 반환
- DI: HTTP handler가 `request_client_t`를 생성자 주입으로 받는다
- middleware: before hook 등록 순서, after hook 역순 실행, 요청 단위 상태 보존, short-circuit
- error mapping: invalid JSON은 `400`, unknown route는 `404`, timeout은 `504`
- server validation: unsupported content type은 `400`, body limit 초과는 `413`
- embedded server lifecycle: keep-alive, request timeout, graceful shutdown drain, connection
  metrics는 [Embedded HTTP Server](61-embedded-http-server.ko.md) 회귀 테스트로 검증한다
- lifecycle: `app.stop()`이 새 request 수락을 중단하고 진행 중 request를 drain한 뒤 완료된다

Handler signature regression matrix:

| 테스트 | 기대 |
|--------|------|
| DTO sync | `reply_type handle(request)`가 `200` JSON을 반환 |
| DTO async | `task_t<reply_type> handle(request)`가 await 뒤 JSON 반환 |
| DTO context sync | context header/status가 response에 반영 |
| DTO context async | async handler와 context 변경이 함께 반영 |
| DTO request sync | `http_request_t`의 header/query/body를 읽을 수 있음 |
| DTO request async | async handler가 `http_request_t`를 받고 정상 완료 |
| response sync | `http_response_t` status/header/body가 그대로 반환 |
| response context | `http_response_t`가 context body보다 우선 |
| response request | `http_request_t`를 읽고 `http_response_t`로 응답 |
| raw request sync | serializer 없이 raw body를 받아 응답 |
| raw request async | raw request async handler가 정상 완료 |
| raw content type | JSON이 아닌 content type도 raw route에서 허용 |
| ambiguous route mode | typed signature와 raw signature가 한 handler에 있으면 실패 |
| invalid return type | raw route가 DTO를 반환하면 실패 |
| content length | handler가 준 `Content-Length`는 runtime 최종값으로 보정 |

---
<!-- framework-adapter-nav:bottom:start -->
[스펙 목차](README.ko.md) | [이전: C++ exact interface](interfaces/README.ko.md) | [다음: C++ 내장 HTTP 서버](61-embedded-http-server.ko.md)
<!-- framework-adapter-nav:bottom:end -->
