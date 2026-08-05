# C++ Actor exact interface

Session에 bind된 Actor를 포함한 Spot relocation은 target에서 Actor와 queue를 복원하고 owner와
membership을 commit한 뒤 message 처리를 시작한다. Target runtime은
`sessionActorLocationUpdateReqMsg`를 send하여 binding route와 bound-session current Actor
location snapshot을 갱신한다. 응답이 없어도 Actor 처리를 멈추지 않으며 정해진 간격으로
같은 요청을 다시 보낸다. Snapshot은 target MeshName·NodeRid를 제공한다. Relocation 자체는 physical·logical disconnect가
아니므로 Actor disconnect callback을 실행하지 않는다. relocation 대상에 포함되지 않은 다른 Actor의 route와 physical connection은
변경하지 않는다.

[C++ exact interface 목차](README.ko.md) · [Actor model](../../../../14-actor-model.ko.md) ·
[Spot·Actor membership](../../../../15-spot-actor.ko.md)

## 1. Identity와 maintenance policy

Actor가 사용하는 `actor_context_t`의 exact declaration은
[Spot interface](04-spots.ko.md)가 소유한다.

```cpp
namespace zlink::framework {

class actor_id_t final {
public:
    explicit actor_id_t(std::string value);
    std::string_view value() const noexcept;
    auto operator<=>(const actor_id_t &) const = default;
};

class actor_ref_t final {
public:
    actor_ref_t(actor_id_t actor_id,
      std::uint64_t object_generation,
      std::string mesh_name,
      node_rid_t node_rid);

    const actor_id_t &actor_id() const noexcept;
    std::uint64_t object_generation() const noexcept;
    std::string_view mesh_name() const noexcept;
    const node_rid_t &node_rid() const noexcept;
};

struct actor_join_accepted_t {
    std::uint64_t operation_id_high;
    std::uint64_t operation_id_low;
    actor_ref_t actor;
    std::optional<message_t> reply;
};

struct actor_join_rejected_t {
    std::uint64_t operation_id_high;
    std::uint64_t operation_id_low;
    std::optional<message_t> reply;
};

struct actor_join_failed_t {
    std::uint64_t operation_id_high;
    std::uint64_t operation_id_low;
    framework_error_kind_t error_kind;
};

using actor_join_completion_t = std::variant<
  actor_join_accepted_t,
  actor_join_rejected_t,
  actor_join_failed_t>;

class actor_t {
public:
    virtual ~actor_t() = default;
    virtual actor_context_t &context() noexcept = 0;
    virtual const actor_context_t &context() const noexcept = 0;
    virtual void configure() {}
    virtual task_t<void> on_join_completed(
      const actor_join_completion_t &completion);
};

template <typename TActor>
  requires std::derived_from<TActor, actor_t>
class actor_factory_t {
public:
    virtual ~actor_factory_t() = default;
    virtual task_t<std::shared_ptr<TActor>> create(
      actor_context_t context,
      std::stop_token operation_cancellation) = 0;
};

template <typename TActor>
class actor_relocation_adapter_t {
public:
    virtual ~actor_relocation_adapter_t() = default;
    virtual task_t<std::vector<std::byte>> capture(
      TActor &actor,
      std::stop_token operation_cancellation) = 0;
    virtual task_t<void> restore(
      TActor &actor,
      std::vector<std::byte> payload,
      std::stop_token operation_cancellation) = 0;
};

template <typename TActor>
class actor_factory_builder_t {
public:
    void disable_relocation();
    void recreate_on_relocation();
    template <typename TAdapter>
      requires std::derived_from<TAdapter, actor_relocation_adapter_t<TActor>>
    void preserve_state_with();
};

} // namespace zlink::framework
```

`actor_id_t`는 UTF-8 `1..255` byte exact global identity다. Constructor는 invalid 값을
`std::invalid_argument`로 거부하고 trim, case folding과 Unicode normalization을 적용하지 않는다.
`actor_ref_t`는 global ActorId, non-zero `1..9223372036854775807` ObjectGeneration과 조회 시점의
MeshName·NodeRid를 담는 immutable location snapshot이다. 일반 message target으로 사용하지 않는다. 별도
`actor_ref_snapshot_t`는 제공하지 않는다.

`actor_t`는 Framework가 연결한 `actor_context_t`를 소유하는 typed lifecycle
base다. ActorId와 ObjectGeneration은 `context()`에서 읽으며 별도 identity 값을
독립적으로 저장하지 않는다. Framework는 `actor_factory_t<TActor>::create(...)`로
concrete Actor를 만든 뒤 `configure()`를 호출한다. Factory는 전달받은 exact
context와 cancellation만 사용하며 ActorId, 다른 owner RID, relocation phase 또는
Store token을 중복 입력으로 받지 않는다.

Join completion의 128-bit operation ID는 completion idempotency ID이며
`RelocationId`, reservation ID나 aggregate commit ID가 아니다. Same-node와
cross-node completion retry는 current source와 target process lifetime으로 제한한다.
Process 종료 뒤 다른 runtime이 completion을 자동 replay하지 않는다.

모든 Actor factory configure callback은 policy를 정확히 하나 선택한다. `preserve_state_with<TAdapter>()`의 `TAdapter`는
`actor_relocation_adapter_t<TActor>`를 구현해야 하며 다른 adapter type이면 socket bind 전에 configuration error로
실패한다. Adapter는 application state를 opaque byte vector로만 주고받으며 typed state, 별도 contract identifier,
message wrapper, authority, relocation reference, relocation phase와 operation ID를 받지 않는다.

Framework는 `preserve_state_with<TAdapter>()`의 cross-node Actor materialization에서만 adapter를 호출한다. 여기에는 maintenance
이관, remote User·Entry Spot join과 whole User Spot relocation의 각 Actor participant가 포함된다. Same-node join과
relocation에서는 adapter를 호출하지 않으며 `DisableRelocation` cross-node operation은 `capture(...)` 전에 거부한다.
`RecreateOnRelocation` policy도 application payload를 capture하거나 restore하지 않는다. Whole User Spot relocation에서는 Spot root에
`spot_relocation_adapter_t<TSpot>`를 사용하고 각 Actor participant에는 이 Actor adapter를 사용한다.

`capture(...)` 결과는 최대 64 MiB이며 빈 vector는 유효하다. 반환한 byte vector의 소유권은 Framework로 이동하고,
`restore(...)`에 전달한 byte vector는 해당 비동기 호출이 소유한다. Capture가 throw하거나 failed task로 끝나면
durable abort와 source normalization 뒤 admission을 복원한다. Restore가 실패한 instance는 폐기하고 새 attempt의
factory가 만든 instance에 같은 immutable payload를 적용한다. Framework가 operation deadline 때문에 callback을
취소하면 `deadline_exceeded`로 분류한다. 같은 source와 target process 안의 재시도 때문에 두 method는 두 번
이상 호출될 수 있으므로 구현은 retry-safe해야 한다. 다른 target을 자동 선택하지 않는다. Framework는
adapter의 external side effect에 exactly-once를 보장하지 않는다.

## 2. ID-only messaging

```cpp
namespace zlink::framework {

class actor_send_call_t {
public:
    actor_send_call_t &metadata(std::string key, std::string value);
    task_t<void> submit();
};

class actor_request_call_t {
public:
    actor_request_call_t &timeout(std::chrono::milliseconds timeout);
    actor_request_call_t &metadata(std::string key, std::string value);

    template <typename TReply>
    task_t<TReply> submit();

    template <typename TReply>
    task_t<TReply> yield();

    task_t<message_t> submit_message();
    task_t<message_t> yield_message();
};

class actor_client_t {
public:
    virtual ~actor_client_t() = default;

    template <typename TMessage>
    actor_send_call_t send(actor_id_t actor_id, TMessage message);

    template <typename TRequest>
    actor_request_call_t request(actor_id_t actor_id, TRequest request);
};

} // namespace zlink::framework
```

Actor send와 request는 global `actor_id_t`만 target으로 받는다. [MeshName](../../../../01-glossary.ko.md#meshname), ActorRef, [owner](../../../../01-glossary.ko.md#owner) NodeRid와 current
SpotId를 받는 overload는 없다. Runtime은 positive Ready route만 cache하고 negative cache를 두지 않는다.
Missing route는 `not_found`, exact-ref generation mismatch는 `invalid_operation`으로 구분한다.

## 3. Single-use manager operation

```cpp
namespace zlink::framework {

struct actor_create_existing_t {
    actor_ref_t actor;
};

struct actor_create_created_t {
    actor_ref_t actor;
    std::optional<message_t> reply;
};

struct actor_create_rejected_t {
    std::optional<message_t> reply;
};

using actor_create_result_t = std::variant<
  actor_create_existing_t,
  actor_create_created_t,
  actor_create_rejected_t>;

class actor_create_call_t {
public:
    actor_create_call_t(actor_create_call_t &&) noexcept;
    actor_create_call_t &operator=(actor_create_call_t &&) noexcept;
    actor_create_call_t(const actor_create_call_t &) = delete;
    actor_create_call_t &operator=(const actor_create_call_t &) = delete;

    actor_create_call_t &in_mesh(std::string mesh_name);
    actor_create_call_t &creation_request(message_t request);

    template <typename TCreation>
    actor_create_call_t &creation_request(TCreation request);

    actor_create_call_t &timeout(std::chrono::milliseconds timeout);
    task_t<actor_create_result_t> submit();
    task_t<actor_create_result_t> yield();
};

class actor_manager_t {
public:
    virtual ~actor_manager_t() = default;
    virtual actor_create_call_t create(
      actor_id_t actor_id, std::string stable_type) = 0;
    virtual actor_create_call_t get_or_create(
      actor_id_t actor_id, std::string stable_type) = 0;
    virtual task_t<std::optional<actor_ref_t>> find(actor_id_t actor_id) = 0;
    virtual task_t<std::optional<spot_ref_t>> find_spot(
      actor_id_t actor_id) = 0;
    virtual task_t<bool> destroy(actor_ref_t actor) = 0;
};

} // namespace zlink::framework
```

Call object는 option마다 최대 한 번 설정하고 `submit()`도 한 번만 호출한다. Duplicate option은
`invalid_operation`, 두 번째 submit도 `invalid_operation`이다. `in_mesh`를 생략했을 때 object role Mesh가
하나면 자동 선택하고, 0개면 `not_configured`, 여러 개면 `invalid_operation`이다. Unknown
Mesh는 `not_found`다.

`Create`는 existing identity에 `already_exists`를 반환하고 새 attempt에서는
`actor_create_created_t` 또는 `actor_create_rejected_t`를 반환한다. `GetOrCreate`는
같은 stable type의 [Ready](../../../../01-glossary.ko.md#ready) Actor를 callback 없이
`actor_create_existing_t`로 반환한다. Creating이면 authority 변경을 기다리며 CAS loser는
별도 factory나 callback을 시작하지 않는다. 서로 다른 operation은 Ready 뒤
`actor_create_existing_t`를 받고 cleanup 뒤 새 reservation을 경쟁하며 앞선 application
reply를 공유하지 않는다. 같은 source Node RID·lifecycle generation·`OperationId`의
재전송만 correlation-free `creation-operation-terminal-v1` envelope를 읽고 현재
correlation·reply route로 reply를 다시 encode한다. Terminal은 original deadline 뒤 5분
동안 유지한다. Callback exception은 rejected result가
아니라 typed creation failure다. Type이 다르면 `type_mismatch`다.
[Deadline](../../../../01-glossary.ko.md#deadline)은 resolve, reservation, factory와
Ready 전체에 적용한다. `Find`는 Ready ref만 반환하며 생성하지 않는다. `FindSpot`은 current User Spot
membership의 Ready `spot_ref_t`만 반환하고 Entry [membership](../../../../01-glossary.ko.md#membership) 또는 Missing Actor에는 빈 optional을 반환한다.
`Destroy`는 exact ActorRef만 변경한다.
같은 incarnation이 없으면 `false`, 다른 generation은 `invalid_operation`, 이동 중이면 `unavailable`이다.
Public Actor directory와 local Actor bind overload는 제공하지 않는다.

Actor creation은 selected owner MeshNode의 [Entry Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot) membership을 Ready barrier 안에서 함께 확정한다. Actor
업무 payload는 membership 종류와 관계없이 Actor queue로 직접 전달하며 Entry Spot callback을 경유하지 않는다.
Original creation payload와 일반 message는 다른 owner나 새 incarnation으로 hidden retry하지 않는다. Caller가
timeout, cancellation 또는 moving 결과를 받으면 새 operation을 명시적으로 시작해야 한다.

## 4. STREAM exact-ref binding

```cpp
namespace zlink::framework {

class session_actor_t {
public:
    const actor_ref_t &ref() const noexcept;
    task_t<void> relay(const message_t &payload);
    task_t<void> relay(
      const session_message_context_t &context,
      const message_t &payload);
    task_t<void> notify_disconnected();
};

class session_actor_manager_t {
public:
    std::vector<session_actor_t> bound() const;
    std::optional<session_actor_t> find(actor_id_t actor_id) const;
    request_call_t<session_actor_t> bind(actor_ref_t actor_ref);
    request_call_t<session_actor_t> bind_or_get(actor_ref_t actor_ref);
};

} // namespace zlink::framework
```

Bind는 caller가 제출한 exact ActorRef 위치로 한 번만 control request를 보낸다. Stale·moving 결과에서 global
ActorId를 다시 lookup하거나 fresh incarnation으로 자동 bind하지 않는다. `find(...)`는 해당 STREAM session에
이미 bind된 Actor만 조회하며 global Actor directory가 아니다.

Actor relocation이 commit되면 `session_actor_t::ref()`는 같은 ActorId·ObjectGeneration과 target
MeshName·NodeRid를 가진 current location snapshot을 반환하고, 저장된 binding route도 같은 시점에
갱신된다. Caller가 복사해 보관한 이전 `actor_ref_t` 값은 변경하지 않는다. Application은 relocation을
알기 위해 `bind(...)`를 다시 호출하지 않는다.

현재 STREAM binding을 통한 one-way push는 connection-bound operation이다. 유효한 binding이 없거나 connection
generation이 바뀌면 session-not-bound 또는 stale 결과로 끝나며, Framework가 다른 session을 찾아 다시
제출하지 않는다. Connection 종료는 Actor의 [Spot](../../../../01-glossary.ko.md#spot) membership을 바꾸거나 Actor를 자동 종료하지 않는다.

## 5. Public trace category

이 문서의 declaration은 public trace의 `actor-relocation` category에 속한다. 공통 의미는
[Actor model](../../../../14-actor-model.ko.md), [Spot·Actor membership](../../../../15-spot-actor.ko.md)과
[Session Actor dispatch](../../../../20-session-actor-dispatch.ko.md)가 소유한다.

이 문서에 선언된 `yield()`와 `yield_message()`는 현재 Actor handler가 `SpotWide` User Spot의 shared
execution gate에서 실행 중일 때만 유효하다. Entry Spot Actor와 `PerActor` User Spot의 Actor가 호출하면
operation을 제출하거나 turn을 반환하지 않고 `invalid_operation`으로 완료한다. `submit()`은 모든 Actor
실행 문맥에서 사용할 수 있다.
