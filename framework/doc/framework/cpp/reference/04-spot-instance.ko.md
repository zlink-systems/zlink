# 04. Spot instance

[레퍼런스 목차](README.ko.md)

이 category는 `spot_manager_t`·`route_client_t`·`spot_publisher_client_t`가 제공하는 외부 진입점과,
Spot 코드 안에서 `spot_context_t`/`spot_common_context_t`로 쓰는 진입점을 다룬다. 정확한 signature는
[Spot exact interface](../../common/spec/server/languages/cpp/interfaces/04-spots.ko.md)가 소유한다.

---

## `spot_manager_t::create`

새 User Spot을 항상 새로 만든다. Framework가 새 global SpotId를 발급한다.

```cpp
zlink::framework::spot_create_result_t created = co_await spot_manager
  .create("room")
  .in_mesh("play")
  .creation_request(create_room_t{"ranked"})
  .timeout(std::chrono::seconds{5})
  .submit();

std::string spot_id = created.spot.spot_id();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.in_mesh(mesh_name)` | Object Client·Server role의 Mesh가 하나면 생략 가능 | Spot을 생성할 Mesh. 후보가 둘 이상인데 생략하면 `invalid_operation`, 없으면 `not_configured`, 지정한 Mesh가 없으면 `not_found` |
| `.creation_request(message_t)` / `.creation_request<TRequest>(TRequest)` | 없음(빈 요청) | Spot의 `on_create(...)`에 전달할 생성 요청 |
| `.timeout(milliseconds)` | resolve·factory·initialize 전체에 적용되는 기본값 | 생성 전체가 terminal state가 될 때까지의 상한 |
| `.submit()` | terminal(택 1) | 생성 완료까지 기다린다 |
| `.yield()` | terminal(택 1) | `spot_wide` handler 안에서만 유효 |

**완료 결과.** `spot_create_result_t::state`가 `created`(새로 생성)다. Spot의 `on_create(...)`가
거부하면 `rejected`이고 `reply`에 거부 메시지가 담긴다. 같은 option을 두 번 설정하거나 terminal을
두 번 호출하면 `invalid_operation`, deadline 안에 끝나지 않으면 `deadline_exceeded`다.

**선택 기준.** 항상 새 인스턴스가 필요할 때 쓴다. 있으면 재사용하고 없을 때만 만들려면
`get_or_create`를 쓴다.

---

## `spot_manager_t::get_or_create`

지정한 SpotId의 Ready Spot이 있으면 그것을 반환하고, 없으면 새로 만든다.

```cpp
zlink::framework::spot_create_result_t existing_or_created = co_await spot_manager
  .get_or_create("lobby-eu", "lobby")
  .in_mesh("play")
  .creation_request(create_lobby_t{"eu"})
  .submit();
```

**옵션.** `create`와 동일하다 — `.in_mesh(...)`, `.creation_request(...)`, `.timeout(...)`, terminal
`.submit()` 또는 `.yield()`.

**완료 결과.** `state`가 `existing`이면 이미 있던 Spot을 그대로 반환하고 `creation_request`는
무시한다. `created`면 새로 만든 것이다. 같은 SpotId가 creating 상태로 경합 중이면 그 결과를
기다렸다가 합류하고, cleanup으로 missing이 되면 새 reservation을 다시 경쟁한다. stable type이
기존 authority와 다르면 `type_mismatch`로 완료한다.

**선택 기준.** SpotId로 멱등하게 "있으면 쓰고 없으면 만들기"가 필요할 때 쓴다. 항상 새 인스턴스가
필요하면 `create`를 쓴다.

---

## `spot_manager_t::find` / `close`

기존 Spot을 조회하거나 정확한 incarnation을 닫는다.

```cpp
std::optional<zlink::framework::spot_ref_t> spot =
  co_await spot_manager.find("lobby-eu");

if (spot) {
    bool closed = co_await spot_manager.close(*spot);
}
```

**옵션.** 두 호출 모두 modifier가 없다 — 대상 식별자만 받는다.

**완료 결과.** `find`는 Ready Spot이 없으면 `std::nullopt`을 반환한다. `close`는 해당 incarnation이
없으면 `false`, generation이 다르면 `invalid_operation`, pre-commit seal 중이면 `unavailable`이다.
User Spot에 Actor membership이 남아 있으면 `false`이며 Actor를 자동으로 leave·destroy하지 않는다.

**선택 기준.** 지금 시점의 존재 여부 확인이나 명시적 종료가 필요할 때 쓴다. `close`는 stale
`spot_ref_t`로 다른 incarnation을 대신 닫지 않는다.

---

## `send_to_spot<TMessage>`

Global SpotId 하나로 one-way message를 보낸다. 외부 client(`route_client_t`)와 Spot 코드 안
(`spot_common_context_t`)이 같은 모양을 제공한다.

```cpp
co_await route_client
  .send_to_spot("room-42", player_joined_room_t{"player-1"})
  .submit();

// Instance Spot을 필요하면 새로 활성화(cold activation)해서 보내는 경우
co_await route_client
  .send_to_spot("device-42", device_command_t{"reboot"})
  .instance_spot("device")
  .submit();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.metadata(key, value)` | 없음 | handler에 전달할 key-value |
| `.instance_spot()` | 없음(User Spot만 resolve) | Missing이면 cold activation한다. 등록된 Instance Spot 타입이 하나일 때만 stable type 생략 가능 |
| `.instance_spot(stable_type)` | — | 등록 타입이 여럿이면 stable type을 명시해야 한다 |
| `.in_mesh(mesh_name)` | Object Client·Server role의 Mesh가 하나면 생략 가능 | Missing Instance Spot을 처음 만들 Mesh. Instance marker 없이 쓰면 `invalid_operation` |
| `.submit()` | 필수 terminal | source-local admission까지만 기다린다 |

**완료 결과.** SpotId가 없고 Instance marker도 없으면 `not_found`. `.instance_spot(...)`을 썼는데
existing authority가 User Spot이거나 명시한 타입과 다르면 `type_mismatch`. 그 외 완료 kind는
messaging-execution category의 공통 규칙과 같다.

**선택 기준.** Reply가 필요 없는 Spot 메시징에 쓴다. Reply가 필요하면 `request_to_spot`을 쓴다.

---

## `request_to_spot<TRequest>`

Global SpotId 하나로 typed request/reply를 주고받는다.

```cpp
room_state_t reply = co_await route_client
  .request_to_spot("room-42", get_room_state_t{})
  .timeout(std::chrono::seconds{3})
  .submit<room_state_t>();
```

**옵션.** `send_to_spot`과 동일한 `.instance_spot(...)`/`.in_mesh(...)`에 더해 다음이 있다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.timeout(milliseconds)` | MeshNode의 request 기본 timeout | resolve, cold activation, handler, reply 전체의 deadline |
| `.submit<TReply>()` | terminal(택 1) | reply 수신까지 기다린다 |
| `.yield<TReply>()` | terminal(택 1) | `spot_wide` User Spot·Instance Spot handler 안에서만 유효. 그 밖에서 호출하면 `invalid_operation` |

**완료 결과.** `send_to_spot`과 같은 실패 kind에 더해, cold activation 중 factory나 initialize가
실패하면 typed failure로 완료된다 — Framework가 내부적으로 재시도하지 않는다.

**선택 기준.** Reply 값이 필요할 때 쓴다. One-way면 `send_to_spot`을 쓴다.

---

## `publish<TEvent>` (Spot Logical Multicast)

ChannelName과 topic으로 구독자에게 typed event를 발행한다. `spot_publisher_client_t`(외부)와
`spot_common_context_t::publish`(Spot 코드 안)가 같은 모양을 제공한다.

```cpp
co_await spot_publisher_client
  .publish("room.events", "room-42", room_state_changed_t{"started"});
```

**옵션.** 이 호출에는 `.metadata(...)`(외부 `spot_publisher_client_t`만) 뒤 `submit()`이 필요한
`publish_call_t`를 반환한다 — topic은 필수 인자다.

**완료 결과.** 정상 완료는 발행 admission이 끝났다는 뜻이다. Subscriber 수신은 기다리지 않는다.
messaging-execution category의 classic fanout `publish`와 달리, ChannelName만으로 owner MeshNode를
결정하며 caller가 MeshName을 추가로 넘기지 않는다.

**선택 기준.** Spot 상태 변화를 관찰자에게 알릴 때 쓴다. 구독자에게 직접 reply가 필요하면 이
항목이 아니라 `request_to_spot`을 쓴다.

---

## `add_timer<THandler>` (Spot 코드 안)

Spot에 속한 주기 timer를 등록한다. `spot_common_context_t::add_timer(...)`로 호출한다.

```cpp
zlink::framework::timer_t timer = context_.add_timer<room_tick_handler_t>(
  "room-tick",
  std::chrono::seconds{1},
  zlink::framework::timer_options_t{
      .overrun_policy =
        zlink::framework::timer_overrun_policy_t::skip_late_ticks,
  });
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `options.overrun_policy` | `skip_late_ticks` | tick이 밀렸을 때 건너뛸지, 상한 안에서 따라잡을지, 다음 tick을 늦출지 |
| `options.max_catch_up_ticks` | 1 | `catch_up_bounded`일 때 한 번에 따라잡을 최대 tick 수 |
| `options.stop_on_unhandled_exception` | `false` | handler 예외 시 timer를 멈출지 여부 |

**완료 결과.** `timer_t`를 반환한다. Timer는 이 Spot에 속한 logical registration이라 relocation
때 자동으로 이전되며 application이 target에서 다시 등록할 필요가 없다. `cancel()`로 취소한다.

**선택 기준.** Spot 안에서 주기 작업이 필요할 때 쓴다.

---

## `run_cpu_worker` / `run_io_worker` (Spot 코드 안)

Spot의 owner turn을 막지 않고 별도 worker에서 작업을 실행한다.

```cpp
// CPU-bound 작업은 동기 callable을 받는다.
int result = co_await context_
  .run_cpu_worker([](std::stop_token token) {
      return compute_expensive_score(token);
  })
  .timeout(std::chrono::seconds{2})
  .submit();

// I/O 대기가 있는 작업은 task_t<TResult>를 반환하는 callable을 받는다.
std::string fetched = co_await context_
  .run_io_worker([](std::stop_token token) -> zlink::framework::task_t<std::string> {
      co_return co_await fetch_remote_profile(token);
  })
  .submit();
```

**옵션.** `worker_call_t<TResult>`가 제공하는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.timeout(milliseconds)` | `worker_options_t`의 기본값 | 작업 완료 상한 |
| `.submit()` | terminal(택 1) | 완료까지 기다린다 |
| `.yield()` | terminal(택 1) | `spot_wide` handler 안에서만 유효 |

**완료 결과.** `TResult`를 반환하거나 timeout이면 `deadline_exceeded`로 완료한다. Worker pool
크기(`min_threads`/`max_threads`)와 idle timeout은 host 시작 전에만 설정한다.

**선택 기준.** CPU-bound 계산은 동기 callable을 받는 `run_cpu_worker`, I/O 대기가 있는 작업은
`task_t<TResult>`를 반환하는 callable을 받는 `run_io_worker`를 쓴다. 둘 다 owner turn의 순차
실행을 막지 않으려는 목적이다.

---

## Handler 등록 (Spot 코드 안, `configure()`)

Spot이 받을 packet·request·구독·member Actor 메시지를 처리할 handler를 등록한다.
`spot_context_t::handlers()`(User Spot)/`instance_spot_context_t::handlers()`(Instance Spot)로
호출하며, `configure()` override 안에서만 호출한다.

```cpp
void room_spot_t::configure() {
    context().handlers()
      .add_handler<&room_spot_t::start_game>()
      .add_subscribe<&room_spot_t::on_score_event>("game.scores", "world")
      .add_actor_send<&room_spot_t::on_player_command>();
}
```

**옵션.** Handler가 처리하는 대상에 따라 등록 메서드가 갈린다.

| 대상 | 등록 메서드 |
| --- | --- |
| User Spot 앞 one-way packet·request | `spot_handler_registry_t::add_handler<Method>(packet_name = {})` |
| Logical Multicast 구독 이벤트 | `spot_handler_registry_t::add_subscribe<Method>(channel_name, topic)` |
| User Spot의 member Actor 앞 one-way packet | `spot_handler_registry_t::add_actor_send<Method>(packet_name = {})` |
| User Spot의 member Actor 앞 request | `spot_handler_registry_t::add_actor_request<Method>(packet_name = {})` |
| Instance Spot 앞 packet | `instance_spot_handler_registry_t::add_handler<Method>(packet_name = {})` |

`Method`는 non-type template parameter로 넘기는 member function pointer다(`&room_spot_t::start_game`
형태). C++에는 assembly reflection이 없으므로 handler class를 별도로 만들지 않고 Spot의 member
function을 직접 등록한다.

**완료 결과.** 반환값 없이 동기로 등록된다. Packet name을 생략하면 handler가 처리하는 메시지
타입의 `packet_name`을 사용하고, 없으면 C++ type name을 쓴다. 같은 owner의 handler key 중복은
`app.run(...)`의 startup 검증에서 configuration error로 드러난다.

**선택 기준.** `configure()`가 호출될 때마다 이 Spot이 처리할 모든 handler를 등록한다.
Node·Channel handler는 topology-discovery category의 등록 항목을, STREAM session handler는
stream-session category를 참고한다.

---

## `outbound()` — `send_to_channel` / `request_to_channel` (Spot 코드 안)

Spot 코드 안에서 ChannelName으로 one-way message를 보내거나 typed request/reply를 주고받는다.
`spot_common_context_t::outbound()`가 반환하는 `channel_client_t`가 제공하며
messaging-execution category의 `send_to_channel`/`request_to_channel`과 같은 모양이다.

```cpp
leaderboard_t reply = co_await context_.outbound()
  .request_to_channel("leaderboard.api", get_leaderboard_t{})
  .submit<leaderboard_t>();
```

**옵션.** messaging-execution category의 `send_to_channel`/`request_to_channel`과 동일한 modifier를
받는다.

**완료 결과.** messaging-execution category의 완료 kind와 같다.

**선택 기준.** Spot이 외부 client가 아니라 자기 코드 안에서 다른 ChannelName의 handler를 호출해야
할 때 쓴다. 다른 Spot을 직접 호출하려면 `send_to_spot`/`request_to_spot`을 쓴다.

---

## `leave_actor` / `close` / `destroy_actor` (Spot 코드 안, 종료·이탈)

Member Actor를 이 Spot에서 내보내거나, Spot 자신을 닫거나, Entry Spot에서 Actor를 파기한다.

```cpp
co_await context_.leave_actor(actor);        // User Spot: member Actor만 내보낸다
bool closed = co_await context_.close();     // User·Instance Spot: 이 Spot 자신을 닫는다
co_await entry_context_.destroy_actor(actor); // Entry Spot: Actor를 완전히 파기한다
```

**옵션.** 세 호출 모두 modifier가 없다 — 대상(`leave_actor`/`destroy_actor`)만 받는다.

**완료 결과.** `leave_actor`(`spot_context_t` 전용)는 member Actor membership만 해제하고 Actor
자체는 파기하지 않는다. `close`(`spot_context_t`/`instance_spot_context_t`)는 manager의
`close(spot_ref)`(spot-instance category 앞부분 항목)와 같은 완료 kind를 쓰되, 이 Spot 자신을
대상으로 한다. `destroy_actor`(`entry_spot_context_t` 전용)는 Actor를 완전히 파기한다 —
`leave_actor`와 달리 membership 해제가 아니라 Actor 자체를 없앤다.

**선택 기준.** Member Actor를 다른 곳으로 옮기지 않고 이 Spot에서만 빼려면 `leave_actor`를, Spot
자신을 스스로 종료하려면 `close`를, Entry Spot에서 더 이상 필요 없는 Actor를 완전히 없애려면
`destroy_actor`를 쓴다.

---

## `relocation_ready().defer()` (Spot 코드 안)

`application_signaled` readiness mode를 선택한 `spot_wide` Spot에서, relocation 경계를 다음
application turn 앞으로 미룬다.

```cpp
context_.relocation_ready().defer();
```

**옵션.** 이 호출에는 modifier가 없다.

**완료 결과.** 반환값 없음. 현재 handler가 끝난 뒤 relocation 경계를 등록한다. 이동하지 않았거나
commit 전에 abort했으면 source에서 `continued`, 이동했으면 target에서 `relocated` completion을
`on_relocation_ready_completed(...)`로 받는다. `any_turn_boundary` mode, `per_actor` Spot, Entry·
Instance Spot, Spot turn 밖, 같은 turn의 중복 호출은 `invalid_operation`으로 완료한다.

**선택 기준.** Application이 relocation 시점을 특정 turn 경계로 정밀하게 제어해야 할 때 쓴다.
기본 `any_turn_boundary` mode에서는 이 호출이 필요하지 않다.

---

전체 근거는
[Spot exact interface](../../common/spec/server/languages/cpp/interfaces/04-spots.ko.md)를 참고한다.
