# C++ monitoring exact interface

[C++ exact interface 목차](README.ko.md)

Endpoint, lifecycle generation과 descriptor source는 Framework가 stale 등록 정보와
connection을 판정할 때만 사용한다. Admission·claim·reservation, pending work와
connection intent도 public status에 포함하지 않는다.

## 1. Host lifecycle 관측

Host 단위 상태는 RouteMesh·ClientServer·fanout snapshot과 분리한다. `topology_state_t`는 등록한
topology 하나의 가용성을 나타내며 host lifecycle 상태로 재사용하지 않는다.

RouteMesh peer 상태는 [Channel messaging](03-channel-messaging.ko.md)의
`peer_state_t`를 사용한다. `not_connected`는 연결이 필요하지만 ready connection이
없는 상태다. `not_required`는 두 Object Client 모두 RouteMesh Channel Server membership이 없어
연결이 필요하지 않은 정상 상태다. Channel Client membership만 등록한 경우도 같다. 어느 한쪽에라도
weight `0`을 포함한 Channel Server membership이 있으면 연결 부재는 `not_connected`다. 둘 다 ready peer
수에서 제외하지만 `not_required`는 liveness·health failure 집계에 포함하지 않는다.

```cpp
struct observation_loss_t {
    std::uint64_t coalesced_count = 0;
    std::uint64_t discarded_terminal_count = 0;
};

template <typename TStatus>
struct observed_status_t final {
    TStatus status;
    observation_loss_t loss;
};

struct inbound_dispatch_status_t {
    std::uint64_t application_hwm_bytes;
    std::uint64_t pending_payload_bytes;
    std::uint64_t queued_payload_bytes;
    std::uint64_t active_payload_bytes;
    bool application_receive_paused;
    std::uint64_t pending_completion_sends;
    std::uint64_t completion_send_limit;
};

struct framework_runtime_status_t {
    framework_runtime_state_t state;
    bool is_ready;
    bool accepting_work;
    std::optional<std::chrono::system_clock::time_point> operation_deadline;
    std::optional<relocation_result_t> relocation_result;
    std::optional<termination_result_t> termination_result;
    inbound_dispatch_status_t inbound_dispatch;
    std::uint64_t sequence;
    std::chrono::system_clock::time_point observed_at;
};

class framework_runtime_t {
public:
    virtual ~framework_runtime_t() = default;
    virtual framework_runtime_status_t status() const = 0;
    virtual std::unique_ptr<runtime_observation_t> observe(
      std::size_t capacity,
      std::function<void(
        const observed_status_t<framework_runtime_status_t> &)> observer) = 0;
};
```

`is_ready`는 `state == framework_runtime_state_t::serving`일 때만 `true`다. `accepting_work`는 host가
새 application operation을 받는지를 나타낸다. Status는 application이 lifecycle operation의 결과와
readiness를 판단하는 데 필요한 값만 제공한다. Relocation unit 수, queue, barrier와 Store 내부 상태는
포함하지 않는다.

### 1.1 RouteMesh 상태

```cpp
enum class mesh_node_state_t {
    starting,
    ready,
    degraded,
    stopping,
    stopped,
    failed
};

enum class topology_reason_t {
    runtime_not_ready,
    no_ready_peer,
    no_ready_target,
    location_unavailable,
    capacity_exceeded,
    draining,
    internal_failure
};

enum class peer_state_t {
    connecting,
    ready,
    draining,
    not_connected,
    not_required
};

struct mesh_peer_snapshot_t {
    zlink::routing_id_t node_rid;
    peer_state_t state;
    std::optional<topology_reason_t> unavailable_reason;
};

struct mesh_channel_snapshot_t {
    std::string channel_name;
    bool is_ready;
    std::uint32_t ready_target_count;
};

struct mesh_placement_snapshot_t {
    bool is_available;
    std::uint32_t active_actor_count;
    std::uint32_t active_spot_count;
    std::optional<topology_reason_t> unavailable_reason;
};

struct mesh_node_snapshot_t {
    std::string mesh_name;
    mesh_node_state_t state;
    bool is_ready;
    std::uint32_t ready_peer_count;
    std::vector<mesh_channel_snapshot_t> channels;
    std::vector<mesh_peer_snapshot_t> peers;
    mesh_placement_snapshot_t placement;
    std::uint64_t sequence;
    std::chrono::system_clock::time_point observed_at;
};

class route_mesh_runtime_t {
public:
    virtual mesh_node_snapshot_t snapshot(std::string mesh_name) const = 0;
    virtual std::unique_ptr<mesh_runtime_observation_t> observe(
      std::string mesh_name,
      std::size_t capacity,
      std::function<void(
        const observed_status_t<mesh_node_snapshot_t> &)> observer) = 0;
    virtual bool is_ready(std::string mesh_name) const = 0;
};
```

`observe(...)`가 전달하는 단위는 `observed_status_t<TStatus>`다. `status`는 관찰자
사이에 공유하는 완전한 status·snapshot이고, `loss`는 이 observation 하나에만 해당하는
유실 누계다. 누계를 status에 넣지 않는 이유와 두 counter의 의미는
[Runtime monitoring §3](../../../../24-runtime-monitoring.ko.md#3-현재-상태-조회와-변화-관찰)이 소유한다.

`observation_loss_t::coalesced_count`는 source별 최신 slot 합치기로 이 observer가 보지
못한 중간 status 수이고, `discarded_terminal_count`는 보관 상한 초과로 폐기한 terminal
status 수다. 둘을 하나로 합치지 않는다. 두 counter는 `observe(...)` 호출마다 `0`에서
시작하고 같은 observation 안에서는 단조 증가하며, `9223372036854775807`(`2^63 - 1`)을 넘으면
그 값으로 고정한다. 이 상한은 네 언어가 같다 — Java `long`이 표현할 수 있는 최댓값에 맞췄다. Framework는 observer queue가 가득 찼다는 이유로 observation을
끝내지 않는다.

`observe(...)`는 nullable field를 조합한 범용 event가 아니라 변경 뒤의 완전한
snapshot을 전달한다. Peer 상태는 Node RID, 현재 상태와 사용할 수 없는 이유만
제공한다. Placement는 새 object 수락 가능 여부와 현재 process의 active Actor·Spot
수만 제공한다. Stable type별 capacity, activation concurrency와 reservation failure는
Framework 내부 배치 판단 값이다.

`mesh_placement_snapshot_t::is_available`은 Actor 또는 Spot capacity와 activation
concurrency에 모두 여유가 있을 때만 `true`다. Activation concurrency의 현재 값과
limit은 snapshot에 별도 field로 노출하지 않는다.

## 2. 메시지 흐름 관측

```cpp
enum class message_flow_log_mode_t {
    off = 0,
    errors_only = 1,
    key_transitions = 2,
    verbose = 3,
    diagnostic = 4
};
enum class message_flow_outcome_t {
    received = 0,
    dispatched = 1,
    replied = 2,
    dropped = 3,
    sent = 4,
    reply_received = 5,
    error = 6
};
enum class dispatch_error_surface_t {
    channel = 0,
    route_mesh_channel = 1,
    spot_route = 2,
    spot_subscription = 3,
    spot_actor = 4,
    stream_session = 5
};
enum class dispatch_message_kind_t {
    request = 0,
    send = 1,
    publish = 2,
    response = 3,
    error = 4,
    actor_request = 5,
    actor_send = 6
};
enum class dispatch_error_reason_t {
    handler_missing = 0,
    payload_decode_failed = 1,
    handler_exception = 2,
    invalid_frame = 3,
    reply_path_missing = 4,
    unexpected_reply = 5
};
enum class dispatch_error_action_t {
    reply_error = 0,
    drop = 1,
    fail_caller = 2
};
enum class flow_origin_t : std::uint8_t
{ inbound = 1, timer = 2, application = 3, lifecycle = 4 };
struct message_dispatch_error_event_t {
    dispatch_error_surface_t surface;
    dispatch_message_kind_t message_kind;
    dispatch_error_reason_t reason;
    dispatch_error_action_t action;
    std::optional<std::string> packet_name;
    std::optional<std::string> channel_name;
    std::optional<std::string> topic;
    std::optional<std::string> spot_id;
    std::optional<std::string> actor_id;
    std::optional<std::string> source_rid;
    std::optional<std::string> correlation_id;
    std::exception_ptr exception;
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;
};
struct message_flow_event_t {
    message_flow_outcome_t outcome;
    dispatch_error_surface_t surface;
    dispatch_message_kind_t message_kind;
    std::optional<std::string> packet_name;
    std::optional<std::string> channel_name;
    std::optional<std::string> topic;
    std::optional<std::string> correlation_id;
    std::optional<std::string> source_rid;
    std::optional<std::string> spot_id;
    std::optional<std::string> actor_id;
    std::optional<std::size_t> message_size;
    std::optional<dispatch_error_reason_t> error_reason;
    std::optional<dispatch_error_action_t> error_action;
    std::exception_ptr exception;
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;
};
class message_flow_observer_t {
public:
    virtual ~message_flow_observer_t() = default;
    virtual void on_message_flow(const message_flow_event_t &event) = 0;
};

class dispatch_diagnostics_options_t {
public:
    message_flow_log_mode_t message_flow() const noexcept;
    double sample_rate() const noexcept;
    bool include_message_sizes() const noexcept;
    const std::optional<std::string> &log_file() const noexcept;
    const std::optional<std::string> &label() const noexcept;
    const std::shared_ptr<std::atomic<message_flow_log_mode_t>> &
      live_mode() const noexcept;
    message_flow_log_mode_t effective_message_flow() const noexcept;
};

struct dispatch_options_t {
    dispatch_diagnostics_options_t diagnostics;
    std::shared_ptr<message_flow_observer_t> message_flow_observer;
    std::function<void(const message_flow_event_t &)> message_flow_callback;
    std::optional<logger_t<>> diagnostics_logger;

    dispatch_options_t &set_message_flow_observer(
      std::shared_ptr<message_flow_observer_t> observer);
    dispatch_options_t &set_message_flow_observer(
      std::function<void(const message_flow_event_t &)> observer);
    dispatch_options_t &message_flow(message_flow_log_mode_t mode);
    dispatch_options_t &trace_sample_rate(double rate);
    dispatch_options_t &include_message_sizes(bool include);
    dispatch_options_t &trace_log_file(std::string path);
    dispatch_options_t &trace_label(std::string id);
    dispatch_options_t &message_flow_live(
      std::shared_ptr<std::atomic<message_flow_log_mode_t>> live);
};
```

의미는 [메시지 흐름 추적](../../../../26-message-flow-tracing.ko.md)과
[흐름 상관관계](../../../../27-flow-correlation.ko.md)가 소유한다.

## 3. Health

```cpp
enum class health_status_t {
    healthy = 0,
    degraded = 1,
    unhealthy = 2
};
enum class health_check_scope_t {
    readiness = 0,
    liveness = 1,
    readiness_and_liveness = 2
};

struct health_check_result_t
{
    std::string name, component;
    health_status_t     status = health_status_t::healthy;
    health_check_scope_t scope = health_check_scope_t::readiness_and_liveness;
    std::string message;
};

struct health_report_t
{
    health_status_t status = health_status_t::healthy;
    health_status_t readiness = health_status_t::healthy;
    health_status_t liveness = health_status_t::healthy;
    std::vector<health_check_result_t> checks;
    bool ready () const noexcept;   // readiness != unhealthy
    bool live  () const noexcept;   // liveness  != unhealthy
};

class health_builder_t
{
public:
    health_builder_t();
    ~health_builder_t();
    health_builder_t(health_builder_t &&) noexcept;
    health_builder_t &operator=(health_builder_t &&) noexcept;
    health_builder_t(const health_builder_t &) = delete;
    health_builder_t &operator=(const health_builder_t &) = delete;

    health_builder_t &add_zlink_runtime_check (std::string name = "zlink.runtime");
    health_builder_t &add_channel_check        (std::string name);
    health_builder_t &add_location_check       (std::string name);
    health_builder_t &add_stream_endpoint_check(std::string name);
    health_builder_t &add_hosted_service_check (std::string name);
    health_builder_t &set_status(
      std::string name,
      health_status_t status,
      std::string message = {});

    health_report_t report () const;
};
```

**`readiness`와 `liveness`를 분리한다.** 트래픽을 받을 준비(readiness)와 프로세스 생존(liveness)은
다른 질문이다. **`degraded`는 `ready()`·`live()`를 막지 않는다.**

## 4. Structured logging과 metric provider 경계

Application은 [Configuration과 host](02-configuration-host.ko.md)의
`logging_builder_t`로 표준 logging provider를 구성한다. Runtime 상태 변화와 진단 정보는
`log_record_t`의 identifier와 field로 전달한다.

다음 타입과 등록 API는 public contract가 아니다.

- socket·Spot·Actor·STREAM별 raw event DTO
- raw event handler와 source 등록 builder
- metric sample DTO와 application callback
- exporter lifecycle, registry와 provider 내부 상태

Peer와 Channel의 현재 상태는 `route_mesh_runtime_t`의 snapshot과 observation으로 확인한다.
Host 상태는 `app_t::runtime_state()`, `is_ready()`, `relocate(...)`와 `shutdown(...)` 결과로
확인한다. Metric 이름, 종류, 단위와 label은
[Runtime metric과 집계 규칙](../../../../25-runtime-metrics.ko.md)이 소유한다.
