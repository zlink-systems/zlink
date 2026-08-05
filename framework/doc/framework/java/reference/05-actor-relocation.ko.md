# 05. Actor relocation

[레퍼런스 목차](README.ko.md)

이 category는 `ZLinkActorManager`·`ZLinkActorClient`가 제공하는 외부 진입점과, Actor 코드 안에서
`ZLinkActorContext`로 Spot에 참여하는 진입점, 그리고 relocation 정책 선택을 다룬다. 정확한
signature는
[Java Actor exact interface](../../common/spec/server/languages/java/interfaces/actors.ko.md)가
소유한다.

---

## `ZLinkActorManager.create`

새 Actor를 항상 새로 만든다.

```java
ZLinkActorCreateResult created = actorManager.create("player-1", "player")
    .inMesh("play")
    .request(new SpawnPlayer("player-1"))
    .submit()
    .toCompletableFuture().get();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.inMesh(meshName)` | Object Client·Server role의 Mesh가 하나면 생략 가능 | Actor를 생성할 Mesh. 후보가 둘 이상인데 생략하면 `INVALID_OPERATION`, 없으면 `NOT_CONFIGURED`, 지정한 Mesh가 없으면 `NOT_FOUND` |
| `.request(Object)` / `.request(ZLinkMessage)` | 없음(빈 요청) | Actor factory 생성 시점에 전달할 요청 |
| `.timeout(Duration)` | 5초 | resolve·reservation·factory·Ready barrier 전체의 deadline |
| `.submit()` | terminal(택 1) | 생성 완료까지 기다린다 |
| `.yield()` | terminal(택 1) | `SpotWide` handler 안에서만 유효 |

**완료 결과.** `ZLinkActorCreateResult`(sealed interface)는 `Created`(새로 생성) 또는
`Rejected`(factory가 거부) 중 하나로 완료한다. 같은 ActorId의 Ready incarnation이 이미 있으면
두 대안이 아니라 `ALREADY_EXISTS` 오류로 완료한다 — `Existing`은 `getOrCreate`에만 있다. Ready
incarnation이 있는데 stable type이 다르면 `TYPE_MISMATCH`다.

**선택 기준.** 항상 새 Actor가 필요할 때 쓴다. 있으면 재사용하고 없을 때만 만들려면
`getOrCreate`를 쓴다.

---

## `ZLinkActorManager.getOrCreate`

같은 ActorId의 Ready Actor가 있으면 그것을 반환하고, 없으면 새로 만든다.

```java
ZLinkActorCreateResult existingOrCreated = actorManager.getOrCreate("player-1", "player")
    .inMesh("play")
    .request(new SpawnPlayer("player-1"))
    .submit()
    .toCompletableFuture().get();
```

**옵션.** `create`와 동일하다 — `.inMesh(...)`, `.request(...)`, `.timeout(...)`, terminal
`.submit()` 또는 `.yield()`.

**완료 결과.** `Existing`이면 이미 있던 Actor를 반환하고 `request`는 무시한다. Creating attempt와
경합하면 그 결과를 기다렸다가 합류하며, 서로 다른 operation은 Ready 뒤 `Existing`을 받고 이전
reply를 공유하지 않는다.

**선택 기준.** ActorId로 멱등하게 "있으면 쓰고 없으면 만들기"가 필요할 때 쓴다.

---

## `find` / `findSpot` / `destroy` (manager)

기존 Actor를 조회하거나, 참여 중인 Spot을 조회하거나, 정확한 incarnation을 종료한다.

```java
Optional<ActorRef> actor = actorManager.find("player-1").toCompletableFuture().get();
Optional<SpotRef> spot = actorManager.findSpot("player-1").toCompletableFuture().get();

if (actor.isPresent()) {
    boolean destroyed = actorManager.destroy(actor.get()).toCompletableFuture().get();
}
```

**옵션.** 세 호출 모두 modifier가 없다 — 대상 식별자만 받는다.

**완료 결과.** `find`는 Ready Actor가 없으면 `Optional.empty()`를 반환한다. `findSpot`은 current
User Spot membership이 없으면 `Optional.empty()`를 반환한다. `destroy`는 해당 incarnation이 없으면
`false`, generation이 다르면 `INVALID_OPERATION`, pre-commit seal 중이면 `UNAVAILABLE`이다.

**선택 기준.** 지금 시점의 존재·소속 확인이나 명시적 종료가 필요할 때 쓴다.

---

## `sendToActor` / `requestToActor` (ZLinkActorClient)

Global ActorId 하나로 one-way message를 보내거나 typed request/reply를 주고받는다. 외부 client에서
쓴다.

```java
actorClient.sendToActor("player-1", new GrantItem("sword")).submit();

CompletionStage<Inventory> reply = actorClient
    .requestToActor("player-1", new GetInventory())
    .timeout(Duration.ofSeconds(3))
    .submit(Inventory.class);
```

**옵션.** `sendToActor`는 `.metadata(...)`와 terminal `.submit()`만 있다. `requestToActor`는
다음이 더 있다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.timeout(Duration)` | MeshNode의 request 기본 timeout | reply를 기다리는 상한 |
| `.submit(TReply.class)` | terminal(택 1) | reply 수신까지 기다린다 |
| `.yield(TReply.class)` | terminal(택 1) | `SpotWide` handler 안에서만 유효 |

**완료 결과.** ActorId가 없으면 `NOT_FOUND`. 나머지 완료 kind는 messaging-execution category의
공통 규칙과 같다.

**선택 기준.** Reply가 필요 없으면 `sendToActor`, 필요하면 `requestToActor`를 쓴다.

---

## `joinSpot` / `joinEntrySpot` (Actor 코드 안)

현재 Actor를 User Spot 또는 Entry Spot에 참여시킨다. `ZLinkActorContext.joinSpot(...)`/
`joinEntrySpot(...)`로 호출하며, 다른 항목과 달리 terminal이 `submit`/`yield`가 아니라 `defer()`
하나뿐이다.

```java
context.joinSpot("room-42", new JoinRoomRequest("player-1"))
    .timeout(Duration.ofSeconds(5))
    .defer();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.timeout(Duration)` | 5초 | monotonic absolute deadline |
| `.defer()` | 필수 terminal | 결과 없는 동기 호출. Join intent와 비활성 barrier만 등록하고 target 조회를 바로 시작하지 않는다 |

**완료 결과.** `defer()` 자체는 반환값이 없다. 현재 handler가 정상적으로 끝나면 barrier가
활성화되어 Join을 실행하고, handler가 실패하면 barrier를 폐기한다. 실제 결과(수락·거부·실패)는
같은 128-bit `ZLinkActorJoinOperationId`를 담은 `ZLinkActor.onJoinCompleted(...)` callback으로
비동기 전달된다 — `Accepted`/`Rejected`/`Failed`(sealed interface `ZLinkActorJoinCompletion`) 중
하나다.

**선택 기준.** Actor를 다른 Spot으로 옮기거나 Entry Spot으로 되돌릴 때 쓴다. Entry Spot과
`PER_ACTOR` User Spot의 Actor에서 호출하면 `INVALID_OPERATION`으로 완료한다.

---

## Relocation 정책 선택 (Actor factory 등록 시점)

`addActorFactory(actorType, actorClass, factoryClass, configure)`(topology-discovery category)의
`configure` callback에서 정확히 하나를 선택한다.

| 정책 | cross-node 이동 시 동작 | 선택 기준 |
| --- | --- | --- |
| `disableRelocation()` | Capture 전에 이동 자체를 거부한다 | 이 Actor가 다른 node로 옮겨지면 안 될 때 |
| `recreateOnRelocation()` | Target factory로 같은 logical identity를 다시 만든다. Application state는 복구하지 않는다 | State 없이 다시 만들어도 되는 Actor일 때 |
| `preserveStateWith(adapterClass)` | `ZLinkActorRelocationAdapter<TActor>.capture`/`restore`로 opaque `byte[]`를 옮긴다 | State를 유지한 채 옮겨야 할 때 |

**완료 결과.** `preserveStateWith`의 `capture(...)` 결과는 최대 64 MiB다. Capture·Restore는 같은
relocation에서 여러 번 호출될 수 있으므로 두 callback 모두 retry-safe해야 한다 — 외부 side
effect의 exactly-once 실행에 의존하면 안 된다. `adapterClass`가 해당 Actor type의
`ZLinkActorRelocationAdapter<TActor>`를 구현하지 않으면 startup configuration error다.

**선택 기준.** 세 정책 중 무엇을 고르느냐가 이 Actor 타입의 relocation 동작 전체를 결정한다 —
factory 등록 시점에 한 번만 정하고 나중에 호출별로 바꿀 수 없다.

---

전체 근거는
[Java Actor exact interface](../../common/spec/server/languages/java/interfaces/actors.ko.md)를
참고한다.
