# 04. Spot instance

[레퍼런스 목차](README.ko.md)

이 category는 `IZLinkSpotManager`·`IZLinkSpotClient`·`IZLinkSpotPublisherClient`가 제공하는 외부
진입점과, Spot 코드 안에서 `IZLinkSpotContext`로 쓰는 진입점을 다룬다. 정확한 signature는
[Spot exact interface](../../common/spec/server/languages/dotnet/interfaces/05-spots.ko.md)가
소유한다.

---

## `Create`

새 User Spot을 항상 새로 만든다. Framework가 새 global SpotId를 발급한다.

```csharp
ZLinkSpotCreateResult created = await spotManager
    .Create("room")
    .InMesh("play")
    .Request(new CreateRoom("ranked"))
    .Timeout(TimeSpan.FromSeconds(5))
    .Async(ct);

string spotId = created.Spot.SpotId;
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.InMesh(meshName)` | Object Client·Server role의 Mesh가 하나면 생략 가능 | Spot을 생성할 Mesh. 후보가 둘 이상인데 생략하면 `InvalidOperation`, 없으면 `NotConfigured`, 지정한 Mesh가 없으면 `NotFound` |
| `.Request(ZLinkMessage)` / `.Request<TRequest>(TRequest)` | 없음(빈 요청) | Spot의 `OnCreateAsync`에 전달할 생성 요청. 최대 1 MiB |
| `.Timeout(TimeSpan)` | resolve·factory·initialize 전체에 적용되는 기본값 | 생성 전체가 terminal state가 될 때까지의 상한 |
| `.Async(ct)` | terminal(택 1, single-use) | 생성 완료까지 기다린다 |
| `.Yield(ct)` | terminal(택 1, single-use) | `SpotWide` handler 안에서만 유효 |

**완료 결과.** `ZLinkSpotCreateResult.State`가 `Created`(새로 생성)다. Spot의 `OnCreateAsync`가
거부하면 `Rejected`이고 `Reply`에 거부 메시지가 담긴다. 같은 option을 두 번 설정하거나 terminal을
두 번 호출하면 `InvalidOperation`, deadline 안에 끝나지 않으면 `DeadlineExceeded`다.

**선택 기준.** 항상 새 인스턴스가 필요할 때 쓴다. 있으면 재사용하고 없을 때만 만들려면
`GetOrCreate`를 쓴다.

---

## `GetOrCreate`

지정한 SpotId의 Ready Spot이 있으면 그것을 반환하고, 없으면 새로 만든다.

```csharp
ZLinkSpotCreateResult existingOrCreated = await spotManager
    .GetOrCreate("lobby-eu", "lobby")
    .InMesh("play")
    .Request(new CreateLobby("eu"))
    .Async(ct);
```

**옵션.** `Create`와 동일하다 — `.InMesh(...)`, `.Request(...)`, `.Timeout(...)`, terminal
`.Async(ct)` 또는 `.Yield(ct)`.

**완료 결과.** `State`가 `Existing`이면 이미 있던 Spot을 그대로 반환하고 `Request`는 무시한다.
`Created`면 새로 만든 것이다. 같은 SpotId가 `Creating` 상태로 경합 중이면 그 결과를 기다렸다가
합류하고, cleanup으로 Missing이 되면 새 reservation을 다시 경쟁한다. Kind나 stable type이 기존
authority와 다르면 `TypeMismatch`로 완료한다.

**선택 기준.** SpotId로 멱등하게 "있으면 쓰고 없으면 만들기"가 필요할 때 쓴다. 항상 새 인스턴스가
필요하면 `Create`를 쓴다.

---

## `FindAsync` / `CloseAsync` (manager)

기존 Spot을 조회하거나 정확한 incarnation을 닫는다.

```csharp
SpotRef? spot = await spotManager.FindAsync("lobby-eu", ct);

if (spot is { } found)
{
    bool closed = await spotManager.CloseAsync(found, ct);
}
```

**옵션.** 두 호출 모두 modifier가 없다 — 대상 식별자와 `CancellationToken`만 받는다.

**완료 결과.** `FindAsync`는 Ready Spot이 없으면 `null`을 반환한다. `CloseAsync`는 해당
incarnation이 없으면 `false`, generation이 다르면 `InvalidOperation`, pre-commit seal 중이면
`Unavailable`이다. User Spot에 Actor membership이 남아 있으면 `false`이며 Actor를 자동으로
leave·destroy하지 않는다.

**선택 기준.** 지금 시점의 존재 여부 확인이나 명시적 종료가 필요할 때 쓴다. `CloseAsync`는 stale
`SpotRef`로 다른 incarnation을 대신 닫지 않는다.

---

## `SendToSpot<TMessage>`

Global SpotId 하나로 one-way message를 보낸다. 외부 client(Node·Channel handler, 다른 Actor·Spot,
application code)에서 쓴다.

```csharp
await spotClient
    .SendToSpot("room-42", new PlayerJoinedRoom("player-1"))
    .Async(ct);

// Instance Spot을 필요하면 새로 활성화(cold activation)해서 보내는 경우
await spotClient
    .SendToSpot("device-42", new DeviceCommand("reboot"))
    .InstanceSpot("device")
    .Async(ct);
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.Metadata(...)` | 없음 | handler에 전달할 key-value |
| `.InstanceSpot()` | 없음(User Spot만 resolve) | Missing이면 cold activation한다. 등록된 Instance Spot 타입이 하나일 때만 타입 생략 가능 |
| `.InstanceSpot(instanceSpotType)` | — | 등록 타입이 여럿이면 타입을 명시해야 한다 |
| `.InMesh(meshName)` | Object Client·Server role의 Mesh가 하나면 생략 가능 | Missing Instance Spot을 처음 만들 Mesh. Instance marker 없이 쓰면 `InvalidOperation` |
| `.Async(ct)` | 필수 terminal | source-local admission까지만 기다린다 |

**완료 결과.** SpotId가 없고 Instance marker도 없으면 `NotFound`. `InstanceSpot(...)`을 썼는데
existing authority가 User Spot이거나 명시한 타입과 다르면 `TypeMismatch`. 그 외 완료 kind는
messaging-execution category의 공통 규칙과 같다.

**선택 기준.** Reply가 필요 없는 Spot 메시징에 쓴다. Reply가 필요하면 `RequestToSpot`을 쓴다.

---

## `RequestToSpot<TRequest, TResponse>`

Global SpotId 하나로 typed request/reply를 주고받는다.

```csharp
var reply = await spotClient
    .RequestToSpot("room-42", new GetRoomState())
    .Timeout(TimeSpan.FromSeconds(3))
    .Async<RoomState>(ct);
```

**옵션.** `SendToSpot`과 동일한 `.InstanceSpot(...)`/`.InMesh(...)`에 더해 다음이 있다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.Timeout(TimeSpan)` | `DefaultRequestTimeout` | resolve, cold activation, handler, reply 전체의 deadline |
| `.Async<TResponse>(ct)` | terminal(택 1) | reply 수신까지 기다린다 |
| `.Yield<TResponse>(ct)` | terminal(택 1) | `SpotWide` User Spot·Instance Spot handler 안에서만 유효. 그 밖에서 호출하면 `InvalidOperation` |

**완료 결과.** `SendToSpot`과 같은 실패 kind에 더해, cold activation 중 factory나 initialize가
실패하면 typed failure로 완료된다 — Framework가 내부적으로 재시도하지 않는다.

**선택 기준.** Reply 값이 필요할 때 쓴다. One-way면 `SendToSpot`을 쓴다.

---

## `Publish<TEvent>` (Spot Logical Multicast)

ChannelName과 topic으로 구독자에게 typed event를 발행한다. `IZLinkSpotPublisherClient`(외부)와
`IZLinkSpotOutbound`(Spot 코드 안, `Context.Outbound`)가 같은 모양을 제공한다.

```csharp
await spotPublisherClient
    .Publish("room.events", "room-42", new RoomStateChanged("started"))
    .Async(ct);
```

**옵션.** 이 호출에는 `.Async(ct)` terminal만 있다 — topic은 필수 인자다.

**완료 결과.** 정상 완료는 발행 admission이 끝났다는 뜻이다. Subscriber 수신은 기다리지 않는다.
messaging-execution category의 classic fanout `Publish`와 달리, ChannelName만으로 owner
MeshNode를 결정하며 caller가 MeshName을 추가로 넘기지 않는다.

**선택 기준.** Spot 상태 변화를 관찰자에게 알릴 때 쓴다. 구독자에게 직접 reply가 필요하면 이 항목이
아니라 `RequestToSpot`을 쓴다.

---

## `AddTimer<THandler>` (Spot 코드 안)

Spot에 속한 주기 timer를 등록한다. `Context.AddTimer(...)`로 호출한다.

```csharp
IZLinkTimer timer = await Context.AddTimer<RoomTickHandler>(
    "room-tick",
    TimeSpan.FromSeconds(1),
    new ZLinkTimerOptions { OverrunPolicy = ZLinkTimerOverrunPolicy.SkipLateTicks },
    ct);
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `options.OverrunPolicy` | `SkipLateTicks` | tick이 밀렸을 때 건너뛸지, 상한 안에서 따라잡을지, 다음 tick을 늦출지 |
| `options.MaxCatchUpTicks` | 1 | `CatchUpBounded`일 때 한 번에 따라잡을 최대 tick 수 |
| `options.StopOnUnhandledException` | `false` | handler 예외 시 timer를 멈출지 여부 |

**완료 결과.** `IZLinkTimer`를 반환한다. Timer는 이 Spot에 속한 logical registration이라
relocation 때 자동으로 이전되며 application이 target에서 다시 등록할 필요가 없다. `CancelAsync()`나
`DisposeAsync()`로 취소한다.

**선택 기준.** Spot 안에서 주기 작업이 필요할 때 쓴다.

---

## `RunCpuWorker<TResult>` / `RunIoWorker<TResult>` (Spot 코드 안)

Spot의 owner turn을 막지 않고 별도 worker에서 작업을 실행한다.

```csharp
int result = await Context
    .RunCpuWorker(ct => ComputeExpensiveScore(ct))
    .Timeout(TimeSpan.FromSeconds(2))
    .Async(ct);
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.Timeout(TimeSpan)` | Worker option의 기본값 | 작업 완료 상한 |
| `.Submit(ct)` | — | 결과를 기다리지 않고 제출만 한다 |
| `.Async(ct)` | terminal(택 1) | 완료까지 기다린다 |
| `.Yield(ct)` | terminal(택 1) | `SpotWide` handler 안에서만 유효 |

**완료 결과.** `TResult`를 반환하거나 timeout이면 `DeadlineExceeded`로 완료한다. Worker pool
크기(`MinThreads`/`MaxThreads`)와 idle timeout은 host 시작 전에만 설정한다.

**선택 기준.** CPU-bound 계산은 `RunCpuWorker`, I/O 대기가 있는 작업은 `RunIoWorker`를 쓴다. 둘 다
owner turn의 순차 실행을 막지 않으려는 목적이다.

---

## Handler 등록 (Spot 코드 안, `Configure()`)

Spot이 받을 packet·request·구독·member Actor 메시지를 처리할 handler type을 등록한다.
`Context.Handlers`(User·Entry Spot은 `IZLinkSpotHandlerRegistry`, Instance Spot은
`IZLinkInstanceSpotHandlerRegistry`)로 호출하며, `Configure()` override 안에서만 호출한다.

```csharp
public void Configure()
{
    Context.Handlers.AddPacket<ChatHandler>();               // Spot 앞 packet·request
    Context.Handlers.AddActorPacket<JoinGameHandler, PlayerActor>(); // member Actor 앞
    Context.Handlers.AddSubscribe<ScoreHandler>("game.scores", "world"); // 구독 이벤트
}
```

**옵션.** Handler가 구현하는 interface에 따라 등록 메서드가 갈린다.

| 대상 | Handler interface | 등록 메서드 |
| --- | --- | --- |
| User Spot 앞 one-way packet | `IZLinkSpotPacketHandler<TSpot, TMessage>` | `AddPacket<THandler>()` |
| User Spot 앞 request | `IZLinkSpotRequestHandler<TSpot, TRequest, TReply>` | `AddPacket<THandler>()` |
| Logical Multicast 구독 이벤트 | `IZLinkSpotSubscriptionHandler<TSpot, TEvent>` | `AddSubscribe<THandler>(channelName, topic)` |
| User Spot의 member Actor 앞 one-way packet | `IZLinkSpotActorSendHandler<TSpot, TActor, TMessage>` | `AddActorPacket<THandler, TActor>()` |
| User Spot의 member Actor 앞 request | `IZLinkSpotActorRequestHandler<TSpot, TActor, TRequest, TReply>` | `AddActorPacket<THandler, TActor>()` |
| Entry Spot 자신의 packet(Actor binding 전) | `IZLinkSpotPacketHandler<TEntrySpot, TMessage>`/`IZLinkSpotRequestHandler<TEntrySpot, TRequest, TReply>` | `AddHandler<THandler>()`(base `IZLinkActorHandlerRegistry`에서 상속) |
| Entry Spot의 member Actor 앞 one-way packet | `IZLinkEntrySpotActorSendHandler<TEntrySpot, TActor, TMessage>` | `AddActorPacket<THandler, TActor>()` |
| Entry Spot의 member Actor 앞 request | `IZLinkEntrySpotActorRequestHandler<TEntrySpot, TActor, TRequest, TReply>` | `AddActorPacket<THandler, TActor>()` |
| Instance Spot 앞 packet | `IZLinkSpotPacketHandler<TSpot, TMessage>`과 동일한 모양 | `AddPacket<THandler>()`(`IZLinkInstanceSpotHandlerRegistry`) |

Entry Spot의 `AddActorPacket<THandler, TActor>()`는 User Spot의 것과 메서드 이름은 같지만 `THandler`가
구현해야 하는 interface가 다르다 — 등록하는 registry가 어느 Spot 종류의 것인지에 따라 갈린다.

**완료 결과.** 반환값 없이 동기로 등록된다. Packet name을 생략하면 handler가 처리하는 메시지
타입의 `ZLinkPacketAttribute`를 확인하고, 그것도 없으면 타입 이름을 쓴다. 같은 owner의 handler key
중복은 host startup 검증에서 `ZLinkConfigurationException`으로 드러난다.

**선택 기준.** `Configure()`가 호출될 때마다 이 Spot이 처리할 모든 handler를 등록한다. Node·Channel
handler는 topology-discovery category의 등록 항목을, STREAM session handler는 stream-session
category를 참고한다.

---

## `SendToChannel<TMessage>` / `RequestToChannel<TRequest, TResponse>` (Spot 코드 안, `Context.Outbound`)

Spot 코드 안에서 ChannelName으로 one-way message를 보내거나 typed request/reply를 주고받는다.
`IZLinkSpotOutbound`가 제공하며 messaging-execution category의 `SendToChannel`/`RequestToChannel`과
같은 모양이다.

```csharp
await Context.Outbound
    .RequestToChannel<GetLeaderboard>("leaderboard.api", new GetLeaderboard())
    .Async<Leaderboard>(ct);
```

**옵션.** messaging-execution category의 `SendToChannel`/`RequestToChannel`과 동일한 modifier를
받는다 — `.Metadata(...)`, `.Timeout(...)`, terminal `.Async(ct)`/`.Async<TResponse>(ct)`.

**완료 결과.** messaging-execution category의 완료 kind와 같다.

**선택 기준.** Spot이 외부 client가 아니라 자기 코드 안에서 다른 ChannelName의 handler를 호출해야
할 때 쓴다. 다른 Spot을 직접 호출하려면 `SendToSpot`/`RequestToSpot`을 쓴다.

---

## `LeaveActorAsync` / `CloseAsync` / `DestroyActorAsync` (Spot 코드 안, 종료·이탈)

Member Actor를 이 Spot에서 내보내거나, Spot 자신을 닫거나, Entry Spot에서 Actor를 파기한다.

```csharp
await Context.LeaveActorAsync(actor, ct);       // User Spot: member Actor만 내보낸다
bool closed = await Context.CloseAsync(ct);     // User·Instance Spot: 이 Spot 자신을 닫는다
await entryContext.DestroyActorAsync(actor, ct); // Entry Spot: Actor를 완전히 파기한다
```

**옵션.** 세 호출 모두 modifier가 없다 — 대상(`LeaveActorAsync`/`DestroyActorAsync`)과
`CancellationToken`만 받는다.

**완료 결과.** `LeaveActorAsync`(`IZLinkSpotContext` 전용)는 member Actor membership만 해제하고
Actor 자체는 파기하지 않는다. `CloseAsync`(`IZLinkSpotContext`/`IZLinkInstanceSpotContext`)는
manager의 `CloseAsync(spotRef)`(spot-instance category 앞부분 항목)와 같은 완료 kind를 쓰되, 이
Spot 자신을 대상으로 한다. `DestroyActorAsync`(`IZLinkEntrySpotContext` 전용)는 Actor를 완전히
파기한다 — `LeaveActorAsync`와 달리 membership 해제가 아니라 Actor 자체를 없앤다.

**선택 기준.** Member Actor를 다른 곳으로 옮기지 않고 이 Spot에서만 빼려면 `LeaveActorAsync`를,
Spot 자신을 스스로 종료하려면 `CloseAsync`를, Entry Spot에서 더 이상 필요 없는 Actor를 완전히
없애려면 `DestroyActorAsync`를 쓴다.

---

## `RelocationReady().Defer()` (Spot 코드 안)

`ApplicationSignaled` readiness mode를 선택한 `SpotWide` Spot에서, relocation 경계를 다음 application
turn 앞으로 미룬다.

```csharp
Context.RelocationReady().Defer();
```

**옵션.** 이 호출에는 modifier가 없다.

**완료 결과.** 반환값 없음. 현재 handler가 끝난 뒤 relocation 경계를 등록한다. 이동하지 않았거나
commit 전에 abort했으면 source에서 `Continued`, 이동했으면 target에서 `Relocated` completion을
`OnRelocationReadyCompletedAsync(...)`로 받는다. `AnyTurnBoundary` mode, `PerActor` Spot, Entry·
Instance Spot, Spot turn 밖, 같은 turn의 중복 호출은 `InvalidOperation`으로 완료한다.

**선택 기준.** Application이 relocation 시점을 특정 turn 경계로 정밀하게 제어해야 할 때 쓴다. 기본
`AnyTurnBoundary` mode에서는 이 호출이 필요하지 않다.

---

전체 근거는
[Spot exact interface](../../common/spec/server/languages/dotnet/interfaces/05-spots.ko.md)를
참고한다.
