# 05. Actor relocation

[레퍼런스 목차](README.ko.md)

이 category는 `actor_manager_t`·`actor_client_t`가 제공하는 외부 진입점과, Actor 코드 안에서
`actor_context_t`로 Spot에 참여하는 진입점, 그리고 relocation 정책 선택을 다룬다. 정확한
signature는 [Actor exact interface](../../common/spec/server/languages/cpp/interfaces/05-actors.ko.md)가
소유한다.

---

## `actor_manager_t::create`

새 Actor를 항상 새로 만든다.

```cpp
zlink::framework::actor_create_result_t created = co_await actor_manager
  .create(zlink::framework::actor_id_t{"player-1"}, "player")
  .in_mesh("play")
  .creation_request(spawn_player_t{"player-1"})
  .submit();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.in_mesh(mesh_name)` | Object Client·Server role의 Mesh가 하나면 생략 가능 | Actor를 생성할 Mesh. 후보가 둘 이상인데 생략하면 `invalid_operation`, 없으면 `not_configured`, 지정한 Mesh가 없으면 `not_found` |
| `.creation_request(message_t)` / `.creation_request<TCreation>(TCreation)` | 없음(빈 요청) | Actor factory 생성 시점에 전달할 요청 |
| `.timeout(milliseconds)` | 5초 | resolve·reservation·factory·Ready barrier 전체의 deadline |
| `.submit()` | terminal(택 1) | 생성 완료까지 기다린다 |
| `.yield()` | terminal(택 1) | `spot_wide` handler 안에서만 유효 |

**완료 결과.** `actor_create_result_t`(`std::variant`)는 `actor_create_created_t`(새로 생성) 또는
`actor_create_rejected_t`(factory가 거부) 중 하나로 완료한다. 같은 ActorId의 Ready incarnation이
이미 있으면 두 대안이 아니라 `already_exists` 오류로 완료한다 — `actor_create_existing_t`는
`get_or_create`에만 있다. Ready incarnation이 있는데 stable type이 다르면 `type_mismatch`다.

**선택 기준.** 항상 새 Actor가 필요할 때 쓴다. 있으면 재사용하고 없을 때만 만들려면
`get_or_create`를 쓴다.

---

## `actor_manager_t::get_or_create`

같은 ActorId의 Ready Actor가 있으면 그것을 반환하고, 없으면 새로 만든다.

```cpp
zlink::framework::actor_create_result_t existing_or_created = co_await actor_manager
  .get_or_create(zlink::framework::actor_id_t{"player-1"}, "player")
  .in_mesh("play")
  .creation_request(spawn_player_t{"player-1"})
  .submit();
```

**옵션.** `create`와 동일하다 — `.in_mesh(...)`, `.creation_request(...)`, `.timeout(...)`, terminal
`.submit()` 또는 `.yield()`.

**완료 결과.** `actor_create_existing_t`이면 이미 있던 Actor를 반환하고 `creation_request`는
무시한다. Creating attempt와 경합하면 그 결과를 기다렸다가 합류하며, 서로 다른 operation은 Ready
뒤 `actor_create_existing_t`를 받고 이전 reply를 공유하지 않는다.

**선택 기준.** ActorId로 멱등하게 "있으면 쓰고 없으면 만들기"가 필요할 때 쓴다.

---

## `find` / `find_spot` / `destroy` (manager)

기존 Actor를 조회하거나, 참여 중인 Spot을 조회하거나, 정확한 incarnation을 종료한다.

```cpp
std::optional<zlink::framework::actor_ref_t> actor =
  co_await actor_manager.find(zlink::framework::actor_id_t{"player-1"});
std::optional<zlink::framework::spot_ref_t> spot =
  co_await actor_manager.find_spot(zlink::framework::actor_id_t{"player-1"});

if (actor) {
    bool destroyed = co_await actor_manager.destroy(*actor);
}
```

**옵션.** 세 호출 모두 modifier가 없다 — 대상 식별자만 받는다.

**완료 결과.** `find`는 Ready Actor가 없으면 `std::nullopt`을 반환한다. `find_spot`은 current User
Spot membership이 없으면 `std::nullopt`을 반환한다. `destroy`는 해당 incarnation이 없으면
`false`, generation이 다르면 `invalid_operation`, pre-commit seal 중이면 `unavailable`이다.

**선택 기준.** 지금 시점의 존재·소속 확인이나 명시적 종료가 필요할 때 쓴다.

---

## `send` / `request` (actor_client_t)

Global ActorId 하나로 one-way message를 보내거나 typed request/reply를 주고받는다. 외부 client에서
쓴다.

```cpp
co_await actor_client
  .send(zlink::framework::actor_id_t{"player-1"}, grant_item_t{"sword"})
  .submit();

inventory_t reply = co_await actor_client
  .request(zlink::framework::actor_id_t{"player-1"}, get_inventory_t{})
  .timeout(std::chrono::seconds{3})
  .submit<inventory_t>();
```

**옵션.** `send`는 `.metadata(...)`와 terminal `.submit()`만 있다. `request`는 다음이 더 있다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.timeout(milliseconds)` | MeshNode의 request 기본 timeout | reply를 기다리는 상한 |
| `.submit<TReply>()` | terminal(택 1) | reply 수신까지 기다린다 |
| `.yield<TReply>()` | terminal(택 1) | `spot_wide` handler 안에서만 유효 |
| `.submit_message()` / `.yield_message()` | terminal(택 1) | typed reply 대신 raw `message_t`로 받는다 |

**완료 결과.** ActorId가 없으면 `not_found`. 나머지 완료 kind는 messaging-execution category의
공통 규칙과 같다.

**선택 기준.** Reply가 필요 없으면 `send`, 필요하면 `request`를 쓴다.

---

## `join_spot` / `join_entry_spot` (Actor 코드 안)

현재 Actor를 User Spot 또는 Entry Spot에 참여시킨다. `actor_context_t::join_spot(...)`/
`join_entry_spot(...)`로 호출하며, 다른 항목과 달리 terminal이 `submit`/`yield`가 아니라 `defer()`
하나뿐이다.

```cpp
context_
  .join_spot("room-42", join_room_request_t{"player-1"})
  .timeout(std::chrono::seconds{5})
  .defer();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.timeout(milliseconds)` | 5초 | monotonic absolute deadline |
| `.defer()` | 필수 terminal | 결과 없는 동기 호출. Join intent와 비활성 barrier만 등록하고 target 조회를 바로 시작하지 않는다 |

**완료 결과.** `defer()` 자체는 반환값이 없다. 현재 handler가 정상적으로 끝나면 barrier가
활성화되어 Join을 실행하고, handler가 실패하면 barrier를 폐기한다. 실제 결과(수락·거부·실패)는
같은 128-bit operation ID를 담은 `actor_t::on_join_completed(...)` callback으로 비동기 전달된다 —
`actor_join_accepted_t`/`actor_join_rejected_t`/`actor_join_failed_t` `std::variant` 중 하나다.

**선택 기준.** Actor를 다른 Spot으로 옮기거나 Entry Spot으로 되돌릴 때 쓴다. Entry Spot과
`per_actor` User Spot의 Actor에서 호출하면 `invalid_operation`으로 완료한다.

---

## Relocation 정책 선택 (Actor factory 등록 시점)

`add_actor_factory<TActor, TFactory>(...)`(topology-discovery category)의 `configure` callback에서
정확히 하나를 선택한다.

| 정책 | cross-node 이동 시 동작 | 선택 기준 |
| --- | --- | --- |
| `disable_relocation()` | Capture 전에 이동 자체를 거부한다 | 이 Actor가 다른 node로 옮겨지면 안 될 때 |
| `recreate_on_relocation()` | Target factory로 같은 logical identity를 다시 만든다. Application state는 복구하지 않는다 | State 없이 다시 만들어도 되는 Actor일 때 |
| `preserve_state_with<TAdapter>()` | `actor_relocation_adapter_t<TActor>::capture`/`restore`로 opaque byte vector를 옮긴다 | State를 유지한 채 옮겨야 할 때 |

**완료 결과.** `preserve_state_with`의 `capture(...)` 결과는 최대 64 MiB다. Capture·Restore는 같은
relocation에서 여러 번 호출될 수 있으므로 두 callback 모두 retry-safe해야 한다 — 외부 side
effect의 exactly-once 실행에 의존하면 안 된다.

**선택 기준.** 세 정책 중 무엇을 고르느냐가 이 Actor 타입의 relocation 동작 전체를 결정한다 —
factory 등록 시점에 한 번만 정하고 나중에 호출별로 바꿀 수 없다.

---

전체 근거는
[Actor exact interface](../../common/spec/server/languages/cpp/interfaces/05-actors.ko.md)를
참고한다.
