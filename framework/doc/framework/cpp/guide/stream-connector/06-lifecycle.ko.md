# 06 — 연결 생명주기

[← 패킷 수신](05-receiving.ko.md) | [목차](INDEX.ko.md) | [다음: 오류 처리 →](07-error-handling.ko.md)

---

## 상태 전이

```
created → connecting → connected → disconnected
                    ↗                     |
              reconnecting ←————————————— |
                                          ↓
                                        closed
```

| 상태 | 의미 |
|------|------|
| `created` | connector가 만들어졌지만 `connect()`를 호출하지 않은 상태 |
| `connecting` | `connect()`를 호출해 연결을 시도하는 중 |
| `connected` | 연결이 열려 있고 패킷 송수신이 가능한 상태 |
| `reconnecting` | 연결이 끊어져 재연결을 시도하는 중 |
| `disconnected` | 연결이 끊어졌고 reconnect가 비활성화되었거나 시도 횟수를 모두 소진한 상태 |
| `closed` | `close()`를 호출해 connector를 종료한 상태 |

현재 상태는 `connector.state()`로 읽는다.

## connect

```cpp
auto connected = connector.connect();
if (!connected) {
    // error_code_t::connect_timeout, error_code_t::disconnected 등
}
```

`connect()`는 연결이 완료되거나 실패할 때까지 blocking한다. callback 방식으로도 사용할 수 있다.

```cpp
connector.connect([](zlink::stream_connector::result_t<void> result) {
    if (!result) { return; }
    // 연결 성공 후 패킷 송신 시작 가능
});
```

callback `connect()`는 등록 후 즉시 반환한다. 연결을 기다리지 않는다.

## close

```cpp
auto closed = connector.close();
```

`close()`는 pending request, pending wait, received queue를 정리하고 연결을 닫는다. 대기 중인 `request().submit(callback)` callback들은 `closed` 오류로 완료된다. callback 방식도 사용할 수 있다.

```cpp
connector.close([](zlink::stream_connector::result_t<void> result) {
    // close 완료
});
```

## reconnect

`reconnect.enabled = true`(기본값)이면 연결 끊김 후 자동으로 재연결을 시도한다.

```cpp
options.reconnect.enabled        = true;
options.reconnect.initial_delay  = std::chrono::milliseconds{250};
options.reconnect.max_delay      = std::chrono::seconds{5};
options.reconnect.backoff_factor = 2.0;
options.reconnect.max_attempts   = 3;
```

재연결 중에는 상태가 `reconnecting`으로 바뀐다. 모든 시도가 실패하면 `disconnected`로 전환된다.

`reconnecting` 상태에서 `send()`, `request()`를 호출하면 `disconnected` 오류를 반환한다. 재연결이 성공한 뒤에는 다시 패킷 송수신이 가능하다.

## heartbeat

heartbeat는 연결이 살아 있는지 주기적으로 확인한다.

```cpp
options.heartbeat.enabled  = true;
options.heartbeat.interval = std::chrono::seconds{10};  // idle 시 ping 전송 간격
options.heartbeat.timeout  = std::chrono::seconds{30};  // 이 시간 동안 응답 없으면 disconnected
```

- `interval` 동안 inbound 트래픽이 없으면 `$zlink.heartbeat.ping` control frame을 전송한다.
- `timeout` 동안 inbound 트래픽이 없으면 연결을 `disconnected`로 처리한다.
- heartbeat control frame은 `on<packet_t>()` callback으로 전달되지 않는다.

## 상태 이벤트 수신

```cpp
using zsc = zlink::stream_connector;

connector.on_connection_state_changed([](const zsc::connection_state_changed_t& ev) {
    switch (ev.state) {
    case zsc::connection_state_t::connected:
        // 연결 성공, 서버에 join 패킷 전송
        break;
    case zsc::connection_state_t::reconnecting:
        // UI에 "재연결 중..." 표시
        break;
    case zsc::connection_state_t::disconnected:
        // 재연결 실패, 로비로 복귀
        break;
    default:
        break;
    }
});

connector.on_disconnected([]() {
    // transport 끊김 감지 직후 (reconnect 시도 전)
});
```

## is_connected()

패킷 송신 전 연결 상태를 빠르게 확인한다.

```cpp
if (!connector.is_connected()) {
    // send 시도하지 않음
    return;
}
```

`is_connected()`가 `true`를 반환해도 즉시 이후에 연결이 끊길 수 있다. send/request 반환값을 항상 확인해야 한다.

## lifecycle 순서 예시

```cpp
// 1. 생성
auto connector = zsc::connector_factory_t::create(options);

// 2. 이벤트 등록 (connect 전에 등록해야 초기 connected 이벤트를 받음)
connector.on_connection_state_changed(state_handler);
connector.on<server_event_t>(event_handler);

// 3. 연결
auto result = connector.connect();

// 4. 사용 (game loop)
while (running) {
    connector.dispatch();
}

// 5. 종료
connector.close();
```
