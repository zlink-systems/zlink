# C++ configuration과 host exact interface

[C++ exact interface 목차](README.ko.md)

## 1. Host maintenance

Host lifecycle operation은 `relocate()`와 `shutdown()`으로 나눈다. `relocate()`는 현재 object를 선택한
운영 목적에 맞는 node로 이전하고 host를 `relocated` 상태로 유지한다. Application은 relocation 결과를 확인한
뒤 필요할 때 `shutdown()`을 호출한다. Relocation이 필요하지 않으면 `shutdown()`만 호출할 수 있다.

Application은 `relocate()`를 호출할 때 목적을 반드시 지정한다. `planned_maintenance`는 같은 application
version의 다른 node로 object를 이전한 뒤 현재 host를 점검하거나 재시작할 때 사용한다.
`rolling_update`는 application이 지정한 더 높은 version으로만 object를 이전할 때 사용한다.

기존 `retire()`, `drain()`과 `await_drained()`는 공개 interface에서 제거한다. `stop()`과
`request_stop()`은 host의 `shutdown()`을 시작한다. MeshName을 받는 component lifecycle operation은
제공하지 않는다.

```cpp
namespace zlink::framework {

enum class framework_runtime_state_t {
    preparing = 0,
    serving = 1,
    relocating = 2,
    relocated = 3,
    draining = 4,
    stopped = 5,
    error = 6
};

enum class relocation_outcome_t {
    relocated = 0,
    blocked = 1
};

enum class relocation_mode_t {
    planned_maintenance = 0,
    rolling_update = 1
};

enum class relocation_reason_t {
    none = 0,
    target_unavailable = 1,
    store_unavailable = 2,
    relocation_disabled = 3,
    state_incompatible = 4,
    deadline_exceeded = 5,
    relocation_failed = 6,
    runtime_not_ready = 7,
    manual_topology_unsupported = 8,
    shutdown_requested = 9,
    operation_in_progress = 10
};

struct relocation_options_t {
    relocation_mode_t mode;
    std::optional<std::int64_t> target_application_version;
    std::optional<std::chrono::milliseconds> deadline;
};

struct relocation_result_t {
    relocation_mode_t mode;
    std::int64_t effective_target_application_version;
    relocation_outcome_t outcome;
    relocation_reason_t reason;
};

enum class termination_outcome_t {
    stopped = 0,
    force_stopped = 1
};

enum class termination_reason_t {
    none = 0,
    deadline_exceeded = 1,
    teardown_failed = 2
};

struct termination_result_t {
    termination_outcome_t outcome;
    termination_reason_t reason;
};

} // namespace zlink::framework
```

`relocation_options_t::mode`는 필수다. `planned_maintenance`에서는
`target_application_version`을 지정하지 않는다. Framework는 source와 같은 application version만
선택하며 result의 `effective_target_application_version`에 source version을 기록한다.
`rolling_update`에서는 source보다 큰 `target_application_version`을 반드시 지정한다. Framework는 이 값과
정확히 같은 application version만 선택하고 result에도 같은 값을 기록한다. 이러한 option 조합을 위반하면
Framework는 `std::invalid_argument`로 호출을 거부하고 shared operation과 host state를 변경하지 않는다.

`deadline`이 비어 있으면 기본 30초를 사용한다. `wait_cancellation`은 waiter만 중단하며 이미 시작한 shared
operation을 취소하지 않는다. 같은 `relocation_options_t`를 사용한 동시 호출은 이미 실행 중인 shared
operation에 합류하며 같은 terminal result를 받는다. Mode, target application version 또는 deadline이
다른 동시 호출은 기존 operation에 합류하거나 이를 변경하지 않고
`blocked/operation_in_progress`를 반환한다. 이 결과의 mode와 effective target version은 거부된 호출이
요청한 유효한 option을 반영한다.

`relocate()`는 continuity preflight가 실패하면 admission과 state를 바꾸지 않고 `blocked`를 반환한다.
성공하면 `relocated/none`을 반환하며 host process와 infrastructure connection은 유지한다. `shutdown()`은
`blocked`를 반환하지 않는다.

두 mode 모두 candidate를 다음 순서로 좁힌다.

1. `planned_maintenance`는 source와 version이 같은 candidate만 남긴다.
   `rolling_update`는 요청한 target version과 정확히 같은 candidate만 남긴다.
2. Source와 같은 non-empty maintenance wave에 속한 candidate를 제외한다.
3. 남은 candidate에 stable type과 relocation policy·adapter 호환성 검사를 적용한다.
4. 호환 candidate의 population capacity를 확인한다.
5. 마지막 후보 집합에서 node-wide placement weight를 적용한다.

Version과 maintenance wave 조건을 capability·capacity·weight보다 먼저 적용하므로, rolling update가 같은
version node로 fallback하거나 planned maintenance가 더 높은 version node를 선택하지 않는다. 첫 번째
단계 뒤 candidate가 없거나 이후 조건을 만족하는 target이 없으면
`blocked/target_unavailable`을 반환한다. 여러 relocation unit은 모두 같은 effective target version을
사용하지만 각각 다른 eligible node를 선택할 수 있다.

Local manual RouteMesh peer, ClientServer client endpoint, fanout subscriber endpoint 또는 manual fanout publisher가
하나라도 있으면 `relocate()`는 `blocked/manual_topology_unsupported`를 반환한다. Automatic RouteMesh는
source의 Core peer table에서 descriptor와 같은 RID·lifecycle generation이 admitted·ready가 된 뒤에만
`relocating`으로 전환한다. 이 제한은 `shutdown()`에 적용하지 않는다.

`blocked/deadline_exceeded`는 모든 target의 `Prepared` 완료와 host `Relocating` descriptor publication 전에
deadline이 끝난 결과다. Connection-bound work와 bound-session request가 pre-`Captured` [deadline](../../../../01-glossary.ko.md#deadline) 안에 terminal
drain되지 않은 경우도 `relocation_disabled`가 아니라 이 결과를 사용한다. Framework는 relocation reference와
reservation을 정리하고 reversible seal을 해제한 뒤 host state와 admission을 복원한다. 모든 target이
`Prepared`이고 `Relocating` publication이 성공하면 모든 relocation unit을 완료하고 `relocated`로 전환한다.

`relocating` 중 `shutdown()`이 시작되면 현재 atomic relocation unit을 terminal 상태로 확정한 뒤 나머지
relocation을 중단한다. Relocation waiter는 `blocked/shutdown_requested`를 받고, host는 `draining`으로
전환하여 종료를 계속한다. 이미 `relocated`인 host에서 `shutdown()`을 호출하면 남은 connection과 resource만
정리한다. `serving`에서 호출하면 relocation 없이 `draining`으로 전환한다.

`relocation_result_t`와 `termination_result_t`의 유효한 조합은 다음과 같다. Caller는 relocation 결과가
`relocated/none`일 때만 모든 object의 이전이 끝났다고 판단한다.

| Result | Reason |
|---|---|
| `relocated` | `none` |
| `blocked` | `target_unavailable`, `store_unavailable`, `relocation_disabled`, `state_incompatible`, `deadline_exceeded`, `relocation_failed`, `runtime_not_ready`, `manual_topology_unsupported`, `shutdown_requested`, `operation_in_progress` |
| `stopped` | `none` |
| `force_stopped` | `deadline_exceeded`, `teardown_failed` |

## 2. App / Host

`app_t`는 framework의 가장 바깥 public type이다. 사용자는 `app_t::create()`로 앱을
만들고, `add_zlink_framework(...)`에서 services, handlers, zlink runtime을 한 번에
구성한 뒤 `run`을 호출한다. 낮은 수준의 runtime builder는 일반 애플리케이션 표면에
직접 노출하지 않는다.

```cpp
namespace zlink::framework {

class app_t {
public:
    app_t();
    ~app_t();
    app_t(app_t &&) noexcept;
    app_t &operator=(app_t &&) noexcept;
    app_t(const app_t &) = delete;
    app_t &operator=(const app_t &) = delete;

    static app_t create();

    config_builder_t &config() noexcept;
    logging_builder_t &logging() noexcept;
    health_builder_t &health() noexcept;

    app_t &add_module(module_t &module);
    app_t &add_zlink_framework(
      std::function<void(zlink_framework_options_t &)> configure);
    template <typename TModule, typename... TArgs>
    app_t &add_zlink_framework(TArgs &&...args);
    app_t &add_hosted_service(std::unique_ptr<hosted_service_t> service);

    bool is_ready() const noexcept;
    app_t &set_message_flow_mode(message_flow_log_mode_t mode) noexcept;
    message_flow_log_mode_t message_flow_mode() const noexcept;

    task_t<relocation_result_t> relocate(
      relocation_options_t options,
      std::stop_token wait_cancellation = {});
    task_t<termination_result_t> shutdown(
      std::chrono::milliseconds deadline = std::chrono::seconds{30},
      std::stop_token wait_cancellation = {});

    int run(int argc, char **argv);
    void stop() noexcept;
    void request_stop() noexcept;
};

} // namespace zlink::framework
```

`run`은 `int`를 반환한다. 반환값은 process exit code로 사용할 수 있어야 한다.
handler 예외, runtime 오류, signal shutdown은 host가 수집하고 종료 경로를 닫는다.

## 3. Service 등록

C++ binding에는 DI 개념이 없으므로 Framework가 handler와 hosted component의 service lifetime을
관리한다. Application은 service type과 lifetime만 등록한다. Runtime의 provider, scope와 type-erased
factory invocation은 public interface에 포함하지 않는다.

```cpp
namespace zlink::framework {

enum class service_lifetime_t {
    singleton = 0,
    scoped = 1,
    transient = 2
};

class service_collection_t {
public:
    service_collection_t();
    ~service_collection_t();
    service_collection_t(service_collection_t &&) noexcept;
    service_collection_t &operator=(service_collection_t &&) noexcept;
    service_collection_t(const service_collection_t &) = delete;
    service_collection_t &operator=(const service_collection_t &) = delete;

    template <typename T>
    service_collection_t &add_singleton();

    template <typename T, typename... TDependencies>
    service_collection_t &add_singleton();

    template <typename T>
    service_collection_t &add_singleton(std::unique_ptr<T> instance);

    template <typename T>
    service_collection_t &add_scoped();

    template <typename T, typename... TDependencies>
    service_collection_t &add_scoped();

    template <typename T>
    service_collection_t &add_transient();

    template <typename T, typename... TDependencies>
    service_collection_t &add_transient();

    template <typename T, typename... TDependencies, typename TFactory>
    service_collection_t &add_factory(
      TFactory factory,
      service_lifetime_t lifetime = service_lifetime_t::transient);
};

} // namespace zlink::framework
```

기본 생성 규칙은 아래와 같다.

- `add_singleton<T>()`, `add_transient<T>()`는 기본 생성 가능한 타입만 자동 생성한다.
- 생성자 의존성이 있는 타입은 `add_singleton<T, Dep1, Dep2>()`,
  `add_scoped<T, Dep1, Dep2>()`, `add_transient<T, Dep1, Dep2>()`처럼 의존 타입을 명시한다.
  Framework는 내부 provider에서 `Dep1`, `Dep2`를 resolve한 뒤 `T(Dep1 &, Dep2 &)`를 호출한다.
- `add_scoped<T>()`는 framework가 소유하는 scope 안에서만 resolve한다.
- 복잡한 외부 객체 생성이나 조건부 생성이 필요한 경우에만
  `add_factory<T, Dep1, Dep2>()`를 사용한다. [Factory](../../../../01-glossary.ko.md#factory)는 선언한 dependency의 typed reference만 받으며
  runtime provider나 scope를 받지 않는다.
- Channel·HTTP handler class는 handler group이나 HTTP route에 등록하면 Framework가
  dispatch scope에서 생성한다. Application이 같은 handler type을 service collection에
  다시 등록하지 않는다.
- Spot packet·Actor payload handler는 Spot member function이므로 별도 service가 아니다.
  Timer handler class는 Spot activation scope에서 한 번 생성하며 같은 activation의 tick이
  재사용한다. Timer handler의 `dependency_types`에 선언한 dependency만 해당 scope에서
  resolve한다.
- `Boost.Ext.DI` 같은 외부 DI 라이브러리는 public dependency로 두지 않는다.

`scoped` lifetime은 raw transport 기능이 아니라 Framework가 소유하는 DI lifetime이다. Framework는
handler dispatch, STREAM session, Spot activation의 scope 경계를 내부에서 만든다.
channel handler는 dispatch마다 scope를 만들고, [STREAM session](../../../../01-glossary.ko.md#stream-session)은 session scope를 가지며,
[Spot](../../../../01-glossary.ko.md#spot)과 Entry Spot은 activation scope를 가진다. actor factory는 actor creation scope에서
resolve하고, actor instance 자체는 actor runtime이 소유한다.

예시는 아래와 같다.

```cpp
options.services()
  .add_singleton<order_repository_t>()
  .add_transient<order_service_t, order_repository_t>()
  .add_transient<order_handler_t, order_service_t>();
```

## 4. Hosted Service 와 Module

hosted service는 app lifecycle에 묶이는 background worker다.

```cpp
namespace zlink::framework {

class hosted_service_t {
public:
    virtual ~hosted_service_t() = default;
    virtual void start() = 0;
    virtual void request_stop() noexcept {}
    virtual void stop() noexcept = 0;
};

class module_t {
public:
    virtual ~module_t() = default;
    virtual void configure(zlink_framework_options_t &options) = 0;
};

template <typename TModule>
concept framework_module_contract_t =
  requires(TModule &module,
    zlink_framework_options_t &options) {
      module.configure(options);
  };

} // namespace zlink::framework
```

module은 서비스 등록, runtime 구성과 handler 등록을 `zlink_framework_options_t` 하나로
묶는다. 별도 low-level builder와 handler registry를 넘겨 runtime wiring 순서를 application에 노출하지 않는다.

`app_t::add_zlink_framework(options_callback)`는 `.NET`의
`AddZLinkFramework(options => ...)`에 대응하는 C++ 고수준 구성 진입점이다. C++에는 assembly
reflection이 없으므로 `.NET`의 `AddHandlersFromAssemblyOf(...)`만 그대로 옮기지 않는다.
그 대신 handler group을 먼저 고르고, 그 group 안에 handler 타입을 명시해서
`options.handlers().group(group_name).add<THandler>()`,
`add_send<THandler>()`, `add_publish<THandler>()`로 등록한다.
나머지 codec, discovery, RouteMesh membership, handler group 구성은 `.NET`과 같은 읽기 수준을
유지한다.

JSON은 기본 codec이므로 별도 등록하지 않는다. 사용자가 모든 request/reply message
type을 codec 설정에 나열하지 않는다. C++ framework는 `options.handlers().group(...).add<THandler>()`에서
handler의 `request_type`, `reply_type`을 읽고 기본 JSON serializer를 내부에서 선택한다.
send handler는 `message_type`, publish handler는 `event_type`을 읽어 같은 방식으로 serializer와
handler registry 항목을 등록한다. `options.codecs().use(...)`는 일반 message type을 나열하는
단계가 아니라, 기본 JSON으로 표현할 수 없는 payload나 별도 binary serializer extension을
연결하는 고급 확장점이다. 따라서 request/send/publish handler를 같은 group 이름으로
묶고, channel builder의 `.add_handler_group(...)`에서 channel에 연결할 수 있다.
handler group은 channel 종류와 맞아야 한다. [RouteMesh](../../../../01-glossary.ko.md#routemesh) ChannelName은 request/send
handler group을 받을 수 있고, fanout channel은 publish handler group만 받을 수 있다. 맞지 않는
group을 연결하면 options 작성 시점에 설정 오류로 실패한다.
같은 channel에 같은 packet 이름의 handler가 두 번 노출되면 host가 message를 받기 전에 startup 설정
오류로 실패한다. 중복 handler는 실행 중 request protocol 오류로 처리하지 않는다.
이 규칙은 fluent options의 handler group 경로에 적용한다. channel이 group을 먼저 참조한 뒤 handler가 들어오는 경우와 handler가 먼저
등록되고 channel이 나중에 group을 참조하는 경우 모두 중복을 허용하지 않는다.
MeshNode는 ROUTER listen endpoint와 0개 이상의 [ChannelName](../../../../01-glossary.ko.md#channelname) [membership](../../../../01-glossary.ko.md#membership)을 가질 수 있다. 호출 또는
Node direct 전용 [MeshNode](../../../../01-glossary.ko.md#meshnode)는 membership 없이 시작할 수 있고, Channel handler를 제공하는 MeshNode는
Server membership을 하나 이상 등록해야 한다. 각 Server ChannelName은 request/send handler group을
가질 수 있다. fanout subscriber는 publish handler group을 하나 이상 등록해야 한다.
Channel·HTTP handler에 생성자 의존성이 있으면 `using dependency_types =
zlink::framework::dependency_list_t<dep1_t, dep2_t>;`처럼 의존 타입을 명시한다. framework는
각 dispatch scope에서 dependency와 handler를 생성한다. Handler를 singleton service로
등록하지 않는다.
`logger_t<THandler>`는 framework 기본 dependency다. handler가
`dependency_types`에 `logger_t<THandler>`를 넣으면 사용자가 별도 service registration을
작성하지 않아도 DI가 `.NET`의 `ILogger<T>`처럼 category logger를 주입한다. 로그 출력 대상은
handler 등록이 아니라 `app.logging().use_console()`, `app.logging().use_file(...)` 같은
host logging 설정에서 정한다. custom category가 필요하면 `logger_factory_t`를 dependency로
받아 handler 내부에서 category logger를 만들 수 있다.

```cpp
app.add_zlink_framework ([&](zlink::framework::zlink_framework_options_t &options) {
    options.use_filter<audit_filter_t>();
    options.metadata()
      .allow_session_to_actor("trace-id")
      .allow_actor_to_session("trace-id");

    auto mesh = options.add_route_mesh(sample_names_t::application_mesh)
      .listen(7300)
      .set_routing_id(topology.application_rid);
    mesh.channel(sample_names_t::api_channel)
      .server()
      .add_handler_group("api");
    mesh.channel(sample_names_t::play_channel).client();

    // 시작 시 사용할 message-flow 관측 수준을 설정한다.
    options.configure_dispatch().message_flow(
      zlink::framework::message_flow_log_mode_t::errors_only);

    options.handlers()
      .group("api")
      .add<authenticate_player_handler_t>()
      .add<match_bingo_api_handler_t>()
      .add_send<player_command_handler_t>();

    options.handlers()
      .group("events")
      .add_publish<notification_event_handler_t>();
});
```

자동 peer discovery를 사용하면 등록된 `location_store_t` provider에서 같은 [MeshName](../../../../01-glossary.ko.md#meshname)의
MeshNode descriptor를 찾는다. 공식 Redis package는 사용할 수 있는 provider 가운데 하나다.
수동 peer는 `peer_connections().connect(endpoint)` 또는 expected RID를 함께 받는 overload로
등록한다. fanout subscriber의 endpoint 목록은 RouteMesh peer intent와 별도다.

이 구조에서는 샘플 `main.cpp`, role `*HostFactory`, 일반 사용자 설정 예제가 handler member
function pointer, handler용 DI factory lambda, monitoring channel 문자열, serializer smoke 검증,
message type을 모두 나열하는 codec 등록 같은 세부 구현을 직접 알 필요가 없다. 그런 내용이 보이면
framework options builder가 충분히 깊지 않은 것으로 본다.

`zlink_framework_options_t`의 사용자 표면은 fluent options builder로 제한한다.
일반 사용자 설정에는 낮은 수준 channel runtime builder를 직접 노출하지 않는다. C++ 내부 runtime builder에는 낮은 수준 API가 남아 있을 수 있지만,
샘플과 guide 수준의 설정은 아래처럼 역할이 바로 보이는 형태를 사용한다.

`options.configure_dispatch(...)`는 interface graph를 만들지 않고
`dispatch_options_t` value를 람다에 넘긴다. 이 value는 Spot과
STREAM dispatch mode, unhandled request/send/publish 정책, message flow diagnostics 설정을
담는다. native dispatch token, queue slot, handler lookup table은 이 표면에 나오지 않는다.
diagnostics sample rate는 `0.0`에서 `1.0` 사이여야 하며 NaN은 허용하지 않는다.
send와 publish는 reply path가 없으므로 unhandled 정책에 `reply_error`를 사용할 수 없다.

```cpp
options.add_location_store(redis_location_store);

auto mesh = options.add_route_mesh(sample_names_t::application_mesh)
  .listen(7300)
  .set_routing_id(topology.application_rid);
mesh.channel(sample_names_t::api_channel)
  .server()
  .add_handler_group("api");

options.add_fanout_channel(sample_names_t::notification_channel)
  .enable_publisher(7400)
  .set_routing_id(topology.notification_publisher_rid)
  .enable_subscriber() // 같은 ChannelName의 publisher를 location store에서 자동으로 발견한다.
  .add_handler_group("events");

mesh.channel(sample_names_t::game_channel).client();
mesh.peer_connections().connect(topology.play_router_endpoint);
mesh.add_entry_spot<session_entry_spot_t>();

options.add_stream_node(sample_names_t::stream_name)
  .bind(7500)
  .enable_actor_dispatch()
  .register_session<client_session_t>()
```

Entry Spot의 SpotId는 Framework가 `<prefix>-entry-<lowercase-canonical-uuid-v4>` 형식으로 발급한다.
Application이 Entry Spot의 RoutingId나 고정 SpotId를 설정하는 public option은 제공하지 않는다.

`enable_actor_dispatch()`는 session Actor dispatch에 global ActorId lookup과 exact ActorRef bind를 사용하도록
설정한다. Target MeshName을 받거나 첫 MeshNode에서 추론하지 않는다. Actor dispatch를 사용하지 않는 STREAM
node는 호출하지 않는다. 같은 builder에서 두 번 호출하면 startup이 실패한다.
`register_session<TSession>()`은 `.NET`의 `RegisterSession<TSession>()`에 맞춘 typed session
등록 표면이다. `TSession`은 `packet_stream_session_t`를 상속해야 하며, framework service
collection에 stream-session scope 서비스로 등록된다. `TSession::session_name`이 있으면 그 값을
native packet session 이름으로 사용하고, 없으면 타입 이름 기반 message name을 사용한다.
`register_session(name)`은 session 이름을 직접 지정해야 하는 low-level 구성에 남긴다.
하나의 stream node에는 packet session을 하나만 선언한다. `register_session<T>()`과
`register_session(...)`을 중복 호출하면 마지막 값으로 덮어쓰지 않고 설정 오류로 처리한다.

MeshNode는 `listen(...)`으로 ROUTER endpoint를 열고 `channel(...)` 뒤에 role을 선택한다.
자동 peer는 Redis [descriptor](../../../../01-glossary.ko.md#descriptor)로, 수동 peer는 `peer_connections()`로 구성한다.
Node·Channel·Spot·Actor 메시지는 같은 MeshNode ROUTER를 사용한다.
fluent options에서 channel 이름, handler group 이름, endpoint, MeshName, stream node
이름처럼 식별자나 연결 주소로 쓰이는 값은 빈 문자열이나 공백 문자열을 허용하지 않는다.
잘못된 값은 low-level socket/runtime까지 전달하지 않고 builder 호출 또는 options 적용 시점의
framework error로 닫는다.
Spot 코드는 owner MeshNode의 client로 [Node direct](../../../../01-glossary.ko.md#node-direct), ChannelName select-one과 Logical Multicast를
사용한다. [Logical Multicast](../../../../01-glossary.ko.md#logical-multicast)는 별도 PUB/SUB 역할을 구성하지 않는다. classic
fanout만 독립 PUB/SUB socket을 사용한다.

### 4.1 HTTP Hosting

HTTP hosting은 ASP.NET Core Minimal API의 `MapGet`, `MapPost`, `MapPut`,
`MapDelete`에 대응하는 C++ framework 표면이다. MVC controller, Razor page,
template rendering, WebSocket transport는 범위에 넣지 않는다. 대신 route handler,
DI scope, JSON binding, middleware/filter, logging, validation, error mapping,
zlink channel 호출은 같은 application host 안에서 제공한다.

```cpp
namespace zlink::framework {

class http_options_builder_t {
public:
    http_options_builder_t &listen(std::string endpoint);
    http_options_builder_t &configure_tls(
      std::function<void(http_tls_options_builder_t &)> configure);
    http_options_builder_t &configure_server(
      std::function<void(http_server_options_builder_t &)> configure);

    template <typename THandler>
    http_options_builder_t &map_get(std::string path);

    template <typename THandler>
    http_options_builder_t &map_post(std::string path);

    template <typename THandler>
    http_options_builder_t &map_put(std::string path);

    template <typename THandler>
    http_options_builder_t &map_delete(std::string path);

    template <typename TMiddleware>
    http_options_builder_t &use();

    http_options_builder_t &map_health(std::string path);
    http_options_builder_t &map_readiness(std::string path);
    http_options_builder_t &map_liveness(std::string path);
    const http_options_snapshot_t &snapshot() const noexcept;
    void validate() const;
};

struct http_context_t {
    http_method_t method;
    std::string path;
    std::string correlation_id;
    std::map<std::string, std::string> request_headers;
    std::map<std::string, std::string> response_headers;
    std::optional<std::string> response_body;
    int response_status;

    http_context_t &response_header(std::string name, std::string value);
    http_context_t &json_response(int status, std::string body);
};

struct http_request_t {
    http_method_t method;
    std::string path;
    std::string target;
    std::string query_string;
    std::string correlation_id;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> route_values;
    std::map<std::string, std::string> query_values;
    std::string body;
    std::string content_type;
    std::string remote_endpoint;
};

struct http_response_t {
    int status = 200;
    std::string body;
    std::string content_type = "application/json";
    std::map<std::string, std::string> headers;

    http_response_t &header(std::string name, std::string value);
};

class handler_options_builder_t {
public:
    class group_builder_t {
    public:
        template <typename THandler>
        group_builder_t &add();

        template <typename THandler>
        group_builder_t &add_send();

        template <typename THandler>
        group_builder_t &add_publish();
    };

    group_builder_t group(std::string group_name);
};

class metadata_policy_builder_t {
public:
    metadata_policy_builder_t &add_forwarded_metadata_key(std::string key);
    metadata_policy_builder_t &allow_session_to_actor(std::string key);
    metadata_policy_builder_t &allow_actor_to_session(std::string key);
};

class codec_options_builder_t {
public:
    template <typename TExtension>
    codec_options_builder_t &use(const TExtension &extension);
};

enum class application_hwm_profile_t {
    compact = 0,
    low_latency = 1,
    balanced = 2,
    throughput = 3
};

class inbound_dispatch_options_t {
public:
    inbound_dispatch_options_t &set_application_hwm_bytes(
      std::optional<std::uint64_t> value);
    inbound_dispatch_options_t &set_application_hwm_profile(
      application_hwm_profile_t value);
    inbound_dispatch_options_t &set_process_memory_limit_bytes(
      std::optional<std::uint64_t> value);
};

class zlink_framework_options_t {
public:
    handler_options_builder_t handlers();
    codec_options_builder_t codecs();
    metadata_policy_builder_t metadata();
    network_options_t &configure_network();
    worker_options_t &worker();
    dispatch_options_t &configure_dispatch();
    dispatch_options_t dispatch_options() const;
    location_options_t &configure_locations();
    inbound_dispatch_options_t &configure_inbound_dispatch();
    location_options_t location_options() const;
    zlink_framework_options_t &set_max_pending(std::size_t count);
    zlink_framework_options_t &set_application_version(
      std::int64_t application_version);
    zlink_framework_options_t &set_maintenance_wave(
      std::optional<std::string> maintenance_wave);
    zlink_framework_options_t &set_default_request_timeout(
      std::chrono::milliseconds timeout);
    service_collection_t &services() noexcept;
    zlink_framework_options_t &add_location_store(
      std::shared_ptr<location_store_t> store);
    client_server_channel_builder_t add_client_server_channel(
      std::string channel_name);
    fanout_channel_builder_t add_fanout_channel(std::string channel_name);
    mesh_node_builder_t add_route_mesh(std::string mesh_name);
    stream_node_options_builder_t add_stream_node(std::string stream_name);
    stream_compression_options_builder_t configure_stream_compression();
    http_options_builder_t &http() noexcept;
    template <typename TFilter>
    zlink_framework_options_t &use_filter();

    zlink_framework_options_t &handler_coroutine_workers(
      std::size_t worker_count);
    std::size_t handler_coroutine_workers() const noexcept;
    zlink_framework_options_t &add_relocation_store(
      std::shared_ptr<relocation_store_t> store);
};

} // namespace zlink::framework
```

Location runtime을 사용하는 application은 `add_location_store(...)`로 Location Store를 정확히 하나 등록한다.
`RecreateOnRelocation` 또는 `PreserveStateWith` factory가 하나라도 있거나 Instance Spot factory가 하나라도 있으면
`add_relocation_store(...)`로 Relocation Store도 정확히 하나 등록한다. [Instance Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot) factory가 없고
`DisableRelocation` factory만 있는 same-node 구성에는 Relocation Store가 필요하지 않다. 필요한 Store가 없거나
같은 capability가 중복 등록되면 Framework는 socket bind 전에 configuration error로 종료한다.

`configure_network()`는 process 전체의 BindHost와 AdvertiseHost 기본값을 반환하며 listener별 설정이 이 값을
재정의한다. `worker()`는 bounded worker pool의 최소·최대 thread 수, idle timeout과 queue 상한을 반환한다.
두 option은 host 시작 전에만 변경할 수 있다.

`configure_inbound_dispatch()`는 host 전체 설정 하나를 반환한다. HWM을 `std::nullopt`로 지정하면 Auto,
`0`이면 제한 없음, 양수이면 정확한 byte 상한을 사용한다. Profile 기본값은 `balanced`다. Process memory
limit은 `std::nullopt` 또는 양수만 허용한다. Auto mode에서 명시한 값이 없으면 process에 적용된 유한한
container·cgroup·Windows Job Object와 같은 OS 상한을 확인한다. C++에는 managed heap 상한이 없으므로
확인된 OS 상한을 사용하고, OS 상한도 없으면 시스템 물리 메모리 총량을 사용한다. 계산 결과가 양수가
아니면 socket bind 전에 configuration error로 실패한다.

Application version과 maintenance wave는 host 전체에 한 번 설정한다. Version은 기본값 0인 non-negative
signed 64-bit deployment ordinal이고 모든 local MeshNode가 같은 값을 게시한다. Empty optional wave는
maintenance wave exclusion을 사용하지 않는다는 뜻이다.

사용 예시는 아래와 같다.

```cpp
app.add_zlink_framework([&](auto &options) {
    auto mesh = options.add_route_mesh(sample_names_t::application_mesh)
      .listen(7300) // 이 RouteMesh가 peer message를 받을 endpoint를 연다.
      .set_routing_id(topology.application_rid); // 같은 mesh 안에서 이 node를 식별한다.
    mesh.channel(sample_names_t::api_channel)
      .server() // 이 node를 api_channel의 요청 처리 후보로 게시한다.
      .add_handler_group("api"); // 등록할 DI handler group을 Channel handler와 연결한다.
    mesh.channel(sample_names_t::play_channel)
      .client(); // Server membership 없이 play_channel 호출 경로만 등록한다.

    options.http()
      .listen(topology.api_http_endpoint) // HTTP client가 연결할 listener를 연다.
      .map_post<create_game_http_handler_t>("/games"); // POST /games를 DI handler에 연결한다.
});
```

HTTP handler는 message handler와 같은 type alias 규칙을 사용한다.

```cpp
class create_game_http_handler_t {
public:
    using request_type = create_game_http_req_t;
    using reply_type = create_game_http_res_t;
    using dependency_types =
      dependency_list_t<request_client_t, logger_t<create_game_http_handler_t>>;

    explicit create_game_http_handler_t(
      request_client_t &client,
      logger_t<create_game_http_handler_t> &logger);

    task_t<create_game_http_res_t> handle(const create_game_http_req_t &request);
};
```

`map_get<THandler>(...)`, `map_post<THandler>(...)`, `map_put<THandler>(...)`,
`map_delete<THandler>(...)`는 handler type을 DI에 등록하고, `request_type`과
`reply_type`의 JSON serializer를 등록하며, HTTP route table에 `method + path`를
연결한다. request마다 DI scope를 만들고 handler를 resolve한다. handler가 반환한 DTO는
JSON response body가 되고, 기본 status는 `200 OK`다.

HTTP handler는 아래 shape를 모두 지원한다.

- typed DTO: `reply_type handle(const request_type &request)`
- typed DTO async: `task_t<reply_type> handle(const request_type &request)`
- typed DTO + context:
  `reply_type handle(const request_type &request, http_context_t &context)`
- typed DTO + context async:
  `task_t<reply_type> handle(const request_type &request, http_context_t &context)`
- typed DTO + request:
  `reply_type handle(const request_type &request, const http_request_t &http)`
- typed DTO + request async:
  `task_t<reply_type> handle(const request_type &request, const http_request_t &http)`
- typed DTO + request + context:
  `reply_type handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- typed DTO + request + context async:
  `task_t<reply_type> handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- typed response: `http_response_t handle(const request_type &request)`
- typed response + context:
  `http_response_t handle(const request_type &request, http_context_t &context)`
- typed response async: `task_t<http_response_t> handle(const request_type &request)`
- typed response + context async:
  `task_t<http_response_t> handle(const request_type &request, http_context_t &context)`
- typed response + request:
  `http_response_t handle(const request_type &request, const http_request_t &http)`
- typed response + request async:
  `task_t<http_response_t> handle(const request_type &request, const http_request_t &http)`
- typed response + request + context:
  `http_response_t handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- typed response + request + context async:
  `task_t<http_response_t> handle(const request_type &request, const http_request_t &http, http_context_t &context)`
- raw HTTP request: `http_response_t handle(const http_request_t &request)`
- raw HTTP request async: `task_t<http_response_t> handle(const http_request_t &request)`

`http_request_t`와 `http_response_t`는 framework public type이다. Raw HTTP handler도
`Boost.Beast` request, socket, SSL stream을 받지 않는다. `map_*<THandler>(...)`는 handler
shape를 compile-time으로 판별한다. typed route에서 여러 overload가 있으면 반환 타입보다
인자 shape를 먼저 본다. `http_request_t`와 `http_context_t`를 모두 받는 shape가 가장 먼저
선택되고, 그 다음 `http_request_t`, `http_context_t`, DTO-only shape 순서로 선택된다.
typed route와 raw route shape를 한 handler에 동시에 제공하면 static assertion 또는 startup
validation으로 실패해야 한다.

route parameter와 query string은 `request_type` DTO에 binding한다. 예를 들어
`/games/{gameId}/moves?actorId=p1`로 들어온 값은 body DTO와 합쳐 handler request가 된다.
같은 필드가 body, route, query에 동시에 있으면 route, query, body 순서로 우선한다. 이
우선순위는 URL에 드러난 식별자가 request body보다 더 명시적인 입력이라는 ASP.NET Core식
route handler 사용성을 따르기 위한 규칙이다.

`use<TMiddleware>()`는 exception, logging, validation, auth, correlation id 같은
cross-cutting 처리를 route handler 앞뒤에 연결한다. middleware/filter는 Beast나 Asio
타입을 받지 않고 `http_context_t`와 framework DTO만 다룬다.
middleware가 `before(http_context_t&)` 또는 `after(http_context_t&)`를 제공하면 runtime은
route handler 전후에 호출한다. request의 `X-Correlation-Id` 또는 `X-Request-Id`는
`http_context_t::correlation_id`로 들어가고 response의 `X-Correlation-Id`로 전파된다.
middleware가 `before(...)`에서 `json_response(...)`를 설정하면 runtime은 handler를 호출하지
않고 해당 JSON response를 반환한다. `map_health(...)`, `map_readiness(...)`,
`map_liveness(...)`는 `app.health()` report를 HTTP endpoint로 노출한다.

`listen(...)`은 `http://`와 `https://` endpoint를 모두 받는다. `https://` endpoint를
사용하면 `configure_tls(...)`로 server certificate와 private key를 설정해야 한다. TLS 설정 public
표면은 파일 경로, PEM data, reload policy 같은 framework 값만 사용하고 OpenSSL 또는
Boost.Asio SSL 타입을 노출하지 않는다.

HTTP runtime은 `hosted_service_t`로 app lifecycle에 묶인다. `Boost.Beast`, `Boost.Asio`,
OpenSSL/SSL context 타입은 runtime 구현에만 있고 public header에는 나타나지 않는다. HTTP error response는
`framework_error_kind_t`를 기반으로 `400`, `404`, `405`, `500`, `503`, `504`로 매핑한다.

handler 안에서 다른 channel로 request를 보낼 때도 호출자는 낮은 수준의 request/reply template
쌍이나 blocking wait를 보지 않아야 한다. `.NET`의 `await client.RequestAsync<TReply>(...)`와
같은 읽기 수준을 C++에서는 아래처럼 표현한다.

샘플 namespace에서는 `using zlink::framework::task_t;`를 두고 `task_t<T>`처럼 짧게 쓴다.
`zlink::framework::task_t<T>`를 handler signature마다 반복하면 async 의미보다 namespace
노이즈가 먼저 보이기 때문이다. framework public contract 문서에서는 전체 이름을 쓸 수 있지만,
application sample과 guide 예제는 짧은 alias를 기본으로 한다.

```cpp
task_t<match_bingo_api_res_t> handle(const match_bingo_api_req_t &request)
{
    allocate_bingo_room_res_t allocated = co_await _client
      .request(sample_names_t::play_channel,
               allocate_bingo_room_req_t { request.mode })
      .submit<allocate_bingo_room_res_t>();

    co_return match_bingo_api_res_t { allocated.room_id };
}
```

샘플 handler는 `.submit().result().value()`로 결과를 직접 꺼내지 않는다. 그런 코드는
handler가 runtime 안에서 blocking wait를 수행하는 것처럼 보이고, 모든 언어 버전에서 같은 async
모델을 제공한다는 목표와 맞지 않다.

```cpp
class order_module_t final : public zlink::framework::module_t {
public:
    void configure(
      zlink::framework::zlink_framework_options_t &options) override
    {
        options.services().add_singleton<order_repository_t>();
        options.services()
          .add_factory<order_service_t, order_repository_t>(
            [](order_repository_t &repository) {
              return std::make_unique<order_service_t>(repository);
        });
        options.services().add_transient<order_handler_t>();
        options.handlers().group("orders").add_publish<order_handler_t>();
    }
};
```

## 5. Configuration 과 Logging

configuration은 JSON, environment variables, CLI args를 기본 Framework 표면으로 둔다.

```cpp
namespace zlink::framework {

enum class optional_t {
    no = 0,
    yes = 1
};

class configuration_model_t {
public:
    configuration_model_t &set(std::string key, std::string value);
    bool contains(std::string_view key) const;
    bool has_section(std::string_view key) const;
    std::optional<std::string> get(std::string_view key) const;
};

class configuration_section_t {
public:
    configuration_section_t(
      const configuration_model_t &model,
      std::string prefix);
    std::string key() const;
    bool contains(std::string_view key) const;
    std::optional<std::string> get(std::string_view key) const;
    std::string require(std::string_view key) const;
};

template <typename T>
concept configuration_bindable =
  requires(const configuration_section_t &section) {
      { T::bind(section) } -> std::same_as<T>;
  };

class config_builder_t {
public:
    configuration_model_t &model() noexcept;
    const configuration_model_t &model() const noexcept;
    config_builder_t &load_json(std::string path);
    config_builder_t &load_json(std::string path, optional_t optional);
    config_builder_t &load_env(std::string prefix);
    config_builder_t &load_cli(int argc, char **argv);
    config_builder_t &use_environment(std::string name);
    std::string environment() const;
    bool is_environment(std::string_view name) const;
    configuration_section_t section(std::string prefix) const;

    template <configuration_bindable T>
    std::optional<T> bind(std::string prefix) const;

    template <configuration_bindable T>
    T bind_required(std::string prefix) const;
};

enum class log_level_t {
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    critical = 5,
    off = 6
};
enum class logging_backend_t { builtin = 0, structured = 1 };
enum class logging_overflow_policy_t {
    drop_debug = 0,
    drop_oldest = 1,
    block = 2
};
struct log_field_t { std::string key; std::string value; };
struct log_record_t {
    log_level_t level = log_level_t::info;
    std::string category;
    std::string message;
    std::vector<log_field_t> fields;
    std::chrono::system_clock::time_point timestamp;
    std::thread::id thread_id;
};
struct logging_async_options_t {
    std::size_t queue_capacity = 8192;
    logging_overflow_policy_t overflow_policy =
      logging_overflow_policy_t::drop_debug;
};
struct rotating_file_options_t {
    std::size_t max_file_size = 10 * 1024 * 1024;
    std::size_t max_files = 5;
};

template <typename TCategory = void>
class logger_t {
public:
    logger_t() = default;
    bool is_enabled(log_level_t level) const noexcept;
    void log(log_level_t level,
      std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    void log_with_fields(log_level_t level,
      std::string message,
      std::vector<log_field_t> fields) const;
    void trace(std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    void debug(std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    void info(std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    void warn(std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    void error(std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    void critical(std::string message,
      std::initializer_list<log_field_t> fields = {}) const;
    const std::string &category() const noexcept;
};

class logger_factory_t {
public:
    logger_factory_t();
    logger_t<> create(std::string category) const;

    template <typename TCategory>
    logger_t<TCategory> create() const;
};

class logging_builder_t {
public:
    using sink_t = std::function<void(const log_record_t &)>;

    logging_builder_t &use_console();
    logging_builder_t &use_file(std::string path);
    logging_builder_t &use_rotating_file(
      std::string path,
      rotating_file_options_t options = {});
    logging_builder_t &use_callback_sink(sink_t sink);
    logging_builder_t &use_provider(std::string name, sink_t sink);
    logging_builder_t &use_async(logging_async_options_t options = {});
    logging_builder_t &use_backend(logging_backend_t backend);
    logging_builder_t &disable_record_capture();
    logging_builder_t &set_max_captured_records(std::size_t max);
    logging_builder_t &set_min_level(log_level_t level);
    logging_builder_t &set_level(std::string level);

    bool console_enabled() const noexcept;
    bool has_output_sink() const noexcept;
    bool async_enabled() const noexcept;
    logging_backend_t backend() const noexcept;
    log_level_t min_level() const noexcept;
    const std::string &level() const noexcept;
    const std::vector<std::string> &file_paths() const noexcept;
    const std::vector<std::string> &provider_names() const noexcept;
    const std::vector<log_record_t> &captured_records() const noexcept;
    logger_factory_t factory() const;
    logger_t<> create_logger(std::string category) const;
};

} // namespace zlink::framework
```

JSON loader는 `nlohmann/json`을 사용한다. YAML은 필요하면 configuration extension으로
둔다. Application은 표준 logging provider와 health 표면을 구성한다. Runtime event DTO,
metric payload callback, exporter lifecycle과 provider 내부 registry는 public contract가 아니다.

### 5.1 Instance Spot metric

Framework가 표준 metric provider에 기록하는 Instance activation 계기는 다음 여섯 이름을 byte 단위로
그대로 사용한다. 종류, 단위, label과 닫힌 outcome 값은
[Runtime metrics §4](../../../../25-runtime-metrics.ko.md#4-object와-stream)가 소유한다.

- `zlink.instance_spot.activations`
- `zlink.instance_spot.activation.duration`
- `zlink.instance_spot.pending.messages`
- `zlink.instance_spot.pending.bytes`
- `zlink.instance_spot.claim.conflicts`
- `zlink.instance_spot.takeovers`

One-way activation 실패는 별도 계기를 만들지 않고 `zlink.mesh_node.messages.dropped`에
`surface=instance_spot`으로 기록한다. `instance_spot_type` label은 startup에 등록한 bounded type 이름만
사용하며 Spot ID, owner ID와 internal authority fields를 label로 사용하지 않는다.

### 5.2 message-flow dispatch error event

미등록 메시지와 dispatch 실패 관측은 메시지 흐름 observer의 `event_id=zlink.dispatch_error`,
`outcome=failed` event로 처리한다.
channel 별, spot 별 observer 등록은 이 버전의 공개 계약이 아니다. request 실패는 reply path 가 있으면
error reply 로 끝나고, local actor call 처럼 reply frame 이 없는 경로는 `task_t` 또는 pending operation
을 framework error 로 완료한다. one-way 실패는 drop 되지만 기본 로그, counter, message-flow event 를 남긴다.

Exact dispatch option declaration은 [Monitoring interface](08-monitoring.ko.md)가 소유한다.

`message_flow_event_t`의 dispatch error event는 `surface`, `message_kind`, `reason`, `action`,
`packet_name`, `channel_name`, `topic`, `spot_id`, `instance_spot_type`, `activation_state`, `actor_id`, `source_rid`,
`correlation_id`를 담는 snapshot이다. native message 소유권과 frame 참조는 포함하지 않는다.

시작 전 기본 mode는 `configure_dispatch().message_flow(...)`에서 정한다. 실행 중 mode 변경은
`app_t::set_message_flow_mode(...)`만 소유하며, channel이나 Spot별 toggle은 제공하지 않는다.

```cpp
app.add_zlink_framework([](auto &options) {
  options.configure_dispatch()
    .set_message_flow_observer(
      std::make_shared<my_message_flow_observer>());
});
```

## 6. HTTP route와 middleware

**HTTP hosting 시나리오는 [60](../60-http-hosting.ko.md)·[61](../61-embedded-http-server.ko.md)이
소유한다.** 여기서는 public 타입만 고정한다.

```cpp
enum class http_method_t { get, post, put, delete_ };

class http_route_t
{
public:
    http_method_t method;
    std::string   path;
    std::string   handler_name;
    bool context_response_precedence = false;  // context가 만든 response를 우선한다
    bool validates_json_content_type = true;   // JSON content type을 검증한다
};

struct http_tls_options_t {
    std::string certificate_file;
    std::string private_key_file;
};
struct http_endpoint_t { std::string uri; std::optional<http_tls_options_t> tls; };
struct http_server_options_t {
    std::size_t max_connections = 1024;
    std::size_t max_request_body_size = 1024 * 1024;
    std::size_t max_header_size = 64 * 1024;
    std::chrono::milliseconds request_headers_timeout{5000};
    std::chrono::milliseconds request_body_timeout{5000};
    std::chrono::milliseconds write_timeout{5000};
    std::chrono::milliseconds keep_alive_timeout{5000};
    std::chrono::milliseconds graceful_shutdown_timeout{5000};
    std::size_t max_keep_alive_requests = 100;
};
struct http_options_snapshot_t {
    std::vector<http_endpoint_t> endpoints;
    std::vector<http_route_t> routes;
    std::vector<std::string> middleware_names;
    http_server_options_t server;
    std::optional<std::string> health_path;
    std::optional<std::string> readiness_path;
    std::optional<std::string> liveness_path;
};

class http_tls_options_builder_t {
public:
    explicit http_tls_options_builder_t(http_tls_options_t &options) noexcept;
    http_tls_options_builder_t &certificate_file(std::string path);
    http_tls_options_builder_t &private_key_file(std::string path);
};

class http_server_options_builder_t {
public:
    explicit http_server_options_builder_t(
      http_server_options_t &options) noexcept;
    http_server_options_builder_t &set_max_connections(std::size_t value);
    http_server_options_builder_t &set_max_request_body_size(std::size_t bytes);
    http_server_options_builder_t &set_max_header_size(std::size_t bytes);
    http_server_options_builder_t &set_request_headers_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_request_body_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_write_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_keep_alive_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_graceful_shutdown_timeout(
      std::chrono::milliseconds value);
    http_server_options_builder_t &set_max_keep_alive_requests(
      std::size_t value);
};
```

- **middleware는 `before`/`after` 쌍이다.** `next` delegate 방식이 아니다 —
  [handler filter](../../../../06-framework-api.ko.md)와 모양이 다르다.
- **middleware 인스턴스는 `create_instance`로 만들고 DI provider를 함께 받는다.**

## 7. Transport

```cpp
enum class transport_scheme_t {
    tcp = 0,
    ipc = 1,
    tls = 2,
    websocket = 3,
    websocket_tls = 4
};

class transport_endpoint_t
{
public:
    transport_endpoint_t (transport_scheme_t scheme, std::string uri);
    transport_scheme_t scheme() const noexcept;
    const std::string &uri() const noexcept;
    static transport_endpoint_t parse(std::string uri);
};
```

**endpoint는 scheme과 URI를 함께 갖는다.** scheme→transport 매핑의 의미는
[Stream Connector §3](../../../../stream-connector/32-stream-connector.ko.md)이 소유한다.

## 8. 등록 builder

**등록 표면은 builder 계층이다.** 각 builder가 자기 역할의 설정만 소유한다.

정식 channel·MeshNode builder declaration은
[Channel messaging](03-channel-messaging.ko.md)이 소유한다. STREAM builder와 압축 option은
[STREAM session](06-stream-session.ko.md), handler·codec·metadata builder는 이 문서의
configuration 등록 표면이 소유한다.

- RouteMesh ChannelName과 classic fanout channel은 서로 다른 namespace와 socket 계약이다.
- Spot·Actor 등록은 owner `mesh_node_builder_t`에 둔다.

drain 중 claim 진행의 의미는 [Graceful Drain §5](../../../../28-graceful-drain-handoff.ko.md)가 소유한다.

## 9. Configuration 조회

**section은 prefix로 잘라낸 view다.** 설정 소스를 계층으로 합치는 규칙은
[01 §5](../01-system-structure.ko.md)가 소유한다.
