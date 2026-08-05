# 02. Topology discovery

[레퍼런스 목차](README.ko.md)

이 category는 `IZLinkFrameworkOptions`가 제공하는 topology 등록 진입점과, RouteMesh·ClientServer·Fanout
운영 상태를 조회하는 진입점을 다룬다. 정확한 signature는
[RouteMesh·MeshNode exact interface](../../common/spec/server/languages/dotnet/interfaces/03-configuration-topology.ko.md)와
[Topology monitoring exact interface](../../common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md)가
소유한다. 등록 진입점은 모두 host 구성 시점의 호출이다.

---

## `AddRouteMesh` (구성 시점)

물리 MeshNode 하나를 등록한다. RouteMesh 기반 topology의 시작점이다.

```csharp
services.AddZLinkFramework(options =>
{
    var play = options.AddRouteMesh("play")
        .Listen(5501)
        .SetRoutingIdPrefix("play")
        .SetPlacementWeight(100);
});
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.Listen(endpoint)` / `.Listen(port)` | 없으면 bind하지 않음 | 이 MeshNode의 수신 endpoint. Server membership이 0개면 listen하지 않아도 된다 |
| `.SetBindHost(string)` / `.SetAdvertiseHost(string)` | `ConfigureNetwork()`의 root 기본값을 따름 | 이 MeshNode에만 적용하는 bind·advertise host. Root 기본값보다 우선한다 |
| `.SetRoutingId(RoutingId)` / `.SetRoutingIdPrefix(string)` | Framework가 발급 | 고정 RID 또는 발급 RID의 prefix |
| `.SetPlacementWeight(int)` | 100(범위 `0..10000`) | 새 Actor·Spot을 이 node에 배치할 상대 가중치 |
| `.SetActorLimit(int)` / `.SetSpotLimit(int)` | `0`(무제한) | 이 node가 수용하는 Actor·Spot 상한 |
| `.SetActivationConcurrency(int)` | 128 | object population이 아니라 동시에 진행되는 activation admission 상한 |
| `.SetDefaultRequestTimeout(TimeSpan)` | 이 MeshNode의 request 기본 timeout | `RequestToNode`/`RequestToChannel`(messaging-execution category) 등이 `.Timeout(...)`을 생략했을 때 쓰는 값 |
| `.SetInstanceSpotIdleTimeout(TimeSpan)` | `TimeSpan.Zero`(정리하지 않음) | Instance Spot idle 회수 시간. 유효 범위는 `TimeSpan.Zero` 이상이며 음수는 startup 오류다 |
| `.Objects()` | — | Object role(Client/Server) 등록으로 진입. Object role 등록 항목을 참고 |
| `.Channel(channelName)` | — | 이 MeshNode의 RouteMesh Channel role 등록으로 진입. RouteMesh Channel 등록 항목을 참고 |
| `.AddRouteSendHandler<THandler, TMessage>(packetName?)` | packet name은 메시지 타입에서 결정 | Node direct one-way handler(`IZLinkRouteSendHandler<TMessage>`) 등록. `SendToNode`(messaging-execution category)가 호출하는 대상 |
| `.AddRouteRequestHandler<THandler, TRequest, TReply>(packetName?)` | packet name은 메시지 타입에서 결정 | Node direct request handler(`IZLinkRouteRequestHandler<TRequest, TReply>`) 등록. `RequestToNode`가 호출하는 대상 |

**완료 결과.** 반환값 없이 동기로 등록된다. 잘못된 조합(중복 MeshName, listener 설정 누락 등)은
host startup 검증에서 `ZLinkConfigurationException`으로 드러난다. 같은 packet name을 RouteMesh
Channel handler family와 Node direct handler family에 각각 등록할 수 있으며, 각 family 안의 중복
key만 startup 오류다.

**선택 기준.** RouteMesh를 쓰는 모든 host가 최소 하나의 MeshNode를 등록할 때 쓴다. Manual peer만
쓰고 분산 discovery가 필요 없는 node는 Location Store 없이 시작할 수 있다.

---

## Object role 등록 (구성 시점)

MeshNode가 Actor·Spot을 어떻게 다루는지(Client만 하는지, Server로 호스팅하는지) 등록한다.

```csharp
play.Objects().Server()
    .AddEntrySpot<GameEntrySpot>()
    .AddSpotFactory<RoomSpot>(
        "room",
        factory => factory
            .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
            .PreserveStateWith<RoomRelocationAdapter>())
    .AddActorFactory<PlayerActor, PlayerActorFactory>(
        "player",
        factory => factory
            .PreserveStateWith<PlayerRelocationAdapter>());
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.AddEntrySpot<TEntrySpot>()` | 없음 | 외부 진입 전용 Entry Spot 타입 등록 |
| `.AddSpotFactory<TSpot>(type, configure)` | 없음 | stable User Spot 타입 등록. `configure`는 `StableTypeLimit(int)`·`ExecutionMode`·`RelocationReadiness`에 더해 `PreserveStateWith`/`RecreateOnRelocation`/`DisableRelocation` 중 정확히 하나를 받는다 |
| `.AddInstanceSpotFactory<TSpot>(type, configure)` | 없음 | cold-activation Instance Spot 타입 등록. `configure`는 `StableTypeLimit(int)`에 더해 `PreserveStateWith`/`RecreateOnRelocation`/`DisableRelocation` 중 정확히 하나를 받는다 |
| `.AddActorFactory<TActor, TFactory>(type, configure)` | 없음 | stable Actor 타입 등록. `configure`는 `PreserveStateWith`/`RecreateOnRelocation`/`DisableRelocation` 중 정확히 하나를 받는다(Actor factory에는 `StableTypeLimit`이 없다) |

**완료 결과.** 반환값 없이 동기로 등록된다. Relocation을 쓰려는 stable type의 adapter·factory 불일치,
type 중복은 host startup 검증에서 `ZLinkConfigurationException`으로 드러난다.

**선택 기준.** 이 node가 Actor·Spot을 실제로 호스팅(Server)하거나, 다른 node가 호스팅하는 Actor·Spot을
메시징 대상으로만 참조(Client)할 때 각각의 role을 등록한다. Relocation 정책 선택 기준은
actor-relocation category를 참고한다.

---

## RouteMesh Channel 등록 (구성 시점)

같은 MeshNode 안에서 논리 ChannelName membership을 등록한다.

```csharp
play.Channel("play.api").Server()
    .SetWeight(100)
    .AddRequestHandler<GetPlayerHandler, GetPlayer, Player>()
    .AddSendHandler<PlayerOnlineHandler, PlayerOnline>();
```

**옵션.** `Channel(channelName)` 뒤에는 `.Client()` 또는 `.Server()`를 정확히 한 번 호출한다.
`.Client()`는 송신 경로만 만들고 modifier가 없다. `.Server()`에 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.SetWeight(int)` | 100(범위 `0..10000`) | 이 Server가 request/send 대상으로 선택될 상대 가중치. `0`이면 선택 대상에서 제외 |
| `.AddHandlerGroup(groupName)` | 없음 | assembly scan이 찾는 handler group 지정 |
| `.AddSendHandler<THandler, TMessage>(packetName?)` | packet name은 메시지 타입에서 결정 | one-way handler 등록 |
| `.AddRequestHandler<THandler, TRequest, TReply>(packetName?)` | packet name은 메시지 타입에서 결정 | request/reply handler 등록 |

**완료 결과.** 반환값 없이 동기로 등록된다. 같은 owner namespace 안 handler key 중복은 host startup
검증에서 `ZLinkConfigurationException`으로 드러난다.

**선택 기준.** `SendToChannel`/`RequestToChannel`(messaging-execution category)로 받을 handler를
등록할 때 `.Server()`를 쓴다. 이 MeshNode가 다른 node의 Server만 호출하고 자신은 handler를 두지
않으면 `.Client()`만 등록한다. 서로 다른 프로세스 사이 통신이 필요하면 ClientServer Channel 등록을
대신 쓴다.

---

## `AddClientServerChannel` (구성 시점)

RouteMesh와 무관하게 독립된 ClientServer Channel을 등록한다.

```csharp
options.AddClientServerChannel("payments.api").Server()
    .Listen(6001)
    .SetWeight(100)
    .AddRequestHandler<ChargeHandler, Charge, ChargeResult>();

options.AddClientServerChannel("payments.api").Client()
    .Connect("payments-1:6001");
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.Server().Listen(port)` | port 생략 시 `0`(automatic discovery는 port 생략·`0` 모두 자동 bind) | 이 Server의 수신 port. `Listen(string endpoint)` overload는 없다 — endpoint 문자열이 아니라 int port만 받는다 |
| `.Server().SetBindHost(string)` / `.SetAdvertiseHost(string)` | root `ConfigureNetwork()` 기본값을 따름 | 이 Server에만 적용하는 bind·advertise host |
| `.Server().SetWeight(int)` / `.AddSendHandler`/`.AddRequestHandler` | RouteMesh Channel Server와 동일 | 가중치와 handler 등록 |
| `.Client().Connect(endpoint)` | manual | 특정 Server에 수동 연결. 생략하면 automatic discovery로 target을 찾는다 |

**완료 결과.** 반환값 없이 동기로 등록된다. Automatic discovery를 쓰는 Client·Server는 Location
Store 등록이 없으면 host startup 검증에서 `ZLinkConfigurationException`으로 드러난다.

**선택 기준.** RouteMesh 멤버가 아닌 독립 서비스 사이의 request/reply나 one-way 메시징에 쓴다. 같은
RouteMesh 안 node끼리는 RouteMesh Channel 등록을 대신 쓴다.

---

## `AddFanoutChannel` (구성 시점)

Classic fanout 전용 채널을 등록한다. `Publish`(messaging-execution category)로 발행할 대상이다.

```csharp
options.AddFanoutChannel("lobby.events")
    .EnablePublisher(7001)
    .AddHandler<PlayerJoinedHandler, PlayerJoined>();

// automatic subscriber — Location Store로 같은 ChannelName의 publisher를 전부 찾는다
options.AddFanoutChannel("lobby.events")
    .EnableSubscriber();

// manual subscriber — 명시한 endpoint만 쓴다. EnableSubscriber()와 함께 쓰면 startup이 실패한다
options.AddFanoutChannel("lobby.events")
    .Connect("lobby-1:7001");
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.EnablePublisher(endpoint)` / `.EnablePublisher(port)` | 없음 | 이 채널의 발행자 역할과 수신 endpoint 등록 |
| `.SetBindHost(string)` / `.SetAdvertiseHost(string)` / `.SetRoutingId(RoutingId)` / `.SetRoutingIdPrefix(string)` | root 기본값 또는 Framework 발급 | publisher에만 적용하는 bind·advertise host와 RID |
| `.EnableSubscriber()` | — | automatic subscriber. Location Store에서 같은 ChannelName의 유효한 publisher를 전부 찾는다 |
| `.Connect(endpoint)` | — | manual subscriber. 명시한 endpoint만 쓴다. 같은 채널에 `EnableSubscriber()`와 함께 등록하면 host startup이 실패한다 |
| `.AddHandler<THandler, TEvent>(packetName?)` | packet name은 이벤트 타입에서 결정 | typed event handler 등록 |

**완료 결과.** 반환값 없이 동기로 등록된다. Automatic subscriber와 manual subscriber를 같은
fanout channel에 함께 설정하면 `ZLinkConfigurationException`으로 드러난다.

**선택 기준.** 발행자가 구독자를 알 필요가 없는 관찰·통지 채널을 새로 만들 때 쓴다. Reply가 필요한
메시징에는 RouteMesh Channel이나 ClientServer Channel 등록을 대신 쓴다.

---

## `AddStreamNode` (구성 시점)

외부 STREAM 연결을 받는 listener를 등록한다.

```csharp
options.AddStreamNode("public-gateway")
    .Bind(9001)
    .EnableActorDispatch()
    .AddSession<GameSession>();
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.Bind(endpoint)` / `.Bind(port)` | port 생략 시 `0`(automatic discovery에서는 port 생략·`0` 모두 자동 bind) | 이 STREAM listener의 수신 port. `Bind(string endpoint)` overload도 있다 |
| `.SetBindHost(string)` / `.SetAdvertiseHost(string)` | root `ConfigureNetwork()` 기본값을 따름 | 이 listener에만 적용하는 bind·advertise host |
| `.ConfigureSocket()` | socket 기본값 | `MaxMessageSize`·HWM·buffer·timeout 등 이 listener 소켓의 세부 조정(`IZLinkSocketConfig`) |
| `.EnableActorDispatch()` | 비활성 | 수신 message를 Session에 연결된 Actor로 dispatch |
| `.SetTlsServer(certPath, keyPath, requireClientCertificate?)` | TLS 없음 | TLS 서버 인증서·키, 상호 인증 여부 |
| `.AddSession<TSession>()` | 없음 | 연결마다 생성할 Session 타입 등록 |

**완료 결과.** 반환값 없이 동기로 등록된다. TLS 설정 오류는 host startup 검증에서
`ZLinkConfigurationException`으로 드러난다.

**선택 기준.** 외부 client가 STREAM 프로토콜로 직접 연결하는 gateway를 열 때 쓴다. 정확한 Session·Actor
연결 규칙은 stream-session category를 참고한다.

---

## Manual peer 연결 (구성 시점·런타임)

Automatic discovery 없이 특정 endpoint에 수동으로 연결한다. `MeshNodeBuilder.PeerConnections`로
호출한다.

```csharp
play.PeerConnections.Connect(RoutingId.From("play-node-2"), "play-node-2:5501");
IReadOnlyList<ZLinkMeshPeerConnection> connections = play.PeerConnections.ListConnections();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.Connect(endpoint)` | expected RID 없음 | Admission handshake가 remote identity를 결정 |
| `.Connect(expectedRoutingId, endpoint)` | — | Handshake identity가 다르면 admission하지 않음 |
| `.Disconnect(endpoint)` | — | 등록한 연결을 해제 |
| `.ListConnections()` | — | 현재 등록된 연결 목록 조회 |

**완료 결과.** 반환값 없이 동기로 등록·해제된다. 양쪽 MeshNode가 Object Client이고 둘 다 RouteMesh
Channel Server membership이 없으면 이 연결 intent는 목록에 남아도 ready peer가 되지 않고, ready peer
수·liveness 대상에도 포함되지 않는다. 어느 한쪽에라도 weight `0`을 포함한 Channel Server
membership이 있으면 일반 peer admission·liveness 규칙을 적용한다.

**선택 기준.** Automatic discovery(Location Store)를 쓰지 않고 고정된 peer 목록으로 RouteMesh를
구성할 때 쓴다.

---

## `UseFilter<TFilter>` (구성 시점)

모든 handler dispatch 앞에 공통 로직(인증, 로깅 등)을 끼워 넣는다.

```csharp
options.UseFilter<AuthenticationFilter>();

public sealed class AuthenticationFilter : IZLinkHandlerFilter
{
    public async ValueTask InvokeAsync(
        IZLinkHandlerFilterContext context,
        ZLinkHandlerFilterNext next,
        CancellationToken cancellationToken)
    {
        if (!IsAuthenticated(context))
        {
            return; // next()를 호출하지 않으면 request는 Rejected로 끝난다
        }

        await next();
    }
}
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.UseFilter<TFilter>()` | 없음(등록한 순서대로 실행) | `IZLinkHandlerFilter` 구현체를 dispatch 체인에 추가 |

**완료 결과.** 반환값 없이 동기로 등록된다. `next()`를 호출하면 남은 filter와 handler가
실행된다. Request에서 `next()`를 호출하지 않으면 `Rejected` reply로 끝나고, `next()`를 두 번
호출하면 `InvalidOperation`으로 실패하며 handler를 다시 실행하지 않는다. `context.DispatchKind`로
`NodeDirectSend`/`NodeDirectRequest`/`ChannelSend`/`ChannelRequest`/`ClassicFanout`을 구분한다 —
`ChannelSend`/`ChannelRequest`는 RouteMesh와 ClientServer를 모두 포함한다.

**선택 기준.** 개별 handler마다 반복할 공통 전처리·검증이 필요할 때 쓴다. Filter는 업무 reply를
직접 만들지 않는다 — 거부만 표현하고 나머지는 handler가 처리한다.

---

## 기타 host-wide 옵션 (구성 시점)

`IZLinkFrameworkOptions`가 제공하는 단순 property-bag 성격의 구성이다. 각 설정은 독립적으로
호출한다.

```csharp
services.AddZLinkFramework(options =>
{
    options.AddHandlersFromAssemblyOf<GameEntrySpot>(); // attribute로 표시한 handler를 assembly에서 찾아 등록
    options.ConfigureNetwork().BindHost = "0.0.0.0";
    options.ConfigureInboundDispatch().ApplicationHwmProfile = ZLinkApplicationHwmProfile.LowLatency;
    options.ConfigureMetadata()
        .AllowSessionToActor("trace-id")
        .AllowActorToSession("server-region"); // forward할 metadata key allowlist
    options.ConfigureStreamCompression().UseLz4();
});
```

**옵션.** 자주 쓰는 항목은 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.AddHandlersFromAssemblyOf<TMarker>()` / `.AddHandlersFromAssembly(assembly)` / `.AddHandlersFromAssemblyOf(Type markerType)` | implicit auto-registration 활성 | attribute 기반 handler assembly scan 대상 지정. 세 overload 모두 동일한 scan을 수행한다 |
| `.DisableImplicitHandlerAutoRegistration()` | 활성(자동 scan) | Attribute 기반 자동 등록을 끄고 명시적 builder 등록만 사용 |
| `.ConfigureMetadata().AllowSessionToActor(key)` / `.AllowActorToSession(key)` | 지정하지 않은 key는 forward 안 함 | STREAM session↔Actor relay로 넘길 metadata key를 각 방향별로 allowlist에 추가 |
| `.ConfigureNetwork()` | `BindHost`는 전체 인터페이스 | 개별 Listen 호출이 override하지 않는 한 쓰는 기본 bind·advertise host. `IZLinkNetworkOptions`(property-bag)를 반환한다 |
| `.ConfigureInboundDispatch()` | `ApplicationHwmProfile = Balanced` | Inbound application HWM 크기·profile, process 메모리 상한. `IZLinkInboundDispatchOptions`를 반환한다 |
| `.ConfigureDispatch().Unhandled` | Framework 기본 정책 | 일치하는 handler가 없는 packet의 처리 방식 |
| `.ConfigureStreamCompression()` | 압축 없음 | STREAM 기본 압축 codec(`UseDefault()`/`UseLz4()`/`Use(codec)`/`Disable()`) |
| `.ConfigureRouterSocket()` / `.ConfigureSpotPublisher()`(MeshNodeBuilder) | socket 기본값 | MeshNode ROUTER 소켓, Spot publisher의 HWM·buffer·timeout 개별 조정 |

**완료 결과.** `.AddHandlersFromAssemblyOf(...)`/`.DisableImplicitHandlerAutoRegistration()`은
반환값 없이 동기로 실행된다. `.ConfigureMetadata()`/`.ConfigureNetwork()`/`.ConfigureInboundDispatch()`/`.ConfigureDispatch()`/`.ConfigureStreamCompression()`은
동기적으로 해당 builder나 options 객체를 반환하며, 그 위에서 property를 설정하거나 추가 modifier를
호출한다. 값 범위를 벗어나면 host startup 검증에서 `ZLinkConfigurationException`으로 드러난다.

**선택 기준.** 위 전용 항목(host lifecycle·topology 등록·diagnostics)에 속하지 않는, 단순 값 하나로
끝나는 host-wide 설정을 조정할 때 쓴다. Diagnostics 관련 설정은 observability-diagnostics
category를 쓴다.

---

## 런타임 weight 조회·변경

배포를 다시 하지 않고 placement weight나 channel weight를 바꾼다.

```csharp
IZLinkMeshPlacementRuntimeOptions placement = routeMeshRuntimeOptions.Mesh("play");
placement.PlacementWeight = 50; // 이 node로 가는 새 Actor·Spot 배치 비중을 낮춘다

IZLinkMeshChannelRuntimeOptions channel = routeMeshRuntimeOptions.Channel("play.api");
channel.Weight = 0; // 이 Channel Server를 선택 대상에서 제외한다
```

**옵션.** 이 진입점에는 두 개의 독립된 property가 있다.

| Property | 기본값 | 의미 |
| --- | --- | --- |
| `Mesh(meshName).PlacementWeight` | 등록 시점 값 | node 단위 Actor·Spot 배치 가중치 |
| `Channel(channelName).Weight` | 등록 시점 값 | ChannelName 단위 Server 선택 가중치 |

**완료 결과.** 동기 get/set이다. 즉시 적용되며 별도 완료 신호가 없다.

**선택 기준.** 운영 중 배치나 트래픽 비중을 조정할 때 쓴다. `MaxMessageSize`를 포함한 transport
option은 이 경로로 바꿀 수 없다 — startup 전에만 설정한다.

---

## Topology 상태 조회·관찰

RouteMesh·ClientServer·Fanout 각각의 운영 상태를 확인한다. 세 runtime이 같은 모양(`GetStatus`
한 번 조회, `ObserveAsync`로 스트리밍 관찰)을 제공한다.

```csharp
ZLinkRouteMeshStatus status = routeMeshRuntime.GetStatus("play");
bool canPlaceNewObjects = status.IsReady && status.Placement.IsAvailable;

await foreach (var observed in routeMeshRuntime.ObserveAsync("play", ct))
{
    // observed.Status.Channels, observed.Status.Peers를 확인한다
}
```

**옵션.** 세 runtime의 대응 관계는 다음과 같다.

| Runtime | 대상 | 반환 status |
| --- | --- | --- |
| `IZLinkRouteMeshRuntime` | MeshName | `ZLinkRouteMeshStatus`(Channels, Peers, Placement 포함) |
| `IZLinkClientServerRuntime` | ChannelName | `ZLinkClientServerStatus`(Targets 포함) |
| `IZLinkFanoutRuntime` | ChannelName | `ZLinkFanoutStatus`(Publishers 포함) |

**완료 결과.** `GetStatus`는 즉시 값을 반환하는 동기 호출이다. `ObserveAsync`는 host-lifecycle
category의 `ObserveAsync`와 같은 모양으로 `ZLinkObservedStatus<TStatus>`를 스트리밍하며,
`Loss` 필드로 관찰 유실 여부를 판단한다. Manual ChannelName을 `IZLinkFanoutRuntime`으로 조회하면
`ZLinkConfigurationException`으로 완료한다.

**선택 기준.** 특정 MeshName·ChannelName의 가용성을 판단하거나 장애 범위를 좁힐 때 쓴다. Host
전체 상태가 필요하면 host-lifecycle category의 `Status`/`ObserveAsync`를 쓴다.

---

전체 근거는
[RouteMesh·MeshNode exact interface](../../common/spec/server/languages/dotnet/interfaces/03-configuration-topology.ko.md)와
[Topology monitoring exact interface](../../common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md)를
참고한다.
