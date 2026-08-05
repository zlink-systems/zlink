# 02 — 시작하기

[← 개요](01-overview.ko.md) | [목차](INDEX.ko.md) | [다음: Connector 옵션 →](03-connector-options.ko.md)

---

## 설치

### vcpkg

```bash
vcpkg install zlink-stream-connector
```

TLS와 WebSocket을 함께 설치하려면 feature를 지정한다.

```bash
vcpkg install "zlink-stream-connector[tls,websocket]"
```

### Conan

```bash
conan install --requires "zlink-stream-connector/0.1.0" \
  -o "zlink-stream-connector/*:with_tls=True" \
  -o "zlink-stream-connector/*:with_websocket=True"
```

### CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(zlink_stream_connector
    GIT_REPOSITORY https://github.com/ulala-x/zlink.git
    GIT_TAG        main
    SOURCE_SUBDIR  framework/languages/cpp/connector/core
)
FetchContent_MakeAvailable(zlink_stream_connector)
```

## CMake 연결

```cmake
find_package(zlink-stream-connector CONFIG REQUIRED)

target_link_libraries(my_game PRIVATE zlink::stream_connector)
```

e2e client도 함께 사용한다면:

```cmake
find_package(zlink-stream-e2e-client CONFIG REQUIRED)

target_link_libraries(my_scenario_test PRIVATE
    zlink::stream_connector
    zlink::stream_e2e_client
)
```

## 첫 연결

```cpp
#include <zlink/stream_connector.hpp>

namespace zsc = zlink::stream_connector;

int main()
{
    zsc::connector_options_t options;
    options.endpoint = "tcp://game.example.com:7000";

    auto connector = zsc::connector_factory_t::create(options);

    auto connected = connector.connect();
    if (!connected) {
        // connected.error_code()로 실패 원인 확인
        return 1;
    }

    // 연결 성공 후 패킷 송수신
    connector.close();
    return 0;
}
```

## send — 단방향 송신

```cpp
struct chat_message_t {
    std::string room_id;
    std::string text;
};

connector.send(chat_message_t{"room-42", "안녕하세요"})
    .packet_name("chat.send")
    .submit();
```

`submit()`은 `result_t<void>`를 반환한다. callback 방식도 사용할 수 있다.

```cpp
connector.send(chat_message_t{"room-42", "안녕하세요"})
    .packet_name("chat.send")
    .submit([](zsc::result_t<void> result) {
        if (!result) {
            // result.error_code()
        }
    });
```

## request — 요청/응답

```cpp
struct login_request_t {
    std::string player_id;
    std::string token;
};

struct login_reply_t {
    int64_t session_id;
    std::string server_time;
};

auto reply = connector
    .request(login_request_t{"player-1", "tok-abc123"})
    .packet_name("auth.login")
    .submit<login_reply_t>();

if (!reply) {
    // reply.error_code() == zsc::error_code_t::request_timeout 등
    return;
}

auto session = reply.value().session_id;
```

## push 수신 — on()

서버가 보내는 push packet은 `on<T>()`으로 등록한 callback으로 받는다. manual dispatch mode에서는 `dispatch()`를 호출할 때 callback이 실행된다.

```cpp
connector.on<chat_pushed_t>([](const chat_pushed_t& msg) {
    // msg.room_id, msg.text
});

// game loop에서
while (running) {
    connector.dispatch();
    // ...
}
```

## 다음 단계

- callback thread 규칙과 dispatch mode → [05 — 패킷 수신](05-receiving.ko.md)
- 연결 옵션 (heartbeat, reconnect, TLS 설정) → [03 — Connector 옵션](03-connector-options.ko.md)
- e2e client coroutine 흐름 → [08 — E2E 클라이언트](08-e2e-client.ko.md)
