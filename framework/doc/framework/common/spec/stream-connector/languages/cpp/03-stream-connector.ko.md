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

**option 전 항목의 검증은 `connect`에서 이뤄진다**([공통 스펙 §6.3](../../32-stream-connector.ko.md#63-옵션-검증)).
C++ core는 예외를 던지지 않고 `create`는 값을 돌려주므로, 생성 표면에는 검증 실패를 전달할
통로가 없다. 그래서 C++이 실패를 알릴 수 있는 가장 이른 지점이 `connect`이며, **연결이 이뤄지기
전에** 거부한다. 값 하나가 허용 범위를 벗어나면 `error_code_t::validation_failed`, 항목 사이가
맞지 않으면 `error_code_t::configuration_error`다. endpoint scheme과 `transport`의 충돌, 이 빌드가
지원하지 않는 transport, `compression_t::none`에 `compression_codec`을 함께 넣는 것이 뒤쪽에
해당한다.

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
std::size_t received_count(std::string_view packet_name) const; // packet 이름별 수신 개수.

result_t<void> connect();                                  // 연결 결과를 현재 호출에서 기다린다.
void connect(std::function<void(result_t<void>)> callback); // 연결 결과를 callback으로 받는다.
result_t<void> close();                                    // 종료 결과를 현재 호출에서 기다린다.
void close(std::function<void(result_t<void>)> callback);   // 종료 결과를 callback으로 받는다.
result_t<void> dispatch();                                 // Manual mode의 대기 callback 하나를 실행한다.
```

**`received_count`는 packet 이름별 수신 개수를 돌려준다**([공통 스펙
§10](../../32-stream-connector.ko.md#10-수신-메시지-큐)). 소비해도 줄지 않고 dispatch mode와 무관하며,
연결이 성립할 때 0에서 다시 시작한다.

수신 message는 `message_t<TPayload>`다. `on<T>` handler와 `wait_for` 계열이 이 타입을 다룬다.

```cpp
enum class flow_origin_t : std::uint8_t {
    inbound = 1, timer = 2, application = 3, lifecycle = 4
};

template <typename TPayload>
struct message_t {
    std::string packet_name;
    TPayload payload;                          // typed codec으로 decode한 payload
    metadata_t metadata;
    std::string flow_id;                       // diagnostics level이 off이면 비어 있다(§6)
    std::optional<flow_origin_t> flow_origin;  // 같은 조건에서 빈 값이다
    std::optional<std::string> actor_id;       // 상대 bound Actor. slot 없는 frame은 빈 값(공통 스펙 §5.6)
};
```

`flow_id`와 `flow_origin`이 [공통 스펙 §5.5](../../32-stream-connector.ko.md#55-flow-노출과-전파)가
요구하는 수신 flow 노출이다. C++ runtime은 handler 실행 문맥에 현재 flow를 보관하므로 송신 call에
flow를 명시하는 인자를 두지 않는다(§5.1).

push callback은 `on<T>(...)`으로 등록한다. `dispatch_mode_t::manual`에서는 `dispatch()`가 callback을
실행하고, `dispatch_mode_t::immediate`에서는 수신 경로가 callback을 실행한다. `wait_for` 계열은 두
mode 모두에서 수신 큐의 일치하는 packet을 직접 소비한다.

**handler 등록은 등록을 해제할 수 있는 `subscription_t`를 반환한다**([공통 스펙
§7](../../32-stream-connector.ko.md#7-dispatch-모드)). push handler와 error·disconnect·connection
state handler가 모두 같은 타입을 반환한다.

```cpp
class subscription_t {   // move-only. 소멸자가 남아 있는 등록을 해제한다
public:
    void unsubscribe();  // 두 번 호출해도 오류로 처리하지 않는다
    bool active() const;
};

template <typename TMessage>
subscription_t on(std::function<void(const message_t<TMessage>&)> callback);
template <typename TMessage>
subscription_t on(std::string packet_name,
                  std::function<void(const message_t<TMessage>&)> callback);
subscription_t on_error(std::function<void(const error_t&)> callback);
subscription_t on_disconnected(std::function<void(std::optional<stream_close_reason_t>)> callback);
subscription_t on_connection_state_changed(
    std::function<void(const connection_state_changed_t&)> callback);

// 지금 bind되어 있는 Actor handle(공통 스펙 §5.6). application이 만들지 않는다.
std::vector<std::shared_ptr<actor_t>> actors() const;
std::shared_ptr<actor_t> actor(std::string_view actor_id) const;   // 없으면 nullptr
subscription_t on_actor_bound(std::function<void(const std::shared_ptr<actor_t>&)> callback);
subscription_t on_actor_unbound(std::function<void(const std::shared_ptr<actor_t>&)> callback);
```

```cpp
class actor_t {                 // bind된 Actor 하나. connector가 소유하고 unbound 통지에서 닫는다
public:
    const std::string& actor_id() const noexcept;
    bool is_bound() const noexcept;            // unbound 통지 뒤 false

    template <typename TMessage>
    send_call_t send(const TMessage& message);       // 이 Actor의 slot을 싣는다
    template <typename TRequest>
    request_call_t request(const TRequest& request);
    template <typename TMessage>
    subscription_t on(std::function<void(const message_t<TMessage>&)> callback);   // 이 Actor가 상대인 message만
    template <typename TMessage>
    subscription_t on(std::string packet_name,
                      std::function<void(const message_t<TMessage>&)> callback);
};
```

해제한 handler는 그 뒤의 `dispatch()`에서 실행하지 않는다.

**종료 사유의 읽기 표면은 `close_reason()` 메서드다.** 아직 연결이 끊긴 적이 없으면 빈 값을
반환하고, 다시 연결해도 값을 지우지 않고 마지막 종료의 사유를 유지한다. `on_disconnected`
handler가 사유를 인자로 받는 것은 이 읽기 표면에 추가하는 것이다. 종료 사유의 닫힌 값과 의미는
[공통 스펙 §6.2](../../32-stream-connector.ko.md#62-종료-사유)이 소유한다. enum 값 1~6은
`session-closing` wire 값과 같지만 codec은 명시적으로 변환하며 enum을 정수로 cast해 frame을 만들지
않는다.

## 3. 전송과 대기 builder

typed `send`와 `request`는 메시지 타입에서 packet 이름을 결정한다. raw packet overload도 제공한다.
각 호출은 builder를 반환하며 종결자인 `submit`을 호출해야 실행된다.

[공통 스펙 §5](../../32-stream-connector.ko.md#5-packet-모델)가 요구하는 **타입에 packet 이름을
붙이는 수단은 정적 멤버다.** C++에는 attribute나 annotation이 없으므로 payload 타입에 이름을 둔다.

```cpp
struct order_changed_t {
    static constexpr const char* packet_name = "order.changed"; // 타입에 붙인 packet 이름
    // ...
};
```

`connector_options_t::name_resolver`(§6)는 이 정적 멤버를 우선하고, 없으면 타입의 단순 이름을
사용한다. **컴파일러가 만드는 mangled name을 packet 이름으로 사용하지 않는다** — 빌드 환경이
바뀌면 값이 달라져 서버가 같은 타입의 handler를 찾지 못한다. 호출자가 builder의 `packet_name(...)`으로
명시하면 그 이름이 가장 우선한다.

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
template <typename TReply>
result_t<TReply> submit(); // 상관관계가 일치하는 reply를 기다리고 TReply로 decode한다.
template <typename TReply>
void submit(std::function<void(result_t<TReply>)> callback);
```

한 번의 push 대기는 `wait_call_t<TMessage>`가 담당한다. 술어와 반환은 payload가 아니라
`message_t<TMessage>`를 다룬다(§2).

```cpp
template <typename TMessage>
wait_call_t<TMessage> wait_for();
template <typename TMessage>
wait_call_t<TMessage> wait_for(std::string packet_name);

wait_call_t<TMessage>& where(std::function<bool(const message_t<TMessage>&)> predicate);
wait_call_t<TMessage>& timeout(std::chrono::milliseconds timeout);
result_t<message_t<TMessage>> submit(); // 일치하는 unread packet 하나를 소비하고 decode한다.
void submit(std::function<void(result_t<message_t<TMessage>>)> callback);
```

one-way `submit()`은 결과를 반환하지 않는다. C++ connector core는 공통 계약의
no-exception·no-coroutine 경계를 유지하므로 이 terminal에 `task_t`를 새로 도입하지 않는다.
전송 실패는 기존 connector error event로 보고한다. Request와 wait는 기존 결과형을 유지하며 callback
완료 경로도 함께 제공한다.

Typed `send`, `request`, `on`과 `wait_for`는
[공통 스펙 §5.4](../../32-stream-connector.ko.md#54-codec)의 주입점인
`connector_options_t::typed_codec`에 넣은 codec 하나를 함께 사용한다. 값을 지정하지 않으면 JSON
codec을 사용한다. Protobuf, MessagePack과 사용자 codec
extension은 `typed_codec_t` 구현을 제공하며 connector를 만들 때 options에 한 번 넣는다. 메시지
타입마다 codec을 등록하거나 send/request operation마다 codec을 고르는 public API는 제공하지 않는다.
Raw encoded payload는 외부 protocol 연동을 위해 payload에 이미 기록된 codec 번호를 그대로 사용한다.

## 4. 테스트 대기 인터페이스

동작 계약은 [공통 스펙 §10.1](../../32-stream-connector.ko.md)가 소유한다.

### 4.1 push 관측 — connector 메서드

`expect_none`과 `wait_for_sequence`는 `wait_for`와 같은 `connector_t` 메서드다. 타입 이름에서 packet
이름을 결정하는 overload와 호출자가 packet 이름을 지정하는 overload를 모두 제공한다.

```cpp
template <typename TMessage>
expect_none_call_t<TMessage> expect_none();
template <typename TMessage>
expect_none_call_t<TMessage> expect_none(std::string packet_name);
expect_none_call_t<packet_t> expect_none(std::string packet_name);

template <typename TMessage>
wait_for_sequence_call_t<TMessage> wait_for_sequence();
template <typename TMessage>
wait_for_sequence_call_t<TMessage> wait_for_sequence(std::string packet_name);
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
사용하며 성공하면 `std::vector<message_t<TMessage>>`를 반환한다. 술어가 받는 인자도 payload가
아니라 message다. 단순히 N개가 도착했는지가 아니라 지정한 순서대로 도착했는지를 검증하는 계약이다.

```cpp
auto result = connector.wait_for_sequence<order_changed_t>()
                .expect([](const auto& message) { return message.payload.status == status_t::paid; })
                .expect([](const auto& message) { return message.payload.status == status_t::shipped; })
                .timeout(std::chrono::seconds(2)) // 두 술어를 모두 만족하는 전체 제한 시간이다.
                .submit();
```

두 builder 모두 `submit()`의 `result_t` 반환 방식과 callback을 받는 `submit(...)` 방식을 제공한다.
이 표면의 실패는 모두 `error_code_t::validation_failed`다. status 전용 메서드는 두지 않는다.
status는 payload 필드이므로 한 번의 관측은
`wait_for<T>().where([](const auto& m) { return m.payload.status == …; })`, 순서 관측은
`wait_for_sequence<T>().expect(...)`로 표현한다.
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
    remote_error
};
```

[공통 스펙 §9.2](../../32-stream-connector.ko.md#92-전달--받는-쪽이-코드를-읽을-수-있어야-한다)의
두 형태를 C++이 모두 사용한다.

**connector core는 값으로 전달하며 던지지 않는다.** 예외가 비활성인 게임 엔진 빌드가 core를
그대로 사용하기 때문이다. 실패할 수 있는 동기 작업은 `result_t<T>` 또는 `result_t<void>`를
반환한다. 성공 여부는 명시적 bool 변환으로 확인하고, 실패 시 `error()`와 `error_code()`로
`error_t`를 읽는다. callback 방식도 같은 `result_t`를 전달한다.

```cpp
struct error_t {
    error_code_t code;    // 코드를 읽는 자리다
    std::string message;
};
```

**e2e와 도구는 예외를 던지는 어댑터를 사용한다.** 어댑터는 별도 CMake target
`zlink::stream_connector_throwing`이며 core 계약을 바꾸지 않는다.

```cpp
#include <zlink/stream_connector_throwing.hpp>

namespace zlink::stream_connector_throwing
{
class stream_connector_error : public std::runtime_error
{
public:
    zlink::stream_connector::error_code_t code() const noexcept; // 코드를 읽는 자리다
};

template <typename T> T value_or_throw(result_t<T> result); // 실패면 stream_connector_error를 던진다
inline void value_or_throw(result_t<void> result);
}
```

두 형태는 같은 `error_code_t` 값을 전달하며, 어느 쪽을 쓰든 받는 쪽이 공통 스펙 §9의 13개 중
무엇인지 읽는다. 오류 종류와 의미는 [공통 스펙](../../32-stream-connector.ko.md)이 소유한다.

### 5.1 Flow correlation

Connector가 시작한 outbound operation은 별도 public option 없이 UUIDv7 `flow_id`를 한 번 생성한다.
Inbound callback에서 시작한 후속 operation은 현재 inbound flow를 재사용하고, callback이 끝나면
connector runtime이 current flow context를 정리한다. wire 형식과 비동기 context 경계는
[공통 Stream Connector §4.2](../../32-stream-connector.ko.md)와
[Flow Correlation §6](../../../server/06-observability/04-flow-correlation.ko.md#6-async-작업과-execution-context)이 소유한다.

## 6. options

`connector_options_t`는 endpoint, transport, connect/request/wait timeout, heartbeat, reconnect,
송수신 payload 한도, TLS 검증, dispatch mode와 compression을
표현한다. 기본값과 검증 규칙은 [공통 스펙 §6.1](../../32-stream-connector.ko.md)을 따르고, 검증
시점은 §1이 고정한다.

[공통 스펙 §6](../../32-stream-connector.ko.md#6-연결-생명주기)이 요구하는 **무제한 reconnect는
`std::optional<int>`의 빈 값으로 표현한다.**

[공통 스펙 §3.1](../../32-stream-connector.ko.md#31-endpoint-scheme--transport)이 요구하는
**transport 미지정은 `std::optional<transport_t>`의 빈 값으로 표현한다.** 빈 값이면 endpoint
scheme이 transport를 결정하고, 값을 넣으면 명시한 transport로 보며 endpoint scheme과 어긋나면
`error_code_t::configuration_error`다. 고정 기본값을 두면 `ws://` endpoint 하나만 준 구성과
`transport_t::tcp`를 명시한 구성을 구분할 수 없다.

```cpp
struct reconnect_options_t {
    bool enabled = true;
    std::chrono::milliseconds initial_delay{250};
    std::chrono::milliseconds max_delay{5000};
    double backoff_factor = 2.0;
    std::optional<int> max_attempts = 3; // 빈 값이면 무제한. 그 밖에는 양수여야 한다
};

struct connector_options_t {
    std::string endpoint;
    std::optional<transport_t> transport;  // 빈 값이면 endpoint scheme이 transport를 결정한다.
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{30000};
    std::chrono::milliseconds wait_timeout{5000};
    heartbeat_options_t heartbeat;
    reconnect_options_t reconnect;
    std::size_t max_send_payload_size = 64 * 1024;
    std::size_t max_receive_payload_size = 64 * 1024;
    bool skip_server_certificate_validation = false;
    dispatch_mode_t dispatch_mode = dispatch_mode_t::manual;
    compression_t compression = compression_t::lz4;
    std::shared_ptr<const compression_codec_t> compression_codec;
    std::shared_ptr<const typed_codec_t> typed_codec;         // 공통 스펙 §5.4의 codec 주입점. 비어 있으면 기본 JSON codec
    std::shared_ptr<const packet_name_resolver_t> name_resolver; // 공통 스펙 §5.4의 name resolver 주입점. 비어 있으면 §3의 기본 규칙
    diagnostics_level_t diagnostics_level = diagnostics_level_t::errors;
};

class packet_name_resolver_t {
public:
    virtual ~packet_name_resolver_t() = default;
    virtual std::string resolve(std::string_view type_name) const = 0;
};

// 계약은 공통 스펙 §13이 소유한다. 기본값 errors. off이면 outbound frame에 flow pair를
// 만들지 않고(0x10 미설정), inbound flow 필드는 구조 길이 검사만 유지한 채 값 검증을
// 생략한다. Request correlation은 level과 무관하게 유지된다.
enum class diagnostics_level_t { off, errors, normal, detailed };
```

`options()`는 [factory](../../../server/00-foundation/02-glossary.ko.md#factory)가 적용한 설정의 복사본을 반환한다. getter에 보이는 값은 실제 connect,
request, wait, queue, TLS와 compression 경로가 사용하는 값이어야 하며, 동작에 반영되지 않는
설정값을 공개하지 않는다.

`connector_options_t::diagnostics_level`은 `create()`가 시작하는 level일 뿐이다. 공통 스펙
§13에 따라 connector는 [flow correlation §4](../../../server/06-observability/04-flow-correlation.ko.md#4-flow를-만드는-시점)가
말하는 client connector이므로, 실행 중 level 변경도
[message-flow-tracing §4.1](../../../server/06-observability/03-message-flow-tracing.ko.md#5-실행-중-기록-수준-변경과-비용-규칙)을
그대로 따른다. Application은 connector를 다시 만들지 않고 `connector_t`의 다음 두 메서드로
level을 읽고 바꾼다.

```cpp
class connector_t {
public:
    // ...
    diagnostics_level_t diagnostics_level() const;
    void set_diagnostics_level(diagnostics_level_t level); // 기다리지 않고 값을 바꾼다
    void set_diagnostics_level_async(                      // 같은 값을 바꾸는 비동기 짝
      diagnostics_level_t level,
      std::function<void(result_t<void>)> callback);
};
```

`set_diagnostics_level_async`는 connector core가 `connect`·`close`에 두는 callback 완료 경로와 같은
모양의 비동기 짝이며, [공통 스펙 §13](../../32-stream-connector.ko.md#13-diagnostics-level)이
요구하는 동기 표면을 대신하지 않는다. 동기 표면은 비동기 짝의 완료를 기다리지 않으므로 수신
callback 안에서 호출해도 자기 완료를 기다리는 순환이 생기지 않는다.

`diagnostics_level()`은 현재 유효한 level을 반환한다. `set_diagnostics_level(level)`은 그 뒤의
처리 지점(outbound frame encode 1회, inbound frame decode 1회)부터 적용되며, 호출 이전에 이미
encode·decode된 frame에는 소급 적용하지 않는다. 각 처리 지점은 level을 정확히 한 번만 읽어 그
처리 전체에 그 값 하나만 쓰므로, 처리 도중 level이 바뀌어도 하나의 frame이 두 level에 걸쳐
나뉘는 일은 없다. `options()`가 보여주는 diagnostics_level도 호출 시점에 `diagnostics_level()`이
반환할 값과 같으며, `create()`에 전달한 값과 다를 수 있다.

## 7. 검증

C++ connector의 공개 동작은 `test_cpp_stream_connector`가 검증한다. 언어별 계약 문서의 존재와
테스트 helper 인터페이스는 `test_cpp_framework_target_contract`의 `TH-CP-01` 게이트가 검증한다.
