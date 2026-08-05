# C++ common runtime exact interface

[C++ exact interface 목차](README.ko.md)

<!-- framework-adapter-nav:start -->
[스펙 목차](README.ko.md) | [이전: C++ 시스템 구조](../01-system-structure.ko.md) | [다음: C++ HTTP Hosting](../60-http-hosting.ko.md)
<!-- framework-adapter-nav:end -->

[스펙 목차](../../../../../README.ko.md)

> 이 문서는 ZLink Framework의 C++ 정식 public interface 계약이다.
> 이 문서는 `framework/doc/framework/common/spec` 아래 공통 Framework 정책을 상위 기준으로 따르고,
> C++ binding의 public 라이브러리 표면을 기반으로 framework 계층을 설계한다.

## 1. 계약 기준

`C++` framework는 C++ binding을 대체하지 않는다. framework는 C++ binding 위에
올라가며, binding이 제공하는 typed public API를 내부 runtime substrate로 사용한다.

기능과 사용성 개념은 framework 공통 스펙을 기준으로 맞춘다. 즉 app/host, DI scope,
handler registry, channel messaging, `STREAM`, `SPOT`, ActorGateway session relay,
monitoring, graceful shutdown은 같은 모델을 제공하고, C++ public API는 C++20 coroutine,
callback, RAII ownership에 맞게 표현만 바꾼다.

binding 기준은 아래 문서를 따른다.

- [C++ Binding Specification](../../../../../../../../../bindings/doc/spec/cpp/README.en.md)
- [C++ Codec Extension Specification](../../../../../../../../../bindings/doc/spec/cpp/codec.en.md)

framework public API는 `zlink::framework` namespace 아래에 둔다. 설치되는 public header에는
정식 contract와 명시적인 extension point만 포함한다. Application은 transport 구현을 알지 않고
framework를 구성할 수 있어야 한다.

## 2. Binding public dependency 경계

Framework package는 C++ binding의 public API만 의존한다. 공개 handler와 client에는 ChannelName, topic,
typed payload, timeout과 lifecycle처럼 framework 계약에 정의된 값만 나타난다.

사용자가 binding 값을 직접 넘길 수 있는 곳은 `message_t`처럼 정식 signature가 명시한 payload
경계로 제한한다. 그 밖의 binding 타입은 framework public signature에 나타나지 않는다.

## 3. Header 와 Namespace

권장 public header layout은 아래와 같다. `contracts/*` 아래 header가 `.NET`
`Contracts/*`에 대응하는 실제 public contract owner이고, `zlink/framework.hpp`는
사용자가 전체 framework 표면을 한 번에 include할 수 있는 facade다. 한 줄짜리
`zlink/framework/*.hpp` compatibility wrapper는 유지하지 않는다.

```text
zlink/framework.hpp
zlink/framework/version.hpp
zlink/framework/contracts/actors/*.hpp
zlink/framework/contracts/channels/*.hpp
zlink/framework/contracts/codecs/*.hpp
zlink/framework/contracts/configuration/*.hpp
zlink/framework/contracts/dispatch/*.hpp
zlink/framework/contracts/errors/*.hpp
zlink/framework/contracts/eventing/*.hpp
zlink/framework/contracts/handlers/*.hpp
zlink/framework/contracts/http/*.hpp
zlink/framework/contracts/locations/*.hpp
zlink/framework/contracts/messaging/*.hpp
zlink/framework/contracts/spots/*.hpp
zlink/framework/contracts/streams/*.hpp
zlink/framework/contracts/timers/*.hpp
zlink/framework/contracts/workers/*.hpp
```

`zlink/framework/runtime.hpp` 같은 public header는 제공하지 않는다. public API에는 `app_t`,
`request_client_t`, `spot_context_t`처럼 사용자가 이해하는 계약 이름만 노출한다.

이 구조는 `.NET`의 public interface를 C++ pure virtual class로 모두 옮긴다는 뜻이
아니다. C++ public API는 concrete facade와 value type을 적극적으로 사용할 수 있다.
다만 facade의 멤버, 생성자, method signature가 runtime 구현 타입을 노출하지 않아야 한다.
사용자 확장점만 abstract interface 또는 concept contract로 둔다.

### 3.1 공개 계약 경계

C++ 공개 header는 사용자가 구성하거나 호출하는 타입과 결과만 정의한다. 공개 facade가 상태를
유지하더라도 사용자는 그 상태의 자료구조나 처리 순서를 알 필요가 없어야 한다.

공개 `route_client_t`와 `route_send_call_t`는 node와 global Spot ID를 대상으로 하는 typed 호출을
제공한다. [User Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot)과 Instance Spot은 같은 ID-only 호출 표면을 사용하며, 별도 handle·resolver·논리 주소
타입을 제공하지 않는다. request 계열은 `channel_request_call_t`을 반환한다. 사용자는 target MeshNode,
location owner token이나 generation을 넘기지 않으며 routing envelope, location claim과
serializer 선택은 framework가 처리한다.

일반 request는 `request_to_node(...).timeout(...).submit<TReply>()`로 typed reply를 받는다.
`.metadata(key, value)`로 설정한 값은 application metadata 계약에 따라 snapshot되며, transport
세부와 correlation 상태는 공개 API에 드러나지 않는다.

하위 transport와 remote error envelope는 다음 공개 오류 의미로 변환한다.

| 하위 오류 의미 | C++ error kind |
|-------------------|----------------|
| `timed_out`, `timeout` | `deadline_exceeded` |
| `not_connected`, `route_not_connected` | `unavailable` |
| `not_found`, `request_target_not_found`, `handler_not_found` | `not_found` |
| typed 결과가 없는 admission 또는 filter 거부 | `rejected` |
| local queue capacity 부족, Message Follow relay queue bound 초과 | `capacity_exceeded` |
| remote error envelope가 알린 target queue capacity 부족 | `unavailable` |
| `busy` | 소유 위치에 따라 위 두 줄 중 하나. 하위 오류만으로 위치를 알 수 없으면 `unavailable` |
| `protocol_error`, `request_protocol_error` | `protocol_error` |

이 표는 request completion과 error envelope reply에 같은 의미로 적용한다.

DTO message name은 `static constexpr const char *packet_name`을 우선 사용한다. framework
handler 등록과 Stream Connector의 send, request와 on 기본 이름은 이 값을 읽는다. 이름이 없는
타입은 C++ type name을 사용할 수 있지만, 공개 sample과 정식 DTO는 명시적인 packet name을
가져야 한다.
### 3.2 C++ 공개 header 제약

C++는 설치된 header가 곧 공개 표면이므로 다음 규칙을 지킨다.

- template header에는 type check와 공개 facade forwarding만 둔다.
- public class의 state는 공개 계약 타입만 사용한다.
- JSON, MessagePack, Protobuf와 같은 선택 dependency 타입은 해당 codec extension의 공개 계약에만
  나타날 수 있다.
- contract test는 설치된 public header만 include한다.
- public inline 함수는 공개 validation과 forwarding을 넘어서 transport state를 조작하지 않는다.

모든 framework 타입은 `zlink::framework` namespace 아래에 둔다. 각 타입의
declaration은 [exact interface 목차](README.ko.md)에서 지정한 단 하나의 범주 문서가
소유한다.

## 4. Common result, coroutine과 message

```cpp
namespace zlink::framework {

template <typename T>
class result_t {
public:
    static result_t success(T value);
    static result_t failure(
      framework_error_kind_t kind,
      std::string message);
    bool has_value() const noexcept;
    explicit operator bool() const noexcept;
    const T &value() const;
    T &value();
    const framework_exception_t *error() const noexcept;
    framework_error_kind_t error_kind() const;
};

template <>
class result_t<void> {
public:
    static result_t success();
    static result_t failure(
      framework_error_kind_t kind,
      std::string message);
    bool has_value() const noexcept;
    explicit operator bool() const noexcept;
    void value() const;
    const framework_exception_t *error() const noexcept;
    framework_error_kind_t error_kind() const;
};

template <typename T>
class task_t {
public:
    struct promise_type {
        task_t get_return_object();
        std::suspend_never initial_suspend() noexcept;
        std::suspend_never final_suspend() noexcept;
        void unhandled_exception();
        void return_value(result_t<T> result);

        template <typename U>
        void return_value(U &&value);
    };

    explicit task_t(result_t<T> result);
    task_t(task_t &&) noexcept = default;
    task_t &operator=(task_t &&) noexcept = default;
    task_t(const task_t &) = delete;
    task_t &operator=(const task_t &) = delete;
    ~task_t() = default;
    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> continuation);
    T await_resume();
    const result_t<T> &result() const;
};

template <>
class task_t<void> {
public:
    struct promise_type {
        task_t get_return_object();
        std::suspend_never initial_suspend() noexcept;
        std::suspend_never final_suspend() noexcept;
        void unhandled_exception();
        void return_void() noexcept;
    };

    explicit task_t(result_t<void> result);
    task_t(task_t &&) noexcept = default;
    task_t &operator=(task_t &&) noexcept = default;
    task_t(const task_t &) = delete;
    task_t &operator=(const task_t &) = delete;
    ~task_t() = default;
    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> continuation);
    void await_resume();
    const result_t<void> &result() const;
};

class message_t {
public:
    message_t() = default;

    template <typename TValue>
    static message_t from(TValue value);

    template <typename TValue>
    TValue decode() const;

    bool encoded() const noexcept;
    bool empty() const noexcept;
};

} // namespace zlink::framework
```

## 5. Serialization

Framework는 typed JSON serializer를 기본 경로로 사용한다. Handler와 messaging API는 payload type을
받으며 application이 registry, type-erased pointer, encoder callback이나 raw dispatch table을 다루지 않는다.
JSON으로 표현할 수 없는 payload는 codec extension package를 `options.codecs().use(...)`로 선택한다. Extension
package의 registry 연결과 payload 변환은 runtime 내부 계약이며 application public header에 노출하지 않는다.
Framework, connector와 HTTP client가 codec을 바꿔도 handler와 client의 typed API는 바뀌지 않는다.

```cmake
target_link_libraries(app PRIVATE zlink::cpp)

# Protobuf가 필요할 때만 추가한다.
target_link_libraries(app PRIVATE zlink::framework_codec_protobuf)
```

## 6. C++ 고유 계약

### 6.1 Backpressure

SPOT과 STREAM의 backpressure는 public **call object, timeout, result error kind**로만 관찰한다.

- **application handler가 framework queue를 직접 제어하는 API를 두지 않는다.**
- **기본 정책은 무한 queue가 아니다.** queue 상한·submit timeout·overflow 정책은 framework runtime
  설정으로 닫고, **한도 초과는 실패 result로 반환한다.**
- 한도 초과의 error kind는 operation family와 queue 위치에 따라 다르다. 위 §오류 매핑 표와
  [Spot 메시징 §5.3](../../../../12-spot-messaging.ko.md)을 따른다 — 일괄 `capacity_exceeded`가
  아니다. one-way·send의 source-local 포화는 `deadline_exceeded`, request의 local queue
  포화는 `capacity_exceeded`, remote queue 포화는 `unavailable`이다.

### 6.2 Handler filter

filter는 `handler_filter_context_t`로 현재 dispatch 종류와 공개 metadata를 읽는다. descriptor와 raw
message storage는 Framework 내부에 유지한다. filter는 result를 반환하지 않으며 request reply를 만들거나
바꿀 수 없다.

`next()`를 호출하지 않으면 send와 Classic Fanout의 현재 handler만 종료한다. request는
`rejected`로 완료한다. `next()`는 한 번만 호출할 수 있으며 두 번째 호출은
`invalid_operation` 오류다.

filter의 등록 순서·`next` 의미·scope는 [framework API §8.1](../../../../06-framework-api.ko.md)이
소유한다.

### 6.3 Public surface 경계

Handler public contract는 `contracts/handlers/*`가 소유한다. Application은 handler signature, 공개 metadata와
결과만 사용하며 handler lookup, DI resolve와 serializer 실행 순서를 제어하지 않는다.


### 6.4 Timer 실행

Timer callback, packet과 Actor turn은 같은 owner의 serial execution queue에서 순서를 정한다. Application은
logical timer registration과 callback metadata만 사용한다.

**CPU-bound이거나 blocking 가능성이 있는 handler는 Framework runtime의 offload 실행으로 넘긴다**
(§7.3 worker).

### 6.5 Actor gateway 결정

| 항목 | 결정 |
|------|------|
| **`actor_ref_t` public 형태** | node routing id, actor id와 **generation**을 담는 C++ 값 타입 |
| **session 생성** | session 구현체는 **DI에서 resolve한다.** handler registry callback은 낮은 수준 확장 표면으로만 둔다 |
| **remote ActorGateway** | application에는 `actor_ref_t`와 session actor 표면만 보인다 |
| **actor factory 중복 정책** | 같은 actor id 중복은 **`already_exists`**, actor id/type 불일치는 **`type_mismatch`** 로 보고한다 |

**`actor_ref_t`의 `node_rid`·`actor_id`·`generation`은 bind·relay·push round-trip에서 보존된다.**
**local actor relay와 remote actor relay는 같은 public 표면을 쓴다.**

## 7. Public 타입 카탈로그

**이 절은 위 절들이 다루지 않은 public 타입을 채운다.** 여기 없는 `*_state_t`·`*_snapshot_t`는
**runtime 내부 상태**이며 공개 계약이 아니다.

### 7.1 Dispatch 오류 계약

Dispatch 실패는 별도 event type을 만들지 않고 [Monitoring §2](08-monitoring.ko.md#2-메시지-흐름-관측)의
`message_flow_event_t`로
표현한다. `surface`, `message_kind`, `reason`, `action`의 닫힌 값과 조건부 field 규칙은
[메시지 흐름 추적 §3~§4](../../../../26-message-flow-tracing.ko.md)이 소유한다.

### 7.2 Dispatch 실행 정책

`handler_execution_t`는 handler 실행 방식을 구분한다. Dispatch 진단, message-flow과 error
event의 exact declaration은 [Monitoring interface](08-monitoring.ko.md)가 소유한다.

### 7.3 Worker

```cpp
template <typename TResult> class worker_call_t
{
public:
    using executor_t = std::function<task_t<TResult>(
      std::stop_token)>;

    worker_call_t() = default;
    explicit worker_call_t(executor_t executor);
    worker_call_t &timeout (std::chrono::milliseconds value);
    task_t<TResult> submit ();
    task_t<TResult> yield ();
};

class worker_options_t {
public:
    std::size_t min_threads() const noexcept;
    worker_options_t &min_threads(std::size_t value);
    std::size_t max_threads() const noexcept;
    worker_options_t &max_threads(std::size_t value);
    std::chrono::milliseconds idle_timeout() const noexcept;
    worker_options_t &idle_timeout(std::chrono::milliseconds value);
    std::size_t max_queue_length() const noexcept;
    worker_options_t &max_queue_length(std::size_t value);
};
```

**worker는 spot·session 실행 문맥 밖에서 실행하는 작업이다.** 완료를 원래 실행 문맥에서 재개하는
규칙은 [비동기 실행 정책](../../../../05-async-execution-policy.ko.md)이 소유한다. Worker function에는 timeout,
host 종료와 caller cancellation을 합친 `std::stop_token`을 전달한다. `submit()`은 결과를 기다리지
않는 terminal이고 `submit()`은 현재 turn을 유지하며 결과를 기다린다. `yield()`는 `SpotWide` User Spot
또는 Instance Spot의 shared turn에서만 그 turn을 반환하고 결과를 기다린다. 다른 실행 문맥에서는
worker를 제출하거나 turn을 반환하지 않고 `invalid_operation`으로 완료한다.
`worker_options_t`의 최소·최대 thread 수, idle timeout과 queue 상한은 host 시작 전에만 설정한다.

### 7.4 오류 경계

동기 validation과 명시적인 결과 객체를 반환하는 API는 `result_t<T>`로 실패를 반환한다. 비동기 call의
`submit()`은 실패하면 같은 오류 정보를 가진 `framework_exception_t`를 throw한다. Application의 오류 분기는
`kind()`를 사용한다. `code()`는 timeout이나 transport처럼 platform 원인이 있을 때 진단 정보를 추가하지만
공통 오류 분류를 대신하지 않는다.


같은 Spot의 dispatch 직렬화와 `yield()` 허용 범위는
[stage-wrapper §3](../../../../17-stage-wrapper-on-spot.ko.md)과
[비동기 실행 정책](../../../../05-async-execution-policy.ko.md)이 소유한다.

---
<!-- framework-adapter-nav:bottom:start -->
[스펙 목차](README.ko.md) | [이전: C++ 시스템 구조](../01-system-structure.ko.md) | [다음: C++ HTTP Hosting](../60-http-hosting.ko.md)
<!-- framework-adapter-nav:bottom:end -->
