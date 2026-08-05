# 08 — E2E 클라이언트

[← 오류 처리](07-error-handling.ko.md) | [목차](INDEX.ko.md) | [다음: 엔진 어댑터 →](09-engine-adapters.ko.md)

---

e2e client는 서버 e2e, smoke, perf scenario를 coroutine 흐름으로 짧게 작성하기 위한 package다. 일반 게임 엔진 client의 기본 API가 아니다.

## 설치

```cmake
find_package(zlink-stream-e2e-client CONFIG REQUIRED)

target_link_libraries(my_scenario_test PRIVATE
    zlink::stream_connector
    zlink::stream_e2e_client
)
```

`task_t`와 `async()`는 `zlink::stream_e2e_client` target을 include하고 link할 때만 보인다.

## 기본 흐름

```cpp
#include <zlink/stream_e2e_client.hpp>

namespace ze = zlink::stream_e2e_client;
namespace zsc = zlink::stream_connector;

zsc::task_t<void> run_scenario(zsc::connector_t& connector)
{
    auto client = ze::use(connector);

    auto connected = co_await client.connect().async();
    if (!connected) {
        co_return;
    }

    auto auth = co_await client
        .request(auth_request_t{"player-1", "tok-abc123"})
        .packet_name("auth.login")
        .async<auth_reply_t>();

    if (!auth) {
        co_await client.close().async();
        co_return;
    }

    auto session_id = auth.value().session_id;

    co_await client
        .send(enter_world_t{session_id, "zone-12"})
        .packet_name("world.enter")
        .async();

    co_await client.close().async();
}
```

`async()`는 blocking `submit()`을 호출하지 않는다. core callback completion을 등록하고, 완료되면 connector delivery policy에 따라 coroutine을 다시 실행한다. `co_await` 중에는 worker thread가 이 coroutine에 묶이지 않는다.

## co_await 결과

`co_await`의 결과는 항상 `result_t<T>`다. 예외를 던지지 않는다.

```cpp
auto reply = co_await client
    .request(ping_t{"player-1", seq})
    .timeout(std::chrono::seconds{3})
    .async<pong_t>();

if (!reply) {
    // reply.error_code() == zsc::error_code_t::request_timeout
    co_return;
}
```

## wait_for coroutine

```cpp
auto event = co_await client
    .wait_for<server_event_t>()
    .packet_name("server.event")
    .timeout(std::chrono::seconds{10})
    .async();
```

## request와 wait 겹쳐 실행

push를 기다리는 wait를 먼저 `start()`로 등록한 다음 request를 보내면, reply와 함께 도착하는 push packet을 wait가 받을 수 있다.

```cpp
auto wait_task = client
    .wait_for<server_push_t>()
    .packet_name("match.push")
    .timeout(std::chrono::seconds{10})
    .async();

wait_task.start(); // wait 먼저 등록

auto reply = co_await client
    .request(match_request_t{"match-7f3a"})
    .async<match_reply_t>();

auto push = co_await std::move(wait_task);
```

## timeout과 close 처리

| 상황 | error_code |
|------|-----------|
| timeout 만료 | `request_timeout` error code |
| connector close | `closed` |
| transport 끊김 | `disconnected` |
| coroutine task 파괴 | `canceled` |

pending coroutine이 있는 상태에서 `close()`를 호출하면 해당 coroutine들은 `closed`로 완료된다.

## immediate dispatch mode

e2e client와 perf client에서는 immediate mode를 사용하면 `dispatch()`를 직접 호출하지 않아도 된다.

```cpp
options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
```

## perf scenario 패턴

5,000개 connector를 적은 수의 worker thread로 운용하는 패턴:

```cpp
namespace ze = zlink::stream_e2e_client;
namespace zsc = zlink::stream_connector;

zsc::task_t<void> perf_client_loop(zsc::connector_t& connector, int client_id)
{
    auto client = ze::use(connector);

    auto connected = co_await client.connect().async();
    if (!connected) { co_return; }

    int seq = 0;
    while (running) {
        auto reply = co_await client
            .request(ping_t{client_id, ++seq})
            .timeout(std::chrono::seconds{1})
            .async<pong_t>();

        record(reply);
    }

    co_await client.close().async();
}
```

`co_await` 중에는 worker thread가 이 coroutine에 묶이지 않으므로 수천 개의 concurrent coroutine을 소수의 thread로 처리할 수 있다.

자세한 내용은 [11 — 성능 테스트](11-performance.ko.md)를 참고한다.
