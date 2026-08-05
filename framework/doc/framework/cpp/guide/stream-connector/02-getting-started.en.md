# 02 — Getting Started

[← Overview](01-overview.en.md) | [Table Of Contents](INDEX.en.md) | [Next: Connector Options →](03-connector-options.en.md)

---

## Installation

### vcpkg

```bash
vcpkg install zlink-stream-connector
```

To install TLS and WebSocket together, specify the features.

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

## CMake Integration

```cmake
find_package(zlink-stream-connector CONFIG REQUIRED)

target_link_libraries(my_game PRIVATE zlink::stream_connector)
```

If you're also using the e2e client:

```cmake
find_package(zlink-stream-e2e-client CONFIG REQUIRED)

target_link_libraries(my_scenario_test PRIVATE
    zlink::stream_connector
    zlink::stream_e2e_client
)
```

## First Connection

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
        // Check the failure reason with connected.error_code()
        return 1;
    }

    // Send/receive packets after the connection succeeds
    connector.close();
    return 0;
}
```

## send — One-Way Sending

```cpp
struct chat_message_t {
    std::string room_id;
    std::string text;
};

connector.send(chat_message_t{"room-42", "안녕하세요"})
    .packet_name("chat.send")
    .submit();
```

`submit()` returns a `result_t<void>`. A callback style can also be used.

```cpp
connector.send(chat_message_t{"room-42", "안녕하세요"})
    .packet_name("chat.send")
    .submit([](zsc::result_t<void> result) {
        if (!result) {
            // result.error_code()
        }
    });
```

## request — Request/Reply

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
    // reply.error_code() == zsc::error_code_t::request_timeout, etc.
    return;
}

auto session = reply.value().session_id;
```

## Receiving A Push — on()

A push packet the server sends is received through the callback registered with `on<T>()`. In
manual dispatch mode, the callback runs when `dispatch()` is called.

```cpp
connector.on<chat_pushed_t>([](const chat_pushed_t& msg) {
    // msg.room_id, msg.text
});

// In the game loop
while (running) {
    connector.dispatch();
    // ...
}
```

## Next Steps

- Callback thread rules and dispatch mode → [05 — Receiving Packets](05-receiving.en.md)
- Connection options (heartbeat, reconnect, TLS settings) → [03 — Connector Options](03-connector-options.en.md)
- The e2e client coroutine flow → [08 — E2E Client](08-e2e-client.en.md)
