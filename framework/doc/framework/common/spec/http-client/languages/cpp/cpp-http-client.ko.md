<!-- framework-adapter-nav:start -->
[문서 목록](../../../../../../README.ko.md) | [다음: Spec -- ZLink Framework C++ HTTP Hosting](../../../server/languages/cpp/60-http-hosting.ko.md)
<!-- framework-adapter-nav:end -->

[스펙 목차](../../../../README.ko.md)

[C++ 묶음](../../../../../cpp/README.ko.md) | [Runtime Architecture](../../../../internals/README.ko.md) | [Application Framework](../../../server/languages/cpp/01-system-structure.ko.md) | [Framework 인터페이스](../../../server/languages/cpp/interfaces/README.ko.md) | [HTTP Hosting](../../../server/languages/cpp/60-http-hosting.ko.md)

# Spec -- ZLink HTTP Client For C++

> 사용법 중심 문서는 [사용자 가이드](../../../../../cpp/guide/http-client/README.ko.md)를 본다.
> **언어 중립 공통 계약은 [공통 spec](../../README.ko.md)이 정본**이며,
> 이 문서는 공통 계약에 대한 C++ 고유 편차(coroutine 실행 계약, `delete_`,
> `result_t` 봉투, OpenSSL 선택 빌드, 자체 connection pool)와 구현 상세를 기술한다.
> 실제 계약의 단일 기준은 공통 spec + `http-client/include/zlink/http_client/**`
> public header와 `test_cpp_http_client`, `test_cpp_framework_contract_headers`
> 회귀 테스트다.

## 1. 목적

`zlink::http_client`는 C++에서 HTTP request를 보내기 위한 별도 client-side 산출물이다.
JSON 전용 client가 아니라 일반 HTTP client이며 zlink의 call object와 fluent builder
스타일로 낮은 수준 타입과 설정의 복잡성을 흡수한다. typed JSON 경로
(`body(dto)`/`submit<T>()`/`fetch<T>()`)는 그 위에 얹은 편의 계층이다.

이 client는 framework HTTP hosting을 검증하는 소비자다. `zlink::framework` core target의
기본 의존성이 아니며, framework public header가 이 client를 include하지 않는다.

## 2. 산출물 경계

public contract와 runtime 구현은 아래처럼 나눈다.

| 역할 | 위치 | 공개 여부 |
|------|------|-----------|
| facade header | `http-client/include/zlink/http_client.hpp` | public |
| contract header | `http-client/include/zlink/http_client/contracts/*` | public |
| runtime 구현 | `http-client/src/runtime/*` | private |
| 회귀 테스트 | `http-client/tests/*` | private |
| CMake target | `zlink::http_client` | public target |

public header에는 `Boost.Beast`, `Boost.Asio`, OpenSSL, socket, resolver, request parser,
response parser, SSL stream, SSL context 타입을 노출하지 않는다.

현재 구현된 public 산출물은 아래와 같다.

- `zlink/http_client.hpp`
- `zlink/http_client/contracts/client.hpp`
- `zlink::http_client` CMake target
- `client_t::create(base_url)` 또는 `create().base_url(...)` +
  `.timeout(...).default_header(...).max_response_body_size(...)`
  `.trust_certificate_file(...)`
  `.follow_redirects(...).retry(...).cookies().proxy(...).compression().build()`
- coroutine 실행 설정: `.coroutines()`, `.coroutines(resume_scheduler)`,
  `.coroutines(execute_scheduler, resume_scheduler)`
- server/framework queue adapter: `framework_resume_scheduler_t`
- 인증: `basic_auth(user, password)`, `bearer_token(token)`,
  `proxy_basic_auth(user, password)`, mTLS `client_certificate_file(cert, key)`
- 단발 request용 `build()` 생략 shortcut: `client_t::create(url).post(...)`
- `get`, `post`, `put`, `delete_`, `patch`, `head`, `options` request builder
- `query(name, value)`: percent-encoding된 query 파라미터
- request 단위 `timeout(duration)` override
- body 소스(상호 배타): typed JSON `body(dto)`, raw `body(content, content_type)`,
  chunked streaming `body_stream(provider, content_type)`,
  `form(name, value)`(x-www-form-urlencoded), `multipart(...)`/`multipart_file(...)`
- `submit<T>()`, `submit_raw()`, blocking `fetch<T>()`(typed body 직접 반환, 실패 시 throw)
- server request builder의 one-way `submit()`은 `task_t<void>`를 반환한다. 이 task는 비동기
  완료와 실패만 전달하며 전송 결과나 admission status를 포함하지 않는다.
- `download(sink)`: 응답 body를 버퍼링 없이 chunk 단위로 streaming
- connection keep-alive pool: 같은 origin(+proxy) 연결을 재사용하고 죽은 pooled 연결은
  fresh 연결로 1회 자동 재시도한다

## 3. Public API Shape

기본 사용 흐름은 아래 형태다.

```cpp
auto client = zlink::http_client::client_t::create()
  .base_url(topology.api_http_endpoint)
  .timeout(std::chrono::seconds(3))
  .build();

auto created = co_await client
  .post("/games")
  .body(create_game_http_req_t { .game_name = game_name })
  .submit<create_game_http_res_t>();
```

typed submit은 내부에서 raw submit 결과를 `.result()`로 기다리지 않고 `task_t` 완료를
await한다. 이 규칙은 샘플 handler가 HTTP client를 사용할 때 runtime thread를 막지 않도록
하기 위한 것이다.

같은 client를 여러 request에 재사용할 때는 위처럼 `build()`로 client를 한 번 만들어
보관한다. `build()`가 runtime(connection pool)을 생성하는 지점이기 때문이다.

request가 한 번뿐인 경우에는 `build()`를 생략하고 builder에서 곧바로
`get`/`post`/`put`/`delete_`를 호출할 수 있다.

```cpp
auto created = zlink::http_client::client_t::create()
  .base_url(topology.api_http_endpoint)
  .post("/games")
  .body(create_game_http_req_t { .game_name = game_name })
  .submit<create_game_http_res_t>()
  .result();
```

이 shortcut은 builder가 임시 객체여도 안전하다. `request_builder_t`가 client를 (raw
pointer가 아니라) 값으로 보유하므로, on-demand로 만든 client와 그 runtime이 request가
끝날 때까지 유지된다.

`submit<T>()`의 결과는 `result_t<http_response_t<T>>`다. 즉 성공/실패 래퍼와 HTTP 봉투
(`status`/`headers`/`body`)를 거쳐 `.value().body`로 typed DTO에 닿는다. typed body만
바로 필요한 경우에는 `fetch<T>()`를 쓴다. `fetch<T>()`는 result와 봉투를 풀어 DTO를 직접
반환하고 실패는 예외로 던진다.

```cpp
auto created = zlink::http_client::client_t::create(topology.api_http_endpoint)
  .post("/games")
  .body(create_game_http_req_t { .game_name = game_name })
  .fetch<create_game_http_res_t>();   // create_game_http_res_t (실패 시 예외)
```

`fetch<T>()`는 결과를 blocking으로 기다린다. 따라서 테스트와 client 시나리오처럼 blocking이
허용되는 곳에서 쓴다. runtime/handler 코드는 runtime thread를 막지 않도록 `submit<T>()`를
`co_await`한다.

일반 HTTP client 기능은 아래 범위를 지원한다.

- `GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `HEAD`, `OPTIONS`
- query 파라미터 (`query(name, value)`, percent-encoding 자동)
- request body: typed JSON DTO, raw(임의 content-type), chunked streaming
  (`body_stream`), form-urlencoded, multipart/form-data
  (한 request에 body 소스는 하나만 허용)
- 응답: raw(`submit_raw`), typed JSON(`submit<T>`/`fetch<T>`), streaming(`download(sink)`)
- timeout(client 기본 + request 단위 override), default header, request header
- 인증: HTTP Basic(`basic_auth`), Bearer(`bearer_token`),
  proxy Basic(`proxy_basic_auth`), mTLS client certificate(`client_certificate_file`)
- connection keep-alive pool: 같은 origin(+proxy)의 idle 연결을 재사용한다. 서버가
  연결을 닫았으면(stale) fresh 연결로 1회 자동 재시도한다. `body_stream` request는
  provider를 되감을 수 없으므로 항상 fresh 연결을 쓴다.
- coroutine scheduler: 설정하지 않은 client는 기존 blocking submit 의미를 유지한다.
  `.coroutines()`를 명시하면 HTTP 작업을 내부 scheduler에 등록하고 custom scheduler를
  주입하면 HTTP 실행 위치와 continuation resume 위치를 분리할 수 있다.
- redirect 자동 추적: `follow_redirects(max)`. `301/302`의 `POST`와 `303`은 `GET`으로
  바뀌고 body를 버린다. `307/308`은 method와 body를 보존한다. 절대 URL과 절대 경로
  Location을 지원하며 한도를 넘으면 `protocol_error`로 닫힌다. 최초 요청 origin과 다른
  redirect hop에는 `Authorization` 헤더를 다시 보내지 않는다. 다른 이름의 비밀 헤더는
  일반 헤더와 구분할 수 없으므로 caller가 직접 관리한다.
- retry: `retry(attempts)`. 설정된 operation 안에서 transport 실패(연결 끊김, timeout)만 재시도하고
  HTTP status 실패는 재시도하지 않는다. 되감을 수 없는 `body_stream`/`download`
  request는 자동 retry에서 제외한다.
- cookie jar: `cookies()`. `Set-Cookie`의 `Path`, `Secure`, `Max-Age`(0 이하 = 삭제)를
  반영하는 in-memory jar. `Domain`, `Expires` 등 나머지 속성은 무시한다.
  host당 최대 128개(공통 계약)이며 초과 시 가장 오래된 것부터 제거한다.
- proxy: `proxy("http://host:port")`. http target은 absolute-form으로 전달하고,
  https target은 `CONNECT` tunnel을 연 뒤 TLS handshake를 수행한다.
  `proxy_basic_auth`는 absolute-form request와 `CONNECT`에 `Proxy-Authorization`을 싣는다.
- 압축: `compression()`. `Accept-Encoding: gzip, deflate`를 보내고 gzip/deflate 응답
  body를 투명하게 해제한다(Boost.Beast zlib 사용, trailer checksum은 검증하지 않는다).
  `download(sink)` streaming 경로에는 적용되지 않는다.
- 응답 body 상한: `max_response_body_size(bytes)`. 기본값은 16 MiB이며 buffered 응답과
  `download(sink)` streaming 응답 모두 같은 상한을 적용한다.
- HTTP와 HTTPS endpoint, TLS server certificate verification, hostname verification,
  test certificate trust option
- HTTP status mapping

HTTP/2와 caller cancellation 공통 모델은 현재 구현 범위 밖이다. HTTP/2는 Boost.Beast가
지원하지 않고 cancellation은 server runtime마다 의미가 달라 별도 설계가 필요하다.

`base_url(...)`, `timeout(...)`, `max_response_body_size(...)`, `default_header(...)`,
`trust_certificate_file(...)`, `follow_redirects(...)`, `retry(...)`, `proxy(...)`,
request path, request header name, query/form/multipart field name은 call을 보내기 전에
검증한다. URL scheme이 `http://` 또는 `https://`가 아니거나 timeout 또는 응답 body 상한이
0 이하인 경우, 또는 이름이 비어 있는 경우에는
`framework_error_kind_t::protocol_error`로 설정 오류를 알린다. 이 오류는 transport
실패를 나타내는 `unavailable`로 바꾸지 않는다.

## 4. JSON 계약

JSON 변환은 `message_t` 또는 DTO serializer hook으로 처리한다. 샘플 application code가
`nlohmann::json::parse`로 field를 직접 꺼내지 않는다.

typed JSON 요청/응답은 아래 흐름으로 작성한다.

| 동작 | C++ 흐름 |
|------|----------|
| JSON 요청 + typed 응답 | `client.post(path).body(dto).submit<TReply>()` |
| 응답 JSON decode | `message_t::parse_json<T>()` 기반 response decode |

## 5. HTTPS와 TLS

`base_url(...)`은 `http://`와 `https://` endpoint를 모두 받는다.

`https://` request는 아래 항목을 수행한다.

- TLS handshake
- server certificate verification
- hostname verification

test certificate나 local development certificate를 trust하는 설정은 HTTP client option으로
명시해야 한다. 묵시적으로 TLS verification을 끄지 않는다.

## 6. Coroutine 실행 계약

`submit_raw()`와 `submit<T>()`는 `zlink::framework::task_t`를 반환한다. coroutine 설정이
없는 client는 기존 blocking submit 의미를 유지한다. 호출 중 HTTP 작업을 동기로 실행하고,
caller가 반환된 task에 `.result()`를 호출하면 현재 스레드는 결과가 올 때까지 기다린다.

`.coroutines()`를 명시한 client는 HTTP 작업을 HTTP client 내부 scheduler에 등록한다. 이
scheduler는 public header에 Boost.Asio, Boost.Beast, OpenSSL runtime 타입을 드러내지
않는다. HTTP client는 process 안에서 공유되는 기본 scheduler를 사용하며 public shutdown
API를 제공하지 않는다.

server runtime처럼 continuation을 다시 실행할 위치를 직접 정해야 하는 경우에는 resume
scheduler를 주입한다. HTTP 작업은 내부 scheduler가 실행하고 완료된 coroutine과 callback은
caller가 제공한 resume scheduler에서 이어진다.

```cpp
auto resume_scheduler =
  std::make_shared<zlink::http_client::framework_resume_scheduler_t>(
    [] (std::function<void ()> continuation) {
      server_queue.post(std::move(continuation));
    });

auto client = zlink::http_client::client_t::create("https://matchmaking.internal")
  .coroutines(resume_scheduler)
  .build();
```

HTTP 작업 실행 위치와 resume 위치를 모두 caller가 정해야 하면
`.coroutines(execute_scheduler, resume_scheduler)`를 쓴다.

| client 설정 | `submit<T>()` 실행 의미 |
|-------------|-------------------------|
| coroutine 설정 없음 | 호출 중 HTTP 작업을 동기 실행한다 |
| `.coroutines()` | 내부 scheduler가 HTTP 작업과 resume을 모두 처리한다 |
| `.coroutines(resume)` | 내부 scheduler가 HTTP 작업을 실행하고 custom scheduler가 resume한다 |
| `.coroutines(execute, resume)` | custom scheduler들이 HTTP 작업과 resume을 처리한다 |

`coroutine_execute_scheduler_t::execute(...)`는 HTTP 작업을 실행한다.
`coroutine_resume_scheduler_t::resume(...)`은 완료된 continuation을 다시 실행한다.
scheduler 인자가 `nullptr`이면 `invalid_operation`으로 실패한다.

coroutine 설정이 있는 client에서 `submit_raw()` 또는 `submit<T>()`를 호출하면 request
builder가 method, path, headers, body provider, timeout 같은 request state를 값으로
복사한 뒤 scheduler에 작업을 등록한다. 임시 builder와 client 객체가 사라져도 등록된
작업은 자신이 가진 request state와 runtime shared ownership으로 완료되어야 한다.

request timeout은 scheduler queue 등록 시점을 시작점으로 둔다. worker가 HTTP 작업을
시작하기 전에 deadline이 지났으면 HTTP 교환을 시작하지 않고 timeout 실패로 task를
완료한다.

`submit<T>()`는 raw HTTP 작업이 끝난 뒤 typed JSON decode를 수행한다. coroutine client에서는
decode와 caller continuation이 resume scheduler 정책을 따른다. `submit<T>(callback)`도
coroutine 설정이 있으면 resume scheduler가 정한 위치에서 callback을 실행한다. callback에서
예외가 나도 이미 완료된 task 결과를 바꾸지 않는다.

`body_stream(provider)`의 provider와 `download(sink)`의 sink는 HTTP 작업을 실행하는
execute scheduler worker에서 호출된다. 이 callback 안에서 server handler state를 직접
건드리지 말고 필요한 경우 thread-safe queue나 server scheduler post를 사용한다.

## 7. HTTP Hosting 테스트에서의 사용

HTTP handler e2e 테스트는 외부 HTTP 도구나 sample-local client가 아니라
`zlink::http_client`로 `GET`, `POST`, `PUT`, `DELETE` route를 호출한다.

이 규칙은 두 가지를 보장하기 위한 것이다.

- HTTP hosting handler가 실제 public client로 검증된다.
- 샘플과 테스트가 서로 다른 HTTP wrapper를 갖지 않는다.

## 8. 회귀 테스트

최소 테스트는 아래 축으로 둔다.

- contract header compile: `#include <zlink/http_client.hpp>`
- public header boundary: runtime 구현 header와 Beast/Asio/OpenSSL 타입이 public header에
  드러나지 않는다
- JSON request/response: typed DTO request를 JSON으로 보내고 reply DTO를 읽는다
- coroutine submit: `co_await submit<T>()`가 typed response를 반환하고 내부 raw submit을
  blocking wait로 기다리지 않는다
- coroutine scheduler: `.coroutines()` 기본 scheduler, custom resume scheduler,
  custom execute/resume scheduler, framework queue adapter, scheduler 등록 실패,
  queue timeout을 검증한다
- streaming callback 위치: coroutine client의 `body_stream(provider)`와 `download(sink)`는
  execute scheduler worker에서 호출된다
- build 생략 shortcut: builder가 임시여도 `build()` 없이 `post(...)` 등으로 보낸 request가
  use-after-free 없이 완료된다
- typed body fetch: `fetch<T>()`가 typed DTO를 직접 반환하고 실패 status를 예외로 던진다
- method coverage: `PATCH`, `OPTIONS`가 전달되고 `HEAD`는 body 없이 status/header를 받는다
- query encoding: `query(...)`가 percent-encoding된 query string으로 전달된다
- body 소스: raw content-type, form-urlencoded, multipart 인코딩이 wire에 그대로 실리고,
  복수 body 소스는 `protocol_error`로 거부된다
- redirect: 추적 on/off, 절대 URL Location, `POST`→`GET` 변환, redirect 한도 초과 실패,
  교차 origin `Authorization` 제거
- retry: 응답 없이 끊긴 연결이 재시도로 복구된다
- cookie: `Set-Cookie`가 jar에 저장되어 후속 request에 실리고 `Path` scope를 벗어나면
  보내지 않는다
- proxy: http absolute-form 전달과 https `CONNECT` tunnel이 origin까지 도달한다
- proxy auth: 인증 없는 request는 `407`로 거부되고 `proxy_basic_auth`로 통과한다
- 압축: `compression()`이 gzip/deflate 응답을 평문으로 해제하고 `Content-Encoding`
  헤더를 제거한다
- streaming download: `download(sink)`가 body를 chunk로 전달하고 redirect 중간 응답
  body는 sink로 새지 않는다
- response body limit: buffered 응답과 `download(sink)` 응답이 설정한 body 상한을 넘으면
  실패한다
- streaming upload: `body_stream(provider)`가 chunked transfer-encoding으로 전달된다
- 인증: `basic_auth`/`bearer_token`이 `Authorization` 헤더로 실리고 mTLS 서버는
  `client_certificate_file` 설정 시에만 handshake가 성공한다
- keep-alive: 같은 client의 연속 request가 단일 connection을 재사용한다
- request timeout override: request 단위 `timeout(...)`이 client 기본값을 덮어쓴다
- HTTP status mapping: `400`, `404`, `500` 응답이 client result/error kind로 고정된다
- timeout: 응답 지연은 timeout error로 닫힌다
- fluent input validation: 잘못된 base URL, path, header name, timeout은
  `protocol_error`로 닫힌다
- HTTPS success: test certificate trust 설정이 있으면 `https://` JSON request/response가
  성공한다
- TLS failure: 신뢰하지 않은 certificate와 hostname mismatch는 명시적인 client error로
  실패한다

현재 회귀 테스트는 `test_cpp_http_client`와 `test_cpp_framework_contract_headers`가 담당한다.
OpenSSL을 찾은 빌드에서는 configure 단계에서 테스트 인증서를 생성하고
`test_cpp_http_client` 안에서 HTTPS success, untrusted certificate failure, hostname
mismatch failure를 함께 검증한다.

검증 label은 아래와 같다.

```bash
ctest --test-dir framework/languages/cpp/build -L http-client-contract
ctest --test-dir framework/languages/cpp/build -L http-client-unit
ctest --test-dir framework/languages/cpp/build -L http-client-e2e
ctest --test-dir framework/languages/cpp/build -L http-client-https
ctest --test-dir framework/languages/cpp/build -L http-client-regression
```

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../../../../../../README.ko.md) | [다음: Spec -- ZLink Framework C++ HTTP Hosting](../../../server/languages/cpp/60-http-hosting.ko.md)
<!-- framework-adapter-nav:bottom:end -->
