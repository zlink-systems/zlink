# 05 — 패킷 수신

[← 패킷 송신](04-sending.ko.md) | [목차](INDEX.ko.md) | [다음: 연결 생명주기 →](06-lifecycle.ko.md)

---

## on\<T\>() — push callback 등록

서버가 보내는 push packet을 callback으로 받는다.

```cpp
struct leaderboard_update_t {
    int32_t rank;
    std::string player_id;
    int64_t score;
};

connector.on<leaderboard_update_t>([](const leaderboard_update_t& update) {
    // update.rank, update.player_id, update.score
});
```

packet name을 명시할 수 있다.

```cpp
connector.on<leaderboard_update_t>(
    "leaderboard.weekly",
    [](const leaderboard_update_t& update) {
        // 주간 리더보드만 처리
    });
```

`on()`은 connector 소유 callback 목록에 추가한다. connector가 닫히면 callback도 제거된다. 현재는 이미 등록한 callback을 개별 해제하는 API를 제공하지 않는다.

## dispatch mode

`on<T>()` callback이 실행되는 시점은 `dispatch_mode`가 결정한다.

### manual mode (기본값)

callback은 `dispatch()`를 호출한 thread에서 실행된다. 게임 엔진의 frame loop와 맞출 때 사용한다.

```cpp
// connector_options_t::dispatch_mode = dispatch_mode_t::manual (기본)

// game loop
while (running) {
    connector.dispatch(); // 대기 중인 push packet callback 실행
    update_game_state();
    render();
}
```

`dispatch()`는 호출 시점에 대기 중인 callback을 처리하고 반환한다. 새로 도착하는 packet을 기다리지 않는다.

### immediate mode

callback은 connector receive path에서 즉시 실행된다. `dispatch()`를 호출하지 않아도 된다. CLI, tool, e2e client에 적합하다.

```cpp
options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
```

immediate mode에서도 callback은 connector 내부 lock 밖에서 실행된다. callback 안에서 `connector.send()`를 안전하게 호출할 수 있다.

## wait_for\<T\>() — 특정 패킷 대기

callback을 등록하지 않고 특정 packet을 한 번 기다린다. matching된 packet은 소비되어 이후 `on<T>()` callback으로 전달되지 않는다.

```cpp
struct server_ready_t {
    std::string server_id;
    int32_t player_capacity;
};

auto ready = connector
    .wait_for<server_ready_t>()
    .packet_name("server.ready")
    .timeout(std::chrono::seconds{10})
    .submit();

if (!ready) {
    // ready.error_code() == error_code_t::request_timeout 등
    return;
}

auto capacity = ready.value().player_capacity;
```

timeout을 생략하면 `connector_options_t::wait_timeout` 값을 사용한다.

### where() — 조건 필터

특정 조건에 맞는 packet만 소비한다. 조건에 맞지 않는 packet은 소비되지 않고 queue에 남아 이후 wait나 dispatch에서 처리된다.

```cpp
auto my_match = connector
    .wait_for<match_found_t>()
    .where([](const match_found_t& msg) {
        return msg.match_id == "match-7f3a";
    })
    .timeout(std::chrono::seconds{30})
    .submit();
```

단일 필드가 특정 값과 같은지만 보면 member pointer overload를 쓸 수 있다.
이 방식은 C++ 람다의 매개변수 선언을 반복하지 않아도 된다.

```cpp
auto my_match = connector
    .wait_for<match_found_t>()
    .where(&match_found_t::match_id, std::string("match-7f3a"))
    .timeout(std::chrono::seconds{30})
    .submit();
```

### wait_for callback 방식

```cpp
connector
    .wait_for<server_ready_t>()
    .packet_name("server.ready")
    .submit([](zlink::stream_connector::result_t<server_ready_t> result) {
        if (!result) { return; }
        // result.value()
    });
```

## 연결 상태 이벤트

```cpp
connector.on_connection_state_changed([](const zlink::stream_connector::connection_state_changed_t& ev) {
    // ev.state: created, connecting, connected, reconnecting, disconnected, closed
});

connector.on_disconnected([]() {
    // 연결이 끊김 (reconnect 시도 전)
});

connector.on_error([](const zlink::stream_connector::error_t& err) {
    // err.code, err.message
});
```

## callback 안에서 send/request 호출

callback 실행 중에 같은 connector의 `send()`, `request()`를 호출할 수 있다. 구현은 connector 내부 lock을 잡은 채로 user callback을 실행하지 않는다.

```cpp
connector.on<pvp_invite_t>([&connector](const pvp_invite_t& invite) {
    // callback 안에서 안전하게 send 가능
    connector
        .send(pvp_accept_t{invite.match_id, "player-1"})
        .packet_name("pvp.accept")
        .submit();
});
```

## pending_dispatch_count()

manual mode에서 대기 중인 callback 수를 확인한다.

```cpp
auto pending = connector.pending_dispatch_count();
```
