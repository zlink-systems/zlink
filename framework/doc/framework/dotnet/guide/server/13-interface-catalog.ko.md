---
title: "13. 주요 타입 사용 색인 · C#/.NET"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: 운영 — 메트릭 · drain · readiness](12-operations.ko.md) | [다음: 샘플 고르기](14-samples.ko.md)
<!-- framework-adapter-nav:end -->

# 13. 주요 타입 사용 색인

> **이 장의 계약 소유 문서** —
> [.NET exact interface](../../../common/spec/server/languages/dotnet/interfaces/README.ko.md)가
> 정확한 signature를 소유한다. 이 챕터는 application에서 자주 사용하는 public interface를
> 기능별로 찾는 안내서다.

## 1. Channel messaging

`IZLinkRouteClient`는 ChannelName으로 ready server 하나를 선택한다.

```csharp
await routeClient
    .SendToChannel("game.api", new PlayerOnline("player-1"))
    .Async(ct); // source-local outbound admission까지만 기다린다.

var reply = await routeClient
    .RequestToChannel("game.api", new GetPlayer("player-1"))
    .Timeout(TimeSpan.FromSeconds(3))
    .Async<Player>(ct); // 선택한 handler의 reply를 기다린다.
```

| Interface | Application에서 하는 일 |
|---|---|
| `IZLinkRouteClient` | ChannelName 또는 관리 대상 Node RID로 send/request |
| `IZLinkSendCall` | one-way operation 제출 |
| `IZLinkRequestCall` | timeout 설정과 typed reply 수신 |
| `IZLinkFanoutClient` | classic fanout channel에 event publish |

Node direct는 특정 MeshNode 자체를 관리할 때만 사용한다. 업무 object의 배치나 메시징에는 ActorId,
SpotId 또는 ChannelName을 사용한다.

```csharp
var status = await routeClient
    .RequestToNode(
        "play",
        RoutingId.From("play-node-1"),
        new GetNodeStatus())
    .Async<NodeStatus>(ct); // 운영 시스템이 특정 node 상태를 조회한다.
```

정확한 handler와 call interface는
[Channel messaging exact interface](../../../common/spec/server/languages/dotnet/interfaces/04-channel-messaging.ko.md)를
참고한다.

## 2. Topology 등록

MeshNode의 Object role과 RouteMesh Channel role은 독립적으로 등록한다.

```csharp
services.AddZLinkFramework(options =>
{
    var play = options.AddRouteMesh("play")
        .Listen(5501)
        .SetRoutingIdPrefix("play")
        .SetPlacementWeight(100);

    play.Objects().Server()
        .AddSpotFactory<RoomSpot>(
            "room",
            factory => factory
                .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                .PreserveStateWith<RoomRelocationAdapter>())
        .AddActorFactory<PlayerActor, PlayerActorFactory>(
            "player",
            factory => factory
                .PreserveStateWith<PlayerRelocationAdapter>());

    play.Channel("play.api").Server()
        .SetWeight(100)
        .AddRequestHandler<GetPlayerHandler, GetPlayer, Player>();
});
```

| Interface | Application에서 하는 일 |
|---|---|
| `IZLinkFrameworkOptions` | Store, topology, handler와 공통 option 등록 |
| `IZLinkMeshNodeBuilder` | RouteMesh 소켓, Node RID, placement와 role 등록 |
| `IZLinkMeshObjectRoleBuilder` | Object Client 또는 Server capability 등록 |
| `IZLinkMeshObjectServerBuilder` | Entry Spot과 stable Actor·Spot type 등록 |
| `IZLinkMeshChannelRoleBuilder` | RouteMesh Channel Client 또는 Server membership 등록 |
| `IZLinkClientServerChannelRoleBuilder` | ClientServer Client·Server 역할 등록 |
| `IZLinkFanoutChannelBuilder` | classic fanout publisher·subscriber 등록 |
| `IZLinkStreamNodeBuilder` | STREAM listener와 session 등록 |

Entry SpotId는 Framework가 `<prefix>-entry-<uuid>` 형식으로 발급한다. Application이 Entry Spot의
RoutingId나 SpotId를 설정하는 API는 없다.

정확한 builder는
[Topology exact interface](../../../common/spec/server/languages/dotnet/interfaces/03-configuration-topology.ko.md)를
참고한다.

## 3. Spot

User Spot은 manager로 생성한다. Application은 target Node RID를 지정하지 않는다.

```csharp
ZLinkSpotCreateResult created = await spotManager
    .Create("room")
    .InMesh("play")
    .Request(new CreateRoom("ranked"))
    .Async(ct); // Framework가 global SpotId와 eligible target을 선택한다.

ZLinkSpotCreateResult existingOrCreated = await spotManager
    .GetOrCreate("lobby-eu", "lobby")
    .InMesh("play")
    .Request(new CreateLobby("eu"))
    .Async(ct);
```

일반 Spot 메시징은 global SpotId만 사용한다.

```csharp
await spotClient
    .SendToSpot("room-42", new RoundStarted())
    .Async(ct);

var state = await spotClient
    .RequestToSpot("room-42", new GetRoomState())
    .Async<RoomState>(ct);
```

Instance Spot은 별도 create API가 없다. Missing Spot에 보내는 첫 message에서 activation intent를
명시한다.

```csharp
var match = await spotClient
    .RequestToSpot("matchmaking:gold", new FindMatch("player-1"))
    .InstanceSpot("level-matchmaking")
    .InMesh("matchmaking")
    .Async<MatchFound>(ct);
```

| Interface | Application에서 하는 일 |
|---|---|
| `IZLinkSpotManager` | User Spot create, get-or-create, current ref 조회와 exact close |
| `IZLinkSpotClient` | global SpotId로 Spot send/request |
| `IZLinkSpotOutbound` | Spot callback 안에서 Spot·Channel·Logical Multicast 호출 |
| `IZLinkSpotContext` | handler, timer, worker, close와 relocation-ready turn 관리 |
| `IZLinkInstanceSpotContext` | Instance Spot handler, timer, worker와 close 관리 |
| `IZLinkEntrySpotContext` | Entry Spot handler, timer와 Actor lifecycle 관리 |
| `IZLinkSpotRelocationAdapter<TSpot>` | `PreserveStateWith`에서 opaque state bytes capture·restore |
| `IZLinkSpotPacketHandler<TSpot, TMessage>` | Spot 앞 one-way packet 처리 |
| `IZLinkSpotRequestHandler<TSpot, TRequest, TReply>` | Spot 앞 request 처리와 reply 반환 |
| `IZLinkSpotSubscriptionHandler<TSpot, TEvent>` | Logical Multicast 구독 이벤트 처리 |
| `IZLinkSpotTimerHandler<TSpot>` | Spot timer tick 처리 |

`SpotRef`는 current location snapshot이다. 일반 message target으로 보관하지 않는다. `CloseAsync(spotRef)`처럼
exact generation을 확인해야 하는 operation에 사용한다.

정확한 lifecycle과 call은
[Spot exact interface](../../../common/spec/server/languages/dotnet/interfaces/05-spots.ko.md)를 참고한다.

## 4. Actor

Actor도 global ActorId로 생성하고 호출한다.

```csharp
ZLinkActorCreateResult result = await actorManager
    .GetOrCreate("player-1", "player")
    .InMesh("play")
    .Request(new CreatePlayer("player-1"))
    .Async(ct);

await actorClient
    .SendToActor("player-1", new GrantReward("daily"))
    .Async(ct);
```

Actor handler 안에서 User Spot join을 예약할 때는 현재 turn을 막지 않는 deferred call을 사용한다.

```csharp
actor.Context
    .JoinSpot("room-42", new JoinRoom("player-1"))
    .Timeout(TimeSpan.FromSeconds(3))
    .Defer(); // 현재 handler가 끝난 뒤 Actor queue에서 순서대로 실행한다.
```

| Interface | Application에서 하는 일 |
|---|---|
| `IZLinkActorManager` | Actor create, get-or-create, current ref·Spot 조회와 exact destroy |
| `IZLinkActorClient` | global ActorId로 Actor send/request |
| `IZLinkActorContext` | 현재 Actor identity, Spot membership, session binding과 deferred join |
| `IZLinkActorFactory<TActor>` | Framework가 선택한 target에서 Actor instance 생성 |
| `IZLinkActorRelocationAdapter<TActor>` | `PreserveStateWith`에서 opaque state bytes capture·restore |
| `IZLinkSpotActorSendHandler<TSpot, TActor, TMessage>` | member Actor 앞 one-way packet 처리 |
| `IZLinkSpotActorRequestHandler<TSpot, TActor, TRequest, TReply>` | member Actor 앞 request 처리와 reply 반환 |

`ActorRef`도 exact incarnation을 가리키는 snapshot이다. 일반 messaging은 ActorId를 사용한다.

정확한 interface는
[Actor exact interface](../../../common/spec/server/languages/dotnet/interfaces/06-actors.ko.md)를 참고한다.

## 5. STREAM session

Session은 client 연결을 받고 typed handler를 등록한다. Actor와 bind하면 Actor가 현재 session으로
push할 수 있다.

```csharp
public sealed class GatewaySession(IZLinkSessionContext context) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddHandler<AuthenticateHandler>();
        // 수신 packet을 typed handler에 연결한다.
    }

    public ValueTask OnConnectedAsync(CancellationToken ct)
        => ValueTask.CompletedTask;

    public ValueTask OnDisconnectedAsync(CancellationToken ct)
        => ValueTask.CompletedTask;

    public ValueTask OnErrorAsync(
        ZLinkStreamError error,
        CancellationToken ct)
        => ValueTask.CompletedTask;
}
```

| Interface | Application에서 하는 일 |
|---|---|
| `IZLinkSession` | STREAM connection lifecycle과 handler 등록 |
| `IZLinkSessionContext` | session identity, client, Actor binding과 close |
| `IZLinkSessionClient` | 연결된 client로 send 또는 request reply |
| `IZLinkSessionActors` | ActorRef를 current session에 bind |
| `IZLinkBoundSession` | Actor에서 bind된 session으로 push |

정확한 interface는 [STREAM](../../../common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md)과
[Bound session](../../../common/spec/server/languages/dotnet/interfaces/07-bound-stream-session.ko.md)을
참고한다.

## 6. Location과 relocation

두 Store capability를 별도로 등록한다.

```csharp
options.AddLocationStore(
    new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions
    {
        ConnectionString = "redis:6379",
        KeyPrefix = "game:location"
    }));

options.AddRelocationStore(
    new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions
    {
        ConnectionString = "redis:6379",
        KeyPrefix = "game:relocation"
    }));
```

| Interface | 책임 |
|---|---|
| `IZLinkLocationStore` | Framework가 넘긴 opaque location record의 read·write·atomic batch |
| `IZLinkRelocationStore` | Framework가 넘긴 immutable relocation blob의 put·get·delete |
| `IZLinkLocationReadiness` | 필요한 Mesh peer가 Ready인지 확인 |
| `IZLinkLocationRuntimeQuery` | Location health와 paged topology·service summary 조회 |

Provider SPI는 public이지만 application 개발자가 직접 호출하지 않는다. Provider 구현자는 두 deep
interface만 구현하며 authority record, reservation, aggregate와 recovery state machine은 Framework가
관리한다.

## 7. Host와 topology 관측

Host relocation과 shutdown은 `IZLinkFrameworkRuntime`이 소유한다.

```csharp
var result = await runtime.RelocateAsync(
    new ZLinkFrameworkRelocationOptions
    {
        Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
        Deadline = TimeSpan.FromSeconds(30)
    },
    ct);

if (result.Outcome == ZLinkFrameworkRelocationOutcome.Relocated)
{
    await runtime.ShutdownAsync(TimeSpan.FromSeconds(10), ct);
}
```

| Interface | Application에서 하는 일 |
|---|---|
| `IZLinkFrameworkRuntime` | host status, Relocate, Shutdown과 status stream |
| `IZLinkRouteMeshRuntime` | RouteMesh별 current status와 status stream |
| `IZLinkClientServerRuntime` | ClientServer channel별 current status와 status stream |
| `IZLinkFanoutRuntime` | fanout channel별 current status와 status stream |
| `IZLinkDiagnosticsRuntime` | 실행 중 diagnostics level과 sampling 변경 |

Public monitoring은 application이 판단할 수 있는 상태만 제공한다. Socket generation, authority record,
relocation staging과 mailbox 내부 상태는 log·trace 또는 Framework 내부 진단에 남긴다.

## 8. 관련 문서

- [공개 계약 관리 원칙](../../../common/spec/00-public-contract-governance.ko.md)
- [.NET exact interface 목차](../../../common/spec/server/languages/dotnet/interfaces/README.ko.md)

---
<!-- framework-adapter-nav:bottom:start -->
[가이드 홈](../../../index.ko.md) | [이전: 운영 — 메트릭 · drain · readiness](12-operations.ko.md) | [다음: 샘플 고르기](14-samples.ko.md)
<!-- framework-adapter-nav:bottom:end -->
