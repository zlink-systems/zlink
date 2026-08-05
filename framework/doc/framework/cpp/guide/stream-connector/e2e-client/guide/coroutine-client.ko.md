# C++ Stream E2E Coroutine Client Guide

e2e client는 서버 e2e, smoke, perf scenario를 coroutine 흐름으로 읽기 쉽게 만들기 위한 package다.
일반 engine client의 기본 API가 아니다.

## 기본 흐름

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

`async()`는 blocking `submit()`을 호출하지 않는다. core callback completion을 등록하고, 완료되면
connector delivery policy에 따라 coroutine을 다시 실행한다.

`co_await` 결과는 `result_t<T>`다. 실패를 예외로 바꾸지 않으므로 scenario code는 결과를 확인한 뒤
다음 요청을 보내야 한다.

request와 wait를 겹쳐 실행해야 하는 perf scenario는 task를 만든 뒤 `start()`로 먼저 등록할 수 있다.
예를 들어 push를 기다리는 wait task를 `start()`한 다음 request task를 실행하면, reply와 함께 들어온
push packet을 pending wait가 받을 수 있다.

## Timeout And Close

request와 wait timeout은 `request_timeout` 오류로 완료된다. connector가 닫히면 pending coroutine은
`closed` 또는 `disconnected` 결과로 완료되어야 한다. coroutine task가 operation을 취소하면
`canceled` 결과를 사용한다.

## Package Boundary

`task_t`와 `async()`는 `zlink::stream_e2e_client` target을 include하고 link할 때만 보인다.
`zlink::stream_connector` core header에는 coroutine type이 보이면 안 된다.
