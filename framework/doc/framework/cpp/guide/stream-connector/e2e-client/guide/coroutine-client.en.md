# C++ Stream E2E Coroutine Client Guide

The e2e client is a package for making server e2e, smoke, and perf scenarios readable as a
coroutine flow. It's not the default API for a general engine client.

## Basic Flow

```cpp
auto client = zlink::stream_e2e_client::use(connector);

auto connected = co_await client.connect().async();
if (!connected) {
    co_return;
}

auto reply = co_await client
  .request(ping_t{client_id, sequence})
  .timeout(1s)
  .async<pong_t>();
```

`async()` doesn't call the blocking `submit()`. It registers a core callback completion, and once
that completes, resumes the coroutine according to the connector's delivery policy.

The `co_await` result is a `result_t<T>`. Since a failure isn't turned into an exception, scenario
code must check the result before sending the next request.

A perf scenario that needs to overlap a request and a wait can build a task and register it first
with `start()`. For example, if you `start()` a wait task waiting for a push, then run a request
task, the pending wait can catch a push packet that arrives together with the reply.

## Timeout And Close

A request or wait timeout completes with a `request_timeout` error. If the connector closes, a
pending coroutine must complete with either a `closed` or `disconnected` result. If a coroutine task
cancels the operation, a `canceled` result is used.

## Package Boundary

`task_t` and `async()` are visible only when the `zlink::stream_e2e_client` target is included and
linked. No coroutine type should be visible in the `zlink::stream_connector` core header.
