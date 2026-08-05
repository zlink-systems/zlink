# 05. Actor relocation

[레퍼런스 목차](README.ko.md)

이 category는 `IZLinkActorManager`·`IZLinkActorClient`가 제공하는 외부 진입점과, Actor 코드 안에서
`IZLinkActorContext`로 Spot에 참여하는 진입점, 그리고 relocation 정책 선택을 다룬다. 정확한
signature는
[Actor exact interface](../../common/spec/server/languages/dotnet/interfaces/06-actors.ko.md)가
소유한다.

---

## `Create` (Actor)

새 Actor를 항상 새로 만든다.

```csharp
ZLinkActorCreateResult created = await actorManager
    .Create("player-1", "player")
    .InMesh("play")
    .Request(new SpawnPlayer("player-1"))
    .Async(ct);
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.InMesh(meshName)` | Object Client·Server role의 Mesh가 하나면 생략 가능 | Actor를 생성할 Mesh. 후보가 둘 이상인데 생략하면 `InvalidOperation`, 없으면 `NotConfigured`, 지정한 Mesh가 없으면 `NotFound` |
| `.Request(ZLinkMessage)` / `.Request<TRequest>(TRequest)` | 없음(빈 요청) | Actor factory 생성 시점에 전달할 요청. 최대 1 MiB |
| `.Timeout(TimeSpan)` | 5초 | resolve·reservation·factory·`Ready` barrier 전체의 deadline |
| `.Async(ct)` | terminal(택 1, single-use) | 생성 완료까지 기다린다 |
| `.Yield(ct)` | terminal(택 1, single-use) | `SpotWide` handler 안에서만 유효 |

**완료 결과.** `ZLinkActorCreateResult`는 `Created`(새로 생성) 또는 `Rejected`(factory가 거부) 중
하나로 완료한다. 같은 ActorId의 Ready incarnation이 이미 있으면 `Created`/`Rejected`가 아니라
`AlreadyExists` 오류로 완료한다 — `Existing` 결과는 `GetOrCreate`에만 있다. Ready incarnation이
있는데 stable type이 다르면 `TypeMismatch`다.

**선택 기준.** 항상 새 Actor가 필요할 때 쓴다. 있으면 재사용하고 없을 때만 만들려면
`GetOrCreate`를 쓴다.

---

## `GetOrCreate` (Actor)

같은 ActorId의 Ready Actor가 있으면 그것을 반환하고, 없으면 새로 만든다.

```csharp
ZLinkActorCreateResult existingOrCreated = await actorManager
    .GetOrCreate("player-1", "player")
    .InMesh("play")
    .Request(new SpawnPlayer("player-1"))
    .Async(ct);
```

**옵션.** `Create`와 동일하다 — `.InMesh(...)`, `.Request(...)`, `.Timeout(...)`, terminal
`.Async(ct)` 또는 `.Yield(ct)`.

**완료 결과.** `Existing`이면 이미 있던 Actor를 반환하고 `Request`는 무시한다. Creating attempt와
경합하면 그 결과를 기다렸다가 합류하며, 서로 다른 operation은 Ready 뒤 `Existing`을 받고 이전
reply를 공유하지 않는다.

**선택 기준.** ActorId로 멱등하게 "있으면 쓰고 없으면 만들기"가 필요할 때 쓴다.

---

## `FindAsync` / `FindSpotAsync` / `DestroyAsync` (manager)

기존 Actor를 조회하거나, 참여 중인 Spot을 조회하거나, 정확한 incarnation을 종료한다.

```csharp
ActorRef? actor = await actorManager.FindAsync("player-1", ct);
SpotRef? spot = await actorManager.FindSpotAsync("player-1", ct);

if (actor is { } found)
{
    bool destroyed = await actorManager.DestroyAsync(found, ct);
}
```

**옵션.** 세 호출 모두 modifier가 없다 — 대상 식별자와 `CancellationToken`만 받는다.

**완료 결과.** `FindAsync`는 Ready Actor가 없으면 `null`을 반환한다. `FindSpotAsync`는 current
User Spot membership이 없으면 `null`을 반환한다. `DestroyAsync`는 해당 incarnation이 없으면
`false`, generation이 다르면 `InvalidOperation`, pre-commit seal 중이면 `Unavailable`이다.

**선택 기준.** 지금 시점의 존재·소속 확인이나 명시적 종료가 필요할 때 쓴다.

---

## `SendToActor<TMessage>` / `RequestToActor<TRequest, TResponse>`

Global ActorId 하나로 one-way message를 보내거나 typed request/reply를 주고받는다. 외부
client에서 쓴다.

```csharp
await actorClient
    .SendToActor("player-1", new GrantItem("sword"))
    .Async(ct);

var reply = await actorClient
    .RequestToActor("player-1", new GetInventory())
    .Timeout(TimeSpan.FromSeconds(3))
    .Async<Inventory>(ct);
```

**옵션.** `SendToActor`는 `.Metadata(...)`와 terminal `.Async(ct)`만 있다. `RequestToActor`는
다음이 더 있다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.Timeout(TimeSpan)` | `DefaultRequestTimeout` | reply를 기다리는 상한 |
| `.Async<TResponse>(ct)` | terminal(택 1) | reply 수신까지 기다린다 |
| `.Yield<TResponse>(ct)` | terminal(택 1) | `SpotWide` handler 안에서만 유효 |

**완료 결과.** ActorId가 없으면 `NotFound`. 나머지 완료 kind는 messaging-execution category의
공통 규칙과 같다.

**선택 기준.** Reply가 필요 없으면 `SendToActor`, 필요하면 `RequestToActor`를 쓴다.

---

## `JoinSpot` / `JoinEntrySpot` (Actor 코드 안)

현재 Actor를 User Spot 또는 Entry Spot에 참여시킨다. `Context.JoinSpot(...)`/`Context.JoinEntrySpot(...)`로
호출하며, 다른 항목과 달리 terminal이 `Async`/`Yield`가 아니라 `Defer()` 하나뿐이다.

```csharp
Context
    .JoinSpot("room-42", new JoinRoomRequest("player-1"))
    .Timeout(TimeSpan.FromSeconds(5))
    .Defer();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.Timeout(TimeSpan)` | 5초 | monotonic absolute deadline |
| `.Defer()` | 필수 terminal | 결과 없는 동기 호출. Join intent와 비활성 barrier만 등록하고 target 조회를 바로 시작하지 않는다 |

**완료 결과.** `Defer()` 자체는 반환값이 없다. 현재 handler가 정상적으로 끝나면 barrier가
활성화되어 Join을 실행하고, handler가 실패하면 barrier를 폐기한다. 실제 결과(수락·거부·실패)는
같은 `ZLinkActorJoinOperationId`를 담은 `OnJoinCompletedAsync(ZLinkActorJoinCompletion, ct)`
callback으로 비동기 전달된다 — `Accepted`/`Rejected`/`Failed` 중 하나다.

**선택 기준.** Actor를 다른 Spot으로 옮기거나 Entry Spot으로 되돌릴 때 쓴다. Entry Spot과
`PerActor` User Spot의 Actor에서 호출하면 `InvalidOperation`으로 완료한다.

---

## Relocation 정책 선택 (Actor factory 등록 시점)

`AddActorFactory<TActor, TFactory>(...)`(topology-discovery category)의 `configure` callback에서
정확히 하나를 선택한다.

| 정책 | cross-node 이동 시 동작 | 선택 기준 |
| --- | --- | --- |
| `DisableRelocation()` | Capture 전에 이동 자체를 거부한다 | 이 Actor가 다른 node로 옮겨지면 안 될 때 |
| `RecreateOnRelocation()` | Target factory로 같은 logical identity를 다시 만든다. Application state는 복구하지 않는다 | State 없이 다시 만들어도 되는 Actor일 때 |
| `PreserveStateWith<TAdapter>()` | `IZLinkActorRelocationAdapter<TActor>.CaptureAsync`/`RestoreAsync`로 opaque byte 배열을 옮긴다 | State를 유지한 채 옮겨야 할 때 |

**완료 결과.** `PreserveStateWith`의 `CaptureAsync` 결과는 최대 64 MiB다. Capture·Restore는
같은 relocation에서 여러 번 호출될 수 있으므로 두 callback 모두 retry-safe해야 한다 — 외부 side
effect의 exactly-once 실행에 의존하면 안 된다.

**선택 기준.** 세 정책 중 무엇을 고르느냐가 이 Actor 타입의 relocation 동작 전체를 결정한다 —
factory 등록 시점에 한 번만 정하고 나중에 호출별로 바꿀 수 없다.

---

전체 근거는
[Actor exact interface](../../common/spec/server/languages/dotnet/interfaces/06-actors.ko.md)를
참고한다.
