<!-- framework-adapter-nav:start -->
[문서 목록](../../../../../../README.ko.md)
<!-- framework-adapter-nav:end -->

# C++ Stream Connector 공개 계약

> 이 문서는 [Stream Connector 공통 스펙](../../32-stream-connector.ko.md)의 **C++ 투영**이다.
> 실행 환경, transport, wire, packet 모델, 생명주기, 오류 의미와 기본값은 공통 스펙이 소유한다.
> 이 문서는 그 의미를 C++에서 표현하는 정확한 공개 인터페이스만 고정한다.

## 1. 패키지와 진입점

일반 C++ client는 CMake target `zlink::stream_connector`를 사용한다. 공개 타입은
`zlink::stream_connector` 이름 공간에 있으며, 전체 connector 계약은 다음 header로 가져온다.

```cpp
#include <zlink/stream_connector/contracts/connector.hpp>
```

connector는 factory가 값 객체로 만든다. 생성 시 options를 복사하며 구현 세부 타입을 공개하지 않는다.

```cpp
static connector_t connector_factory_t::create(connector_options_t options);
```

## 2. `connector_t`

`connector_t`는 연결 상태, 생명주기와 packet 작업의 진입점을 제공한다.

```cpp
enum class stream_close_reason_t : std::uint8_t {
    client_close = 1,
    idle_timeout = 2,
    heartbeat_timeout = 3,
    server_drain = 4,
    protocol_error = 5,
    transport_error = 6
};

bool is_connected() const;
connection_state_t state() const;
std::optional<stream_close_reason_t> close_reason() const;
connector_options_t options() const;
std::size_t pending_dispatch_count() const;

result_t<void> connect();                                  // 연결 결과를 현재 호출에서 기다린다.
void connect(std::function<void(result_t<void>)> callback); // 연결 결과를 callback으로 받는다.
result_t<void> close();                                    // 종료 결과를 현재 호출에서 기다린다.
void close(std::function<void(result_t<void>)> callback);   // 종료 결과를 callback으로 받는다.
result_t<void> dispatch();                                 // Manual mode의 대기 callback 하나를 실행한다.
```

push callback은 `on<T>(...)`으로 등록한다. `dispatch_mode_t::manual`에서는 `dispatch()`가 callback을
실행하고, `dispatch_mode_t::immediate`에서는 수신 경로가 callback을 실행한다. `wait_for` 계열은 두
mode 모두에서 수신 큐의 일치하는 packet을 직접 소비한다.

`close_reason()`은 아직 연결이 끊긴 적이 없으면 빈 값을 반환한다. 종료 사유의 닫힌 값과 의미는
[공통 스펙 §6.3](../../32-stream-connector.ko.md#63-종료-사유)이 소유한다. enum 값 1~6은
`session-closing` wire 값과 같지만 codec은 명시적으로 변환하며 enum을 정수로 cast해 frame을 만들지
않는다.

## 3. 전송과 대기 builder

typed `send`와 `request`는 메시지 타입에서 packet 이름을 결정한다. raw packet overload도 제공한다.
각 호출은 builder를 반환하며 종결자인 `submit`을 호출해야 실행된다.

```cpp
send_call_t send(const TMessage& message);
request_call_t request(const TRequest& request);

send_call_t& packet_name(std::string name); // 외부 protocol과 연동할 때 packet 이름을 덮어쓴다.
send_call_t& metadata(std::string key, std::string value);
send_call_t& metadata(metadata_t metadata);
send_call_t& compress();
void submit(); // connector core의 기존 no-coroutine 경계에서 one-way 전송을 시작한다.

request_call_t& packet_name(std::string name);
request_call_t& metadata(std::string key, std::string value);
request_call_t& metadata(metadata_t metadata);
request_call_t& timeout(std::chrono::milliseconds timeout);
request_call_t& compress();
result_t<TReply> submit<TReply>(); // 상관관계가 일치하는 reply를 기다리고 TReply로 decode한다.
void submit<TReply>(std::function<void(result_t<TReply>)> callback);
```

한 번의 push 대기는 `wait_call_t<TMessage>`가 담당한다.

```cpp
wait_call_t<TMessage> wait_for<TMessage>();
wait_call_t<TMessage> wait_for<TMessage>(std::string packet_name);

wait_call_t<TMessage>& where(std::function<bool(const TMessage&)> predicate);
wait_call_t<TMessage>& timeout(std::chrono::milliseconds timeout);
result_t<TMessage> submit(); // 일치하는 unread packet 하나를 소비하고 decode한다.
void submit(std::function<void(result_t<TMessage>)> callback);
```

one-way `submit()`은 결과를 반환하지 않는다. C++ connector core는 공통 계약의
no-exception·no-coroutine 경계를 유지하므로 이 terminal에 `task_t`를 새로 도입하지 않는다.
전송 실패는 기존 connector error event로 보고한다. Request와 wait는 기존 결과형을 유지하며 callback
완료 경로도 함께 제공한다.

Typed `send`, `request`, `on`과 `wait_for`는 `connector_options_t::typed_codec`에 넣은 codec 하나를
함께 사용한다. 값을 지정하지 않으면 JSON codec을 사용한다. Protobuf, MessagePack과 사용자 codec
extension은 `typed_codec_t` 구현을 제공하며 connector를 만들 때 options에 한 번 넣는다. 메시지
타입마다 codec을 등록하거나 send/request operation마다 codec을 고르는 public API는 제공하지 않는다.
Raw encoded payload는 외부 protocol 연동을 위해 payload에 이미 기록된 codec 번호를 그대로 사용한다.

## 4. 테스트 대기 인터페이스

동작 계약은 [공통 스펙 §10.2](../../32-stream-connector.ko.md)가 소유한다.

### 4.1 push 관측 — connector 메서드

`expect_none`과 `wait_for_sequence`는 `wait_for`와 같은 `connector_t` 메서드다. 타입 이름에서 packet
이름을 결정하는 overload와 호출자가 packet 이름을 지정하는 overload를 모두 제공한다.

```cpp
expect_none_call_t<TMessage> expect_none<TMessage>();
expect_none_call_t<TMessage> expect_none<TMessage>(std::string packet_name);
expect_none_call_t<packet_t> expect_none(std::string packet_name);

wait_for_sequence_call_t<TMessage> wait_for_sequence<TMessage>();
wait_for_sequence_call_t<TMessage> wait_for_sequence<TMessage>(std::string packet_name);
wait_for_sequence_call_t<packet_t> wait_for_sequence(std::string packet_name);
```

negative 관측은 양수인 window를 반드시 지정한다. window 안에 같은 이름의 packet이 도착하면
`validation_failed`로 실패하고, 도착하지 않으면 성공한다.

```cpp
auto result = connector.expect_none<order_changed_t>()
                .within(std::chrono::milliseconds(100)) // 이 시간 동안 같은 push가 없어야 한다.
                .submit();
```

순서 관측은 각 `expect` 술어를 같은 이름의 push에 도착 순서대로 적용한다. 하나의 전체 timeout을
사용하며 성공하면 decode된 payload 목록을 반환한다. 단순히 N개가 도착했는지가 아니라 지정한
순서대로 도착했는지를 검증하는 계약이다.

```cpp
auto result = connector.wait_for_sequence<order_changed_t>()
                .expect([](const auto& value) { return value.status == status_t::paid; })
                .expect([](const auto& value) { return value.status == status_t::shipped; })
                .timeout(std::chrono::seconds(2)) // 두 술어를 모두 만족하는 전체 제한 시간이다.
                .submit();
```

두 builder 모두 `submit()`의 `result_t` 반환 방식과 callback을 받는 `submit(...)` 방식을 제공한다.
status 전용 메서드는 두지 않는다. status는 payload 필드이므로 한 번의 관측은
`wait_for<T>().where(...)`, 순서 관측은 `wait_for_sequence<T>().expect(...)`로 표현한다.
도메인 REST polling은 HTTP client의 책임이며 connector 인터페이스에 포함하지 않는다.

### 4.2 테스트 assertion helper

공통 E2E와 애플리케이션 테스트는 다음 namespace의 helper를 사용할 수 있다. 이 helper는 connector의
오류 결과를 검사할 때 반복되는 분기와 진단 생성을 한곳에서 처리한다.

```cpp
namespace zlink::stream_connector::assertions
{
void ensure(bool condition, std::string_view message);

template <typename TAction>
error_t expect_failure(
  TAction&& action,
  std::optional<error_code_t> expected_kind = std::nullopt);

template <typename TAction>
error_t expect_timeout(TAction&& action);
}
```

`ensure`는 조건이 거짓이면 전달받은 진단 메시지로 실패한다. 빈 진단 메시지는 허용하지 않는다.
`expect_failure`는 action의 실패 결과를 반환하며, 오류 종류를 지정하면 같은 종류인지도 검사한다.
`expect_timeout`은 request 또는 connect timeout만 반환하고 다른 실패는 그대로 전달한다.

## 5. 결과와 오류

`error_code_t`는 다음 닫힌 값 집합이다. 각 값의 의미와 operation·연결에 미치는 영향은
[공통 오류 표](../../32-stream-connector.ko.md#9-오류-의미)와 일대일로 대응한다.

```cpp
enum class error_code_t
{
    disconnected,
    configuration_error,
    validation_failed,
    request_timeout,
    connect_timeout,
    frame_decode_failed,
    frame_too_large,
    send_failed,
    compression_failed,
    tls_validation_failed,
    decompression_failed,
    user_callback_failed,
    observer_failed,
    observer_dropped,
    received_message_dropped,
    remote_error
};
```

실패할 수 있는 동기 작업은 `result_t<T>` 또는 `result_t<void>`를 반환한다. 성공 여부는 명시적 bool
변환으로 확인하고, 실패 시 `error()`와 `error_code()`로 `error_t`를 읽는다. callback 방식도 같은
`result_t`를 전달한다. 오류 종류와 의미는 [공통 스펙](../../32-stream-connector.ko.md)이 소유한다.

### 5.1 Flow correlation

Connector가 시작한 outbound operation은 별도 public option 없이 UUIDv7 `flow_id`를 한 번 생성한다.
Inbound callback에서 시작한 후속 operation은 현재 inbound flow를 재사용하고, callback이 끝나면
connector runtime이 current flow context를 정리한다. wire 형식과 비동기 context 경계는
[공통 Stream Connector §4.2](../../32-stream-connector.ko.md)와
[Flow Correlation §6](../../../27-flow-correlation.ko.md#6-async-작업과-execution-context)이 소유한다.

## 6. options

`connector_options_t`는 endpoint, transport, connect/request/wait timeout, heartbeat, reconnect,
송수신 payload 한도, observer와 수신 메시지 queue 한도, TLS 검증, dispatch mode와 compression을
표현한다. 기본값과 검증 규칙은 [공통 스펙 §6.1](../../32-stream-connector.ko.md)을 따른다.

Connector metric은 다음 public sink로 전달한다. Sink를 설정하지 않으면 metric 기록만 생략하며 connector
동작은 바뀌지 않는다.

```cpp
using connector_metric_attributes_t =
  std::map<std::string, std::variant<std::string, std::int64_t, double, bool>>;

class connector_metric_sink_t {
public:
    virtual ~connector_metric_sink_t() = default;
    virtual void add_counter(
      std::string_view name,
      std::string_view unit,
      std::uint64_t value,
      const connector_metric_attributes_t &attributes) noexcept = 0;
};

struct connector_options_t {
    std::string endpoint;
    transport_t transport = transport_t::tcp;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{30000};
    std::chrono::milliseconds wait_timeout{5000};
    heartbeat_options_t heartbeat;
    reconnect_options_t reconnect;
    std::size_t max_send_payload_size = 64 * 1024;
    std::size_t max_receive_payload_size = 64 * 1024;
    std::size_t max_inbound_observer_notifications = 1024;
    std::size_t max_received_messages = 1024;
    std::size_t max_inbound_observer_payload_preview_bytes = 0;
    bool skip_server_certificate_validation = false;
    dispatch_mode_t dispatch_mode = dispatch_mode_t::manual;
    compression_t compression = compression_t::lz4;
    std::shared_ptr<const compression_codec_t> compression_codec;
    std::shared_ptr<const typed_codec_t> typed_codec; // 비어 있으면 기본 JSON codec
    std::shared_ptr<connector_metric_sink_t> metric_sink;
};
```

`zlink.stream.reconnects`의 이름과 닫힌 attribute는
[공통 스펙 §6.2](../../32-stream-connector.ko.md#62-connector-reconnect-계기)를 따른다. Application과
E2E는 sink 구현에서 counter를 읽는다. Sink가 예외를 경계 밖으로 내보내지 않도록 `noexcept`로 고정하며
metric 처리 실패는 send, request와 연결 상태를 바꾸지 않는다.

`options()`는 [factory](../../../01-glossary.ko.md#factory)가 적용한 설정의 복사본을 반환한다. getter에 보이는 값은 실제 connect,
request, wait, queue, TLS와 compression 경로가 사용하는 값이어야 하며, 동작에 반영되지 않는
설정값을 공개하지 않는다.

## 7. Inbound observer

`observe_inbound(...)`는 연결 시작 전에 등록하고 이동 전용 `inbound_observer_registration_t`를
반환한다. registration의 `close()`가 관측을 해제한다. observer의 격리, payload preview와 overflow
동작은 [공통 스펙 §10](../../32-stream-connector.ko.md)을 따른다.

## 8. 검증

C++ connector의 공개 동작은 `test_cpp_stream_connector`가 검증한다. 언어별 계약 문서의 존재와
테스트 helper 인터페이스는 `test_cpp_framework_target_contract`의 `TH-CP-01` 게이트가 검증한다.
