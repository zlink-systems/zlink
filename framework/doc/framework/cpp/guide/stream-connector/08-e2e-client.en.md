# 08 — E2E Client

[← Error Handling](07-error-handling.en.md) | [Table Of Contents](INDEX.en.md) | [Next: Engine Adapters →](09-engine-adapters.en.md)

---

The e2e client is a package for writing server e2e, smoke, and perf scenarios concisely as a
coroutine flow. It's not the default API for a general game engine client.

## Installation

```cmake
find_package(zlink-stream-e2e-client CONFIG REQUIRED)

target_link_libraries(my_scenario_test PRIVATE
    zlink::stream_connector
    zlink::stream_e2e_client
)
```

`task_t` and `async()` are visible only when the `zlink::stream_e2e_client` target is included and
linked.

## Basic Flow

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

`async()` doesn't call the blocking `submit()`. It registers a core callback completion, and once
that completes, resumes the coroutine according to the connector's delivery policy. While
`co_await`ing, the worker thread isn't tied to this coroutine.

## The co_await Result

The result of `co_await` is always a `result_t<T>`. No exception is thrown.

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

## wait_for Coroutine

```cpp
auto event = co_await client
    .wait_for<server_event_t>()
    .packet_name("server.event")
    .timeout(std::chrono::seconds{10})
    .async();
```

## Overlapping A Request And A Wait

If you register a wait for a push with `start()` first, then send the request, the wait can catch a
push packet arriving together with the reply.

```cpp
auto wait_task = client
    .wait_for<server_push_t>()
    .packet_name("match.push")
    .timeout(std::chrono::seconds{10})
    .async();

wait_task.start(); // register the wait first

auto reply = co_await client
    .request(match_request_t{"match-7f3a"})
    .async<match_reply_t>();

auto push = co_await std::move(wait_task);
```

## Handling Timeout And Close

| Situation | error_code |
|------|-----------|
| Timeout expired | `request_timeout` error code |
| Connector close | `closed` |
| Transport dropped | `disconnected` |
| Coroutine task destroyed | `canceled` |

If `close()` is called while pending coroutines exist, those coroutines complete with `closed`.

## Immediate Dispatch Mode

In the e2e client and perf client, using immediate mode means you don't need to call `dispatch()`
directly.

```cpp
options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
```

## The Perf Scenario Pattern

A pattern for running 5,000 connectors with a small number of worker threads:

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

Since the worker thread isn't tied to this coroutine while `co_await`ing, thousands of concurrent
coroutines can be processed by a small number of threads.

See [11 — Performance Testing](11-performance.en.md) for details.
