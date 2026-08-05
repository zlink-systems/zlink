<!-- framework-adapter-nav:start -->
[스펙 목차](README.ko.md) | [이전: C++ HTTP Hosting](60-http-hosting.ko.md)
<!-- framework-adapter-nav:end -->

[Framework 공통 문서](../../../../README.ko.md)


# C++ 내장 HTTP server 공개 계약

> 이 문서는 `C++` framework가 제공하는 내장 HTTP server의 공개 계약을 정리한다.
>
> [HTTP Hosting](60-http-hosting.ko.md)이 사용자가 보는 route, handler, DTO binding
> 표면을 다룬다면, 이 문서는 endpoint, connection lifecycle, timeout, TLS, shutdown과
> observability 계약을 다룬다.

## 1. 공개 계약에서 고정하는 선택

`ZLink Framework for C++`는 별도 HTTP framework 없이 backend API를 제공할 수 있다. public
application model은 route mapping, typed DTO binding, middleware와 lifecycle을 C++ 스타일로
제공한다.

핵심 결정은 아래와 같다.

- public API는 zlink가 소유한다.
- route mapping은 `map_get`, `map_post`, `map_put`, `map_delete`처럼 HTTP method가 드러나는
  `map_get`, `map_post`, `map_put`, `map_delete`를 사용한다.
- server endpoint는 C++ 사용자에게 직관적인 `listen(...)`으로 표현한다.
- `Boost.Beast`, `Boost.Asio`, OpenSSL stream, socket, acceptor 타입은 public header에
  노출하지 않는다.
- 내장 server는 HTTP/1.1 backend API server 계약을 제공한다.
- HTTP/2, HTTP/3, WebSocket, static file server, template engine, ORM은 이 계약의 지원 범위가 아니다.

## 2. 내장 server capability

`options.http().listen(...)`으로 endpoint를 등록하면 application host lifecycle에 맞춰 server가
시작되고 종료된다. 내장 server는 아래 기능을 제공한다.

- `http://`, `https://` endpoint parse
- TCP listen과 HTTP request 수신
- HTTPS endpoint의 TLS handshake
- connection당 request loop와 keep-alive
- request header/body/write/keep-alive timeout
- request body size limit와 header size limit
- max connections 한도와 overload 처리
- HTTP method와 path 기반 route matching
- path parameter, query parameter binding
- health/readiness/liveness route
- request별 DI scope 생성
- middleware before/after 실행
- 동기·비동기 handler 호출
- framework error kind를 HTTP status와 JSON error body로 매핑
- HTTP response write
- application 종료 시 새 request 수락 중단과 진행 중 request drain

내장 HTTP host는 backend API framework의 기본 server로 다음 기능을 제공한다.

- connection/request observability extension point
- request logging과 correlation id의 표준화
- malformed request에 대한 `400 Bad Request`
- server option startup validation

## 3. Public API

HTTP server API는 route handler API와 섞이되, 내부 server 구현 타입은 숨긴다.

```cpp
app.add_zlink_framework ([&] (auto &options) {
    options.http ()
      .listen ("https://0.0.0.0:8443")
      .configure_tls ([] (auto &tls) {
          tls.certificate_file ("cert.pem")
             .private_key_file ("key.pem");
      })
      .configure_server ([] (auto &server) {
          server.set_max_connections (4096)
                .set_max_request_body_size (1024 * 1024)
                .set_request_headers_timeout (std::chrono::seconds (15))
                .set_keep_alive_timeout (std::chrono::seconds (60));
      })
      .map_get<get_game_handler_t> ("/games/{id}")
      .map_post<create_game_http_handler_t> ("/games")
      .map_put<update_game_handler_t> ("/games/{id}")
      .map_delete<delete_game_handler_t> ("/games/{id}");
});
```

위 예시는 embedded HTTP server option을 포함한 정식 public API다. `map_get`,
`map_post`, `map_put`, `map_delete`, `listen`, `configure_tls`, `configure_server`는
[framework option builder naming](../../../../../../../../doc/principal/framework-option-builder-naming.ko.md)
원칙을 따른다.

이 표면은 아래 규칙을 따른다.

- C++ public method는 `snake_case`를 사용한다.
- route mapping은 `.NET` Minimal API 개념을 따른다.
- endpoint listen 설정은 C++ 서버 구성으로 읽히게 `listen(...)`을 사용한다.
- TLS 설정 영역은 `configure_tls(...)`로 연다.
- TLS 설정은 마지막 `listen(...)` endpoint에 적용되거나, endpoint builder를 반환하는 방식으로
  명확히 묶는다.
- server runtime 설정 영역은 `configure_server(...)`로 연다.
- server option은 route handler가 아니라 HTTP server runtime에 적용된다.
- logger, DI, serializer, monitoring은 별도 framework 표면과 연결되며 HTTP server가 자체
  독립 framework처럼 노출하지 않는다.

## 4. Endpoint 와 TLS

endpoint는 `http://host:port` 또는 `https://host:port` 형식이다. port를 생략하면 scheme에 따라
기본 port(`http` 80, `https` 443)를 채운다. host가 없으면 startup validation에서 실패한다.

TLS는 endpoint별 설정이다. HTTPS endpoint에는 certificate와 private key가 필요하다. TLS 설정이
없는 HTTPS endpoint는 runtime start 뒤가 아니라 options apply 또는 hosted service start 전에
실패해야 한다.

TLS 계약은 다음과 같다.

- certificate/private key 파일 존재 여부를 startup에서 확인한다.
- TLS handshake timeout을 둔다.
- TLS handshake 실패는 request handler 오류가 아니라 connection 오류로 집계한다.
- public API는 OpenSSL 또는 Asio SSL 타입을 노출하지 않는다.

## 5. Connection lifecycle

connection lifecycle은 다음 공개 동작을 보장한다.

- keep-alive가 켜진 HTTP/1.1 connection은 여러 request를 처리한다.
- `max_connections`를 넘는 새 connection은 server option에 정한 overload 결과로 종료한다.
- [shutdown](../../../01-glossary.ko.md#shutdown) 시작 뒤에는 새 connection을 받지 않는다.
- shutdown drain 동안 active request는 timeout 안에서 완료를 기다린다.
- drain timeout이 지나면 connection을 닫는다.

## 6. Request 처리 흐름

request 처리의 표준 흐름은 아래와 같다.

```text
Read request
  -> validate method / target / headers
  -> create context
  -> match system route
  -> match user route
  -> create DI scope
  -> run middleware before
  -> bind body / route / query
  -> invoke handler
  -> run middleware after
  -> map response
  -> write response
```

중요한 규칙:

- malformed request는 `400 Bad Request`로 닫는다.
- 지원하지 않는 method는 `405 Method Not Allowed`로 닫는다.
- route가 없으면 `404 Not Found`로 닫는다.
- content type이 맞지 않으면 `protocol_error`를 던지고 `400 Bad Request`로 닫는다.
- body size가 limit를 넘으면 `413 Payload Too Large`를 사용한다.
- handler timeout은 `framework_error_kind_t::deadline_exceeded`로 식별하고
  `504 Gateway Timeout`을 사용한다.
- shutdown 중 새 request는 `framework_error_kind_t::shutting_down`과 host 상태로
  식별하여 `503 Service Unavailable`로 닫거나 connection을 drain 정책에 따라 닫는다.
- Framework exception의 HTTP status와 JSON body는 `framework_error_kind_t`를 기준으로 만든다.
  `framework_exception_t::code()`는 platform 원인을 log에 남길 때만 사용한다.

## 7. Binding 과 Handler 통합

HTTP server는 handler model을 새로 만들지 않는다. [C++ HTTP Hosting](60-http-hosting.ko.md)의 handler signature를
그대로 사용한다.

```cpp
class create_game_http_handler_t {
  public:
    using request_type = create_game_http_req_t;
    using reply_type = create_game_http_res_t;
    using dependency_types =
      zlink::framework::dependency_list_t<
        zlink::framework::request_client_t,
        zlink::framework::logger_t<create_game_http_handler_t>>;

    task_t<create_game_http_res_t> handle (const create_game_http_req_t &request);
};
```

server runtime은 typed route에서 아래 작업을 담당한다.

- route path와 query를 request binding input에 합친다.
- JSON body를 request DTO로 deserialize한다.
- request DI scope를 만든다.
- handler를 framework DI에서 resolve한다.
- handler 결과 DTO를 JSON body로 serialize한다.

server runtime은 raw route에서 아래 작업을 담당한다.

- `http_request_t`를 만든다.
- method, path, target, route value, query value, header, body, content type, remote endpoint를
  public framework type으로 복사한다.
- handler를 framework DI에서 resolve한다.
- handler가 반환한 `http_response_t`의 status, header, content type, body를 HTTP response로 쓴다.

지원해야 하는 handler signature:

- `reply_type handle(const request_type &request)`
- `task_t<reply_type> handle(const request_type &request)`
- `reply_type handle(const request_type &request, http_context_t &context)`
- `task_t<reply_type> handle(const request_type &request, http_context_t &context)`
- `reply_type handle(const request_type &request, const http_request_t &http)`
- `task_t<reply_type> handle(const request_type &request, const http_request_t &http)`
- `reply_type handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- `task_t<reply_type> handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- `http_response_t handle(const request_type &request)`
- `http_response_t handle(const request_type &request, http_context_t &context)`
- `task_t<http_response_t> handle(const request_type &request)`
- `task_t<http_response_t> handle(const request_type &request, http_context_t &context)`
- `http_response_t handle(const request_type &request, const http_request_t &http)`
- `task_t<http_response_t> handle(const request_type &request, const http_request_t &http)`
- `http_response_t handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- `task_t<http_response_t> handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- `http_response_t handle(const http_request_t &request)`
- `task_t<http_response_t> handle(const http_request_t &request)`

handler는 socket, HTTP parser, Beast request, TLS stream을 알면 안 된다. HTTP 세부 정보가
필요하면 `http_request_t`, response 직접 제어가 필요하면 `http_response_t`를 사용한다.

## 8. Middleware, Filter, Error Boundary

middleware는 HTTP context를 다룬다. filter는 handler invocation과 message dispatch 정책을
다룬다. 둘을 같은 개념으로 섞지 않는다.

HTTP middleware 책임:

- correlation id
- request logging
- auth header 검사
- response header 추가
- short-circuit response
- CORS 같은 HTTP 전용 정책

handler/filter 책임:

- DTO validation
- handler exception masking
- zlink request failure mapping
- business level audit

middleware `after`는 handler 성공뿐 아니라 short-circuit, handler exception, binding failure
경로에서도 가능한 한 실행되어야 한다. 그래야 logging/correlation 지식이 handler마다 반복되지 않는다.

## 9. Server Options

server option은 endpoint 전체 또는 HTTP server 전체에 적용된다. route handler마다 같은 option을
반복하게 만들지 않는다.

public option과 기본값은 다음과 같다.

| option | 기본값 | 의미 |
|--------|--------|------|
| `max_connections` | 1,024 | 동시에 유지할 active connection 수 |
| `max_request_body_size` | 1 MiB | JSON body 최대 크기 |
| `max_header_size` | 64 KiB | header 전체 크기 제한 |
| `request_headers_timeout` | 5s | header read 제한 시간 |
| `request_body_timeout` | 5s | body read 제한 시간 |
| `write_timeout` | 5s | response write 제한 시간 |
| `keep_alive_timeout` | 5s | keep-alive 연결에서 다음 request header를 기다리는 제한 시간 |
| `graceful_shutdown_timeout` | 5s | 종료 시 진행 중인 request가 끝나기를 기다리는 시간 |
| `max_keep_alive_requests` | 100 | connection당 request 수 제한 |

Public builder 이름은 아래처럼 고정한다.

```cpp
namespace zlink::framework {

class http_tls_options_builder_t {
public:
    http_tls_options_builder_t &certificate_file(std::string path);
    http_tls_options_builder_t &private_key_file(std::string path);
};

class http_server_options_builder_t {
public:
    http_server_options_builder_t &set_max_connections(std::size_t value);
    http_server_options_builder_t &set_max_request_body_size(std::size_t bytes);
    http_server_options_builder_t &set_max_header_size(std::size_t bytes);
    http_server_options_builder_t &set_request_headers_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_request_body_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_write_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_keep_alive_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_graceful_shutdown_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_max_keep_alive_requests(
      std::size_t value);
};

class http_options_builder_t {
public:
    http_options_builder_t &configure_tls(
      std::function<void(http_tls_options_builder_t &)> configure);

    http_options_builder_t &configure_server(
      std::function<void(http_server_options_builder_t &)> configure);
};

} // namespace zlink::framework
```

`configure_server(...)`에서 값을 바꾸지 않으면 위 기본값을 사용한다. 0이나 범위를 벗어난 값의
startup 오류는 [HTTP Hosting](60-http-hosting.ko.md)의 validation 계약을 따른다.

## 10. Observability

내장 server는 framework logging, monitoring, health와 연결되어야 한다.

logging:

- request start/end log
- status code와 duration
- route template
- correlation id
- remote endpoint
- error kind

monitoring event:

- listener started/stopped
- connection accepted/closed
- request started/completed
- request rejected
- timeout
- TLS handshake failed
- graceful shutdown started/completed

metrics:

- active connections
- total accepted connections
- rejected connections
- in-flight requests
- request duration
- response status count
- request body bytes

health:

- listener bind 실패는 startup failure다.
- started listener 수와 expected endpoint 수가 다르면 unhealthy다.
- shutdown 중 readiness는 unhealthy로 바뀐다.

## 11. 보안과 운영 기준

내장 server는 backend API server이므로 기본 보안 경계를 제공해야 한다.

- default body limit를 둔다.
- header limit를 둔다.
- timeout 없는 request read를 허용하지 않는다.
- TLS certificate/private key 설정 오류를 시작 전에 잡는다.
- error response는 stack trace나 내부 파일 경로를 노출하지 않는다.
- reverse proxy 뒤에서 쓸 수 있도록 forwarded header 정책을 별도 option으로 둔다.
- request logging에서 민감 header를 그대로 기록하지 않는다.

auth provider는 이 계약의 지원 범위가 아니다. 인증은 middleware/filter extension point와
header/context API를 사용한다.

## 12. 계약 검증

필수 회귀 테스트:

| 테스트 | 기대 |
|--------|------|
| startup validation | invalid endpoint, missing TLS file, duplicate system route 실패 |
| route mapping | `map_get/post/put/delete`가 올바른 handler를 호출 |
| not found | 없는 path는 `404` |
| method not allowed | path는 있으나 method가 다르면 `405` |
| unsupported media type | JSON route에 잘못된 content type이면 `400` |
| malformed body | JSON decode 실패는 `400` |
| body limit | limit 초과는 `413` |
| typed handler signature | DTO, DTO+context, DTO+request와 response 반환의 sync/async signature를 모두 호출한다. |
| raw request handler | `http_request_t`를 받고 `http_response_t`로 응답 |
| raw handler no serializer | raw route는 request/reply JSON serializer 없이 등록 |
| ambiguous handler signature | 모호한 handler signature는 static assertion 또는 startup validation으로 실패한다. |
| keep-alive | 같은 connection에서 두 request 처리 |
| request timeout | header/body timeout이 connection을 닫고 event 기록 |
| handler timeout | `504` response |
| middleware | success, short-circuit, exception에서 after 실행 |
| TLS | HTTPS route 성공, TLS 설정 오류 실패 |
| graceful shutdown | 새 accept 중단, active request drain |
| logging | route, status, duration, correlation id 기록 |
| metrics | request/status/connection counter 갱신 |
| zlink integration | HTTP handler에서 channel request 또는 SPOT call 사용 |

계약 검증은 설치된 public header와 package를 사용하는 consumer 관점에서 수행한다.

## 13. 적합성 기준

내장 HTTP server가 backend API framework의 기본 server로 자리 잡으려면 아래 조건을 모두 만족해야
한다.

- `options.http().listen(...).map_*<THandler>(...)` public 표면을 유지한다.
- HTTP/1.1, HTTPS, keep-alive, timeout, limit, graceful shutdown을 지원한다.
- handler는 Beast/Asio/TLS 타입을 알 필요가 없다.
- typed DTO, typed response, raw HTTP request handler signature를 모두 지원한다.
- raw HTTP request handler도 `http_request_t`와 `http_response_t`만 사용하고 Beast/Asio/TLS
  타입을 받지 않는다.
- logging, monitoring, health, DI, serializer와 같은 app model에 통합된다.
- malformed request, route 없음, method mismatch, body limit, handler failure가 status와 JSON body로
  일관되게 매핑된다.
- 샘플과 HTTP E2E는 `zlink::http_client`를 사용하는 공개 consumer 흐름으로 검증한다.
- public header에는 Boost.Beast, Boost.Asio와 OpenSSL 구현 타입이 노출되지 않는다.
