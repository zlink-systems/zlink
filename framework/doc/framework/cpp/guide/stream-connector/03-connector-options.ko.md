# 03 — Connector 옵션

[← 시작하기](02-getting-started.ko.md) | [목차](INDEX.ko.md) | [다음: 패킷 송신 →](04-sending.ko.md)

---

`connector_options_t`는 connector를 만들 때 전달하는 설정 구조체다.

```cpp
zlink::stream_connector::connector_options_t options;
auto connector = zlink::stream_connector::connector_factory_t::create(options);
```

## endpoint

```cpp
options.endpoint = "tcp://game.example.com:7000";
options.endpoint = "wss://game.example.com:443/stream";
```

scheme이 transport를 결정한다. 지원 scheme은 `tcp`, `tls`, `ws`, `wss`다.

| scheme | transport | 필요 build feature |
|--------|-----------|--------------------|
| `tcp://host:port` | TCP | 항상 포함 |
| `tls://host:port` | TLS over TCP | `WITH_TLS` |
| `ws://host:port/path` | WebSocket | `WITH_WEBSOCKET` |
| `wss://host:port/path` | WebSocket over TLS | `WITH_WEBSOCKET` + `WITH_TLS` |

build에 없는 transport를 사용하면 `connect()`가 `unsupported_codec` 오류를 반환하지 않고, 현재는 `configuration_error`를 반환한다.

## transport

endpoint scheme으로 자동 설정된다. scheme과 별도로 명시할 수도 있다.

```cpp
options.transport = zlink::stream_connector::transport_t::websocket_secure;
```

## timeout

```cpp
options.connect_timeout   = std::chrono::seconds{5};   // connect 시도 전체 제한
options.request_timeout   = std::chrono::seconds{30};  // request 기본 제한
options.wait_timeout      = std::chrono::seconds{5};   // wait_for 기본 제한
```

`request_timeout`은 `request().submit()`의 기본값이고, `wait_timeout`은 `wait_for().submit()`의 기본값이다. 호출마다 `.timeout()`으로 덮어쓸 수 있다.

## heartbeat

```cpp
options.heartbeat.enabled  = true;
options.heartbeat.interval = std::chrono::seconds{10}; // idle 상태에서 ping 간격
options.heartbeat.timeout  = std::chrono::seconds{30}; // 이 시간 동안 응답 없으면 disconnected
```

heartbeat는 `$zlink.heartbeat.ping` / `$zlink.heartbeat.pong` control frame을 사용한다. 이 frame은 `on<packet_t>()` callback으로 전달되지 않는다.

## reconnect

```cpp
options.reconnect.enabled       = true;
options.reconnect.initial_delay = std::chrono::milliseconds{250};
options.reconnect.max_delay     = std::chrono::seconds{5};
options.reconnect.backoff_factor = 2.0;
options.reconnect.max_attempts  = 3; // std::nullopt이면 제한 없음
```

첫 연결 실패와 연결 끊김 모두 reconnect를 시도한다. 재시도 중에는 `reconnecting` 상태 이벤트가 발행된다. 모든 시도가 실패하면 `disconnected`로 전환된다.

## dispatch_mode

```cpp
options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;    // 기본값
options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
```

- `manual`: `on<T>()` callback은 `dispatch()`를 호출할 때 실행된다. 게임 엔진의 frame loop와 맞출 때 사용한다.
- `immediate`: callback은 connector receive path에서 즉시 실행된다. CLI, tool, e2e client에 적합하다.

자세한 내용은 [05 — 패킷 수신](05-receiving.ko.md)을 참고한다.

## compression

```cpp
options.compression = zlink::stream_connector::compression_t::lz4; // 기본값
options.compression_codec = zlink::stream_connector::lz4_compression_codec();
```

connector가 compressed frame을 보낼 때와 받을 때 사용할 codec 설정이다. 기본값은 LZ4다.
이 기본값은 모든 frame을 자동으로 압축한다는 뜻이 아니다. 호출마다 `.compress()`로
패킷 단위 압축을 요청한 frame만 압축된다.

server framework와 connector는 같은 compression codec을 사용해야 한다. custom codec을
쓰는 경우에도 built-in LZ4와 같은 option 경로로 설정한다.

```cpp
options.compression = zlink::stream_connector::compression_t::lz4;
options.compression_codec = std::make_shared<my_compression_codec_t>();
```

압축을 명시적으로 끄면 `.compress()`를 호출한 send/request는 송신 단계에서 실패하고,
compressed frame을 받으면 수신 단계에서 복원 오류로 처리된다.

```cpp
options.compression = zlink::stream_connector::compression_t::none;
options.compression_codec.reset();
```

## payload/metadata 크기 제한

```cpp
options.max_send_payload_size    = 64 * 1024; // 기본 64 KB
options.max_receive_payload_size = 64 * 1024; // 기본 64 KB
options.max_metadata_size        = 8 * 1024;  // 기본 8 KB
```

`max_send_payload_size`는 `send()`와 `request()`가 보낼 payload 크기를 제한한다. 이 한도를
넘으면 transport write 전에 `frame_too_large` 오류를 반환한다.

`max_receive_payload_size`는 서버에서 받은 stream frame의 encoded payload 크기를 제한한다. 이
한도를 넘는 frame은 payload buffer를 만들기 전에 `frame_too_large` 오류로 거부한다. 서버가 더
큰 push 또는 reply를 보낼 수 있는 환경이라면 이 값을 명시적으로 올린다.

`max_metadata_size`는 송신 metadata와 수신 frame header 크기에 모두 적용된다. metadata가 큰
프로토콜을 쓰는 경우 payload 상한과 별도로 조정한다.

## TLS 인증서 검증

```cpp
options.skip_server_certificate_validation = false; // 기본값 (검증함)
```

개발 환경에서 자체 서명 인증서를 사용할 때만 `true`로 설정한다. 프로덕션에서는 사용하지 않는다.

## 전체 예시

```cpp
namespace zsc = zlink::stream_connector;

zsc::connector_options_t options;
options.endpoint                 = "wss://game.example.com:443/stream";
options.connect_timeout          = std::chrono::seconds{5};
options.request_timeout          = std::chrono::seconds{10};
options.wait_timeout             = std::chrono::seconds{5};
options.heartbeat.interval       = std::chrono::seconds{15};
options.heartbeat.timeout        = std::chrono::seconds{45};
options.reconnect.max_attempts   = 5;
options.reconnect.max_delay      = std::chrono::seconds{10};
options.dispatch_mode            = zsc::dispatch_mode_t::manual;

auto connector = zsc::connector_factory_t::create(options);
```
