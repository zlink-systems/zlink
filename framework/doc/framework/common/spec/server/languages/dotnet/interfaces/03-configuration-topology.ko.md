# .NET RouteMesh·MeshNode 공개 인터페이스

[.NET 언어별 interface 목차](README.ko.md) · [공통 topology](../../../02-channel-transport/01-channel-topology.ko.md) ·
[MeshNode](../../../03-spot-actor/03-mesh-node.ko.md) · [메시지 모델](../../../00-foundation/05-message-model.ko.md)

## 1. 범위

이 문서는 ZLink Framework의 .NET RouteMesh·MeshNode 공개 인터페이스를 고정한다. 대상 독자는
.NET application 개발자와 public provider 구현자다. 물리 mesh 등록, 논리 channel membership, manual peer, handler,
Spot·Actor 등록과 실행 중 weight 변경의 정확한 C# signature를 이 문서가 소유한다.

## 2. 등록 인터페이스

```csharp
public interface IZLinkFrameworkOptions
{
    TimeSpan DefaultRequestTimeout { get; set; }
    TimeSpan DefaultSocketSendTimeout { get; set; }
    TimeSpan SessionReplacementCallbackTimeout { get; set; }
    long ApplicationVersion { get; set; }
    string? MaintenanceWave { get; set; }
    IZLinkCodecRegistryBuilder Codecs { get; }
    IZLinkWorkerOptions Worker { get; }

    void AddHandlersFromAssemblyOf<TMarker>();
    void AddHandlersFromAssemblyOf(Type markerType);
    void AddHandlersFromAssembly(System.Reflection.Assembly assembly);
    void DisableImplicitHandlerAutoRegistration();
    IZLinkMetadataPolicyBuilder ConfigureMetadata();
    void AddLocationStore(IZLinkLocationStore store);
    void AddRelocationStore(IZLinkRelocationStore store);
    ZLinkLocationOptions ConfigureLocations();
    IZLinkNetworkOptions ConfigureNetwork();
    IZLinkDispatchOptions ConfigureDispatch();
    IZLinkInboundDispatchOptions ConfigureInboundDispatch();
    IZLinkStreamCompressionBuilder ConfigureStreamCompression();
    void UseFilter<TFilter>() where TFilter : class, IZLinkHandlerFilter;

    IZLinkMeshNodeBuilder AddRouteMesh(string meshName);
    IZLinkClientServerChannelRoleBuilder AddClientServerChannel(string channelName);
    IZLinkFanoutChannelBuilder AddFanoutChannel(string channelName);
    IZLinkStreamNodeBuilder AddStreamNode(string streamNodeName);
}

public enum ZLinkCoreHwmProfile
{
    Compact = 0,
    LowLatency = 1,
    Balanced = 2,
    Throughput = 3
}

public enum ZLinkApplicationJobQueueProfile
{
    Compact = 0,
    LowLatency = 1,
    Balanced = 2,
    Throughput = 3
}

public enum ZLinkApplicationJobQueuePressureState
{
    Running = 0,
    Paused = 1
}

public interface IZLinkMeshNodeBuilder
{
    IZLinkMeshChannelRoleBuilder Channel(string channelName);
    IZLinkMeshNodeBuilder Listen(string endpoint);
    IZLinkMeshNodeBuilder Listen(int port = 0);
    IZLinkMeshNodeBuilder SetBindHost(string bindHost);
    IZLinkMeshNodeBuilder SetAdvertiseHost(string advertiseHost);
    IZLinkMeshNodeBuilder SetRoutingId(RoutingId routingId);
    IZLinkMeshNodeBuilder SetRoutingIdPrefix(string prefix);
    IZLinkMeshNodeBuilder SetPlacementWeight(int weight);
    IZLinkMeshNodeBuilder SetActorLimit(int limit);
    IZLinkMeshNodeBuilder SetSpotLimit(int limit);
    IZLinkMeshNodeBuilder SetActivationConcurrency(int limit);
    IZLinkMeshNodeBuilder SetInstanceSpotIdleTimeout(TimeSpan timeout);
    IZLinkMeshObjectRoleBuilder Objects();
    IZLinkMeshNodeSocketConfig ConfigureRouterSocket();
    IZLinkSpotPublisherConfig ConfigureSpotPublisher();
    IZLinkMeshPeerConnections PeerConnections { get; }

    IZLinkMeshNodeBuilder SetDefaultRequestTimeout(TimeSpan timeout);
    IZLinkMeshNodeBuilder AddRouteSendHandler<THandler, TMessage>(
        string? packetName = null)
        where THandler : class, IZLinkRouteSendHandler<TMessage>;
    IZLinkMeshNodeBuilder AddRouteSendHandler<THandler>(string? packetName = null)
        where THandler : class;
    IZLinkMeshNodeBuilder AddRouteRequestHandler<THandler, TRequest, TReply>(
        string? packetName = null)
        where THandler : class, IZLinkRouteRequestHandler<TRequest, TReply>;
    IZLinkMeshNodeBuilder AddRouteRequestHandler<THandler>(string? packetName = null)
        where THandler : class;

}

public interface IZLinkMeshObjectRoleBuilder
{
    IZLinkMeshObjectClientBuilder Client();
    IZLinkMeshObjectServerBuilder Server();
}

public interface IZLinkMeshObjectClientBuilder
{
}

public interface IZLinkMeshObjectServerBuilder
{
    IZLinkMeshObjectServerBuilder AddEntrySpot<TEntrySpot>()
        where TEntrySpot : class, IZLinkEntrySpot;
    IZLinkMeshObjectServerBuilder AddSpotFactory<TSpot>(
        string spotType,
        Action<IZLinkUserSpotFactoryBuilder<TSpot>> configure)
        where TSpot : class, IZLinkSpot;
    IZLinkMeshObjectServerBuilder AddInstanceSpotFactory<TSpot>(
        string instanceSpotType,
        Action<IZLinkInstanceSpotFactoryBuilder<TSpot>> configure)
        where TSpot : class, IZLinkInstanceSpot;
    IZLinkMeshObjectServerBuilder AddActorFactory<TActor, TFactory>(
        string actorType,
        Action<IZLinkActorFactoryBuilder<TActor>> configure)
        where TActor : class, IZLinkActor
        where TFactory : class, IZLinkActorFactory<TActor>;
}

public enum ZLinkUserSpotExecutionMode
{
    SpotWide = 0,
    PerActor = 1
}

public enum ZLinkSpotRelocationCoordinationMode
{
    FrameworkManaged = 0,
    ApplicationSignaled = 1
}

public interface IZLinkActorFactoryBuilder<TActor>
    where TActor : class, IZLinkActor
{
    IZLinkActorFactoryBuilder<TActor> DisableRelocation();
    IZLinkActorFactoryBuilder<TActor> RecreateOnRelocation();
    IZLinkActorFactoryBuilder<TActor> PreserveStateWith<TAdapter>()
        where TAdapter : class, IZLinkActorRelocationAdapter<TActor>;
}

public interface IZLinkUserSpotFactoryBuilder<TSpot>
    where TSpot : class, IZLinkSpot
{
    IZLinkUserSpotFactoryBuilder<TSpot> StableTypeLimit(int limit);
    IZLinkUserSpotFactoryBuilder<TSpot> ExecutionMode(
        ZLinkUserSpotExecutionMode mode);
    IZLinkUserSpotFactoryBuilder<TSpot> RelocationCoordinationMode(
        ZLinkSpotRelocationCoordinationMode mode);
    IZLinkUserSpotFactoryBuilder<TSpot> DisableRelocation();
    IZLinkUserSpotFactoryBuilder<TSpot> RecreateOnRelocation();
    IZLinkUserSpotFactoryBuilder<TSpot> PreserveStateWith<TAdapter>()
        where TAdapter : class, IZLinkSpotRelocationAdapter<TSpot>;
}

public interface IZLinkInstanceSpotFactoryBuilder<TSpot>
    where TSpot : class, IZLinkInstanceSpot
{
    IZLinkInstanceSpotFactoryBuilder<TSpot> StableTypeLimit(int limit);
    IZLinkInstanceSpotFactoryBuilder<TSpot> DisableRelocation();
    IZLinkInstanceSpotFactoryBuilder<TSpot> RecreateOnRelocation();
    IZLinkInstanceSpotFactoryBuilder<TSpot> PreserveStateWith<TAdapter>()
        where TAdapter : class, IZLinkSpotRelocationAdapter<TSpot>;
}

public interface IZLinkNetworkOptions
{
    string BindHost { get; set; }
    string? AdvertiseHost { get; set; }
}

public interface IZLinkMeshChannelRoleBuilder
{
    IZLinkMeshChannelClientBuilder Client();
    IZLinkMeshChannelServerBuilder Server();
}

public interface IZLinkMeshChannelClientBuilder
{
}

public interface IZLinkMeshChannelServerBuilder
{
    IZLinkMeshChannelServerBuilder SetWeight(int weight);
    IZLinkMeshChannelServerBuilder AddHandlerGroup(string groupName);
    IZLinkMeshChannelServerBuilder AddSendHandler<THandler, TMessage>(
        string? packetName = null)
        where THandler : class, IZLinkSendHandler<TMessage>;
    IZLinkMeshChannelServerBuilder AddSendHandler<THandler>(string? packetName = null)
        where THandler : class;
    IZLinkMeshChannelServerBuilder AddRequestHandler<THandler, TRequest, TReply>(
        string? packetName = null)
        where THandler : class, IZLinkRequestHandler<TRequest, TReply>;
    IZLinkMeshChannelServerBuilder AddRequestHandler<THandler>(string? packetName = null)
        where THandler : class;
}

public interface IZLinkClientServerChannelRoleBuilder
{
    IZLinkClientServerChannelClientBuilder Client();
    IZLinkClientServerChannelServerBuilder Server();
}

public interface IZLinkClientServerChannelClientBuilder
{
    IZLinkClientServerChannelClientBuilder Connect(string endpoint);
}

public interface IZLinkClientServerChannelServerBuilder
{
    IZLinkClientServerChannelServerBuilder Listen(int port = 0);
    IZLinkClientServerChannelServerBuilder SetBindHost(string bindHost);
    IZLinkClientServerChannelServerBuilder SetAdvertiseHost(string advertiseHost);
    IZLinkClientServerChannelServerBuilder SetWeight(int weight);
    IZLinkClientServerChannelServerBuilder AddHandlerGroup(string groupName);
    IZLinkClientServerChannelServerBuilder AddSendHandler<THandler, TMessage>(
        string? packetName = null)
        where THandler : class, IZLinkSendHandler<TMessage>;
    IZLinkClientServerChannelServerBuilder AddRequestHandler<THandler, TRequest, TReply>(
        string? packetName = null)
        where THandler : class, IZLinkRequestHandler<TRequest, TReply>;
}

public interface IZLinkEndpointConnections
{
    void Connect(string endpoint);
    void Disconnect(string endpoint);
    IReadOnlyList<string> ListConnections();
}

public interface IZLinkFanoutChannelBuilder
{
    IZLinkFanoutChannelBuilder EnablePublisher(string endpoint);
    IZLinkFanoutChannelBuilder EnablePublisher(int port = 0);
    IZLinkFanoutChannelBuilder SetBindHost(string bindHost);
    IZLinkFanoutChannelBuilder SetAdvertiseHost(string advertiseHost);
    IZLinkFanoutChannelBuilder SetRoutingId(RoutingId publisherRoutingId);
    IZLinkFanoutChannelBuilder SetRoutingIdPrefix(string prefix);
    IZLinkFanoutChannelBuilder SetNoDrop(bool noDrop = true);
    IZLinkFanoutChannelBuilder EnableSubscriber();
    IZLinkFanoutChannelBuilder Subscribe(string topic);
    IZLinkFanoutChannelBuilder Connect(string endpoint);
    IZLinkEndpointConnections SubscriberConnections { get; }
    IZLinkFanoutChannelBuilder AddHandler<THandler, TEvent>(
        string? packetName = null)
        where THandler : class, IZLinkFanoutHandler<TEvent>;
}

public interface IZLinkStreamNodeBuilder
{
    IZLinkStreamNodeBuilder Bind(string endpoint);
    IZLinkStreamNodeBuilder Bind(int port = 0);
    IZLinkStreamNodeBuilder SetBindHost(string bindHost);
    IZLinkStreamNodeBuilder SetAdvertiseHost(string advertiseHost);
    IZLinkStreamNodeBuilder MaxMessageSize(long bytes);
    IZLinkStreamSocketConfig ConfigureSocket();
    IZLinkStreamNodeBuilder EnableActorDispatch();
    IZLinkStreamNodeBuilder SetTlsServer(
        string certificatePath,
        string keyPath,
        bool requireClientCertificate = false);
    IZLinkStreamNodeBuilder AddSession<TSession>()
        where TSession : class, IZLinkSession;
}

public interface IZLinkStreamCompressionBuilder
{
    IZLinkStreamCompressionBuilder UseDefault();
    IZLinkStreamCompressionBuilder UseLz4();
    IZLinkStreamCompressionBuilder Use(IZlinkStreamCompressionCodec codec);
    IZLinkStreamCompressionBuilder Disable();
}

public interface IZLinkMetadataPolicyBuilder
{
    IZLinkMetadataPolicyBuilder AllowSessionToActor(string key);
    IZLinkMetadataPolicyBuilder AllowActorToSession(string key);
}

```

`IZLinkStreamNodeBuilder.MaxMessageSize(long bytes)`의 기본값은 `64 KiB`다. 이 값은
StreamNode의 Core STREAM inbound에서 client→server complete message를 검사할 때만 사용하며,
6-byte prefix를 제외한 header와 payload의 합으로 계산한다. `0`은 Core `-1`로 변환되어
Framework 상한을 사용하지 않고, 음수는 startup configuration error다. 상한을 넘은 message는
handler에 일부도 전달하지 않으며 server는 `EMSGSIZE`와 진단 trace를 남기고 연결을 종료한다.
raw client는 별도 wire error code가 아니라 연결 종료를 관찰한다. server→client outbound에는
이 Framework 상한을 적용하지 않는다. ClientServer와 RouteMesh SS에는 이 설정을 추가하지 않는다.

`IZLinkCodecRegistryBuilder`와 codec extension의 정확한 선언은
[Serialization](11-serialization.ko.md)이 소유한다.

`AddRouteMesh(meshName)`은 process-local [MeshNode](../../../00-foundation/02-glossary.ko.md#meshnode) 하나를 등록한다. 같은 process에서 같은 `meshName`을
두 번 등록하면 host startup이 `ZLinkConfigurationException`으로 실패한다. `Channel(channelName)` 뒤에는
`Client()` 또는 `Server()`를 정확히 한 번 호출한다. `Client()`는 송신 경로만 만들고, `Server()`만
[weight](../../../00-foundation/02-glossary.ko.md#weight)와 handler 등록을 제공한다. Server [membership](../../../00-foundation/02-glossary.ko.md#membership)이 없는 MeshNode도 시작할 수 있다.

[Channel topology §8](../../../02-channel-transport/01-channel-topology.ko.md)가 RouteMesh 연결 방향과 peer 필요성을 정한다.

Manual handshake와 `NotRequired` 처리는 [Channel topology §8](../../../02-channel-transport/01-channel-topology.ko.md)가 정한다.

`Listen(string endpoint)`, `Bind(string endpoint)`와 `EnablePublisher(string endpoint)`를 제공하며,
host·port 조합 overload도 같은 listener 설정을 표현한다.

`AddClientServerChannel(channelName)`은 `Client()`와 `Server()`를 제공한다. 등록과 연결은 [ClientServer channel](../../../02-channel-transport/03-client-server-channel.ko.md)이 정한다.

Local Server의 후보 포함과 선택은 [ClientServer channel §4](../../../02-channel-transport/03-client-server-channel.ko.md)가 정한다.

`ConfigureNetwork()`의 기본 BindHost는 `127.0.0.1`이고 AdvertiseHost를 생략하면
[Network listener identity §2.1](../../../02-channel-transport/04-network-listener-identity.ko.md#21-기본값)의 기본값을 따른다. [Automatic discovery](../../../00-foundation/02-glossary.ko.md#automatic-discovery) listener는 `Listen()`·`Bind()`·`EnablePublisher()`의 port를 생략하거나
listener 호출 자체를 생략하면 port `0`으로 bind한다. Manual mode에서 endpoint를 다른 discovery source로
얻지 못하면 listen port와 remote endpoint를 명시한다. Listener별 host 설정은 root 기본값보다 우선한다.

Fanout descriptor, discovery와 연결 방향은 [Channel topology](../../../02-channel-transport/01-channel-topology.ko.md)이 정한다. 이 builder는 `EnableSubscriber()`와 `Connect(endpoint)`를 제공한다.

Automatic RID는 `prefix-<lowercase-canonical-uuid-v4>` 형식이다. UUID v4는 `8-4-4-4-12` 자리의
lowercase canonical 문자열로 표현한다. Prefix는 ASCII `[A-Za-z0-9._-]` 1..64자이고 full RID는 UTF-8
255 bytes 이하다. Active owner와 충돌하면 새 UUID로 다시 시도하지 않고 즉시 `RoutingIdConflict`로
실패한다. Fixed RID의 사용 범위와 재시작 충돌은 [공통 MeshNode §3.3](../../../03-spot-actor/03-mesh-node.ko.md#33-fixed-rid)이 정한다. Slot count, allocation group과 public allocation provider는 제공하지 않는다.

Object Server의 Entry Spot ID에도 같은 prefix를 사용하지만 MeshNode RID와 별도로 생성한 UUID v4를
붙인다. 형식은 `<prefix>-entry-<lowercase-canonical-uuid-v4>`이며 caller가 fixed Entry Spot ID를 지정하지
않는다. 이 ID의 전역 충돌과 caller가 지정한 Spot ID의 예약 형식 검증은
[Spot model](../../../03-spot-actor/01-spot-model.ko.md)이 정의한다. Prefix와 생성된 RID·Spot ID를 placement, shard 또는
stable application identity로 해석하지 않는다.

등록한 MeshNode descriptor는 1 MiB 이하여야 한다. [Spot](../../../00-foundation/02-glossary.ko.md#spot) type과 stateful object capability collection은 각각
최대 1024개다. Bound를 넘으면 startup을 실패시키며 일부 registration만 적용하지 않는다.

`SubscriberConnections`는 manual subscriber endpoint 집합의 runtime handle이다. Builder에서 등록한
endpoint와 같은 집합을 대상으로 연결, 해제와 현재 목록 조회를 제공한다. Automatic subscriber의
discovery 결과는 이 handle로 변경하지 않는다.

`AddHandlersFromAssemblyOf(...)`와 `AddHandlersFromAssembly(...)`는 명시한 assembly만 handler scan 범위로
추가한다. Scan에 사용하는 method, group과 packet attribute의 정확한 선언은
[Common runtime](01-common-runtime.ko.md)가 소유한다.

`EnableActorDispatch()`는 STREAM node의 Actor dispatch capability만 활성화한다. 같은 host에 object role이
`Client` 또는 `Server`인 Mesh와 Location Store가 없으면 startup이 실패한다. Global ActorId가 current Mesh와
owner route를 결정하므로 이 설정은 MeshName을 받지 않는다.

`DefaultRequestTimeout`의 기본값은 30초, `DefaultSocketSendTimeout`의 기본값은 1초다.
`SessionReplacementCallbackTimeout`은 actor binding 교체 callback이 실행될 수 있는 최대 시간이며 기본값은
30초다. 이 시간을 넘기면 Framework가 물러난 session을 강제로 닫는다. `Worker`는 worker의
최소·최대 thread 수와 idle timeout을 host startup 전에 설정한다.

`ConfigureStreamCompression()`과 `IZLinkStreamCompressionBuilder`는 STREAM payload compression을 고른다.
이 builder는 service transport lifecycle이나 relocation codec을 설정하지 않는다.

`ApplicationVersion`은 host 전체에 한 번 설정하며 `0..long.MaxValue` 범위이고 기본값은 `0`이다. 모든 local
MeshNode가 이 값을 게시하며 음수는 startup 전에 `ZLinkConfigurationException`으로 거부한다.
`MaintenanceWave`는 `null`이면 wave exclusion을 사용하지 않는 stable ID다.

`Objects().Client()`와 `Objects().Server()`는 .NET role builder다. 역할은 [MeshNode](../../../03-spot-actor/03-mesh-node.ko.md)가 정한다.

Object Client의 Channel peer 연결 조건은 [Channel topology](../../../02-channel-transport/01-channel-topology.ko.md)가 정한다. 잘못된 Node direct handler 등록은 `ZLinkConfigurationException`으로 표현한다.

Object Client pair의 연결 필요성은 [Channel topology](../../../02-channel-transport/01-channel-topology.ko.md)가 정한다.

Actor·User Spot·Instance Spot [factory](../../../00-foundation/02-glossary.ko.md#factory)는 stable type, object 종류별 factory option과 explicit relocation
policy를 같은 registration에서 고정한다. Policy를 생략하는 overload는 없다. [Stable type](../../../00-foundation/02-glossary.ko.md#stable-type)은 UTF-8
1..255 bytes이고 중복 type은 startup 오류다. Entry Spot ID는 Framework가 발급한다.

.NET placement builder는 weight와 population capacity option을 제공한다. Eligibility와 capacity 판정은 [Location runtime](../../../05-location-relocation/01-location-runtime.ko.md)이 정한다.

`SetInstanceSpotIdleTimeout(...)`은 .NET option이다. 기본값, 검증과 유휴 정리는 [Spot 모델 §6.2](../../../03-spot-actor/01-spot-model.ko.md)가 정한다.

## 3. Manual peer

```csharp
public readonly record struct ZLinkMeshPeerConnection(
    string Endpoint,
    RoutingId? ExpectedRoutingId);

public interface IZLinkMeshPeerConnections
{
    void Connect(string endpoint);
    void Connect(RoutingId expectedRoutingId, string endpoint);
    void Disconnect(string endpoint);
    IReadOnlyList<ZLinkMeshPeerConnection> ListConnections();
}
```

`Connect(...)` peer의 수락과 liveness는 [Channel topology §8](../../../02-channel-transport/01-channel-topology.ko.md)가 정한다.

Handler filter는 application이 구현하고 root에 등록하는 public extension point다. `next`를 호출하면 남은
filter와 handler가 실행된다. 호출하지 않은 request는 `Rejected`로 끝나며 filter가 업무 reply를 직접
만들지 않는다. 적용 범위, 실행 순서와 fanout 격리는 공통 Framework API가 정한다.

```csharp
public enum ZLinkHandlerDispatchKind
{
    NodeDirectSend = 0,
    NodeDirectRequest = 1,
    ChannelSend = 2,
    ChannelRequest = 3,
    ClassicFanout = 4
}

public interface IZLinkHandlerFilterContext : IZLinkMessageContext
{
    ZLinkHandlerDispatchKind DispatchKind { get; }
}

public delegate ValueTask ZLinkHandlerFilterNext();

public interface IZLinkHandlerFilter
{
    ValueTask InvokeAsync(
        IZLinkHandlerFilterContext context,
        ZLinkHandlerFilterNext next,
        CancellationToken cancellationToken);
}
```

`ChannelSend`와 `ChannelRequest`는 RouteMesh와 ClientServer를 포함한다. .NET은 filter 실패를 `ZLinkFrameworkErrorKind.InvalidOperation`과 `Rejected`로 표현한다. 실행과 거부는 [Framework API §10](../../../00-foundation/06-framework-api.ko.md)이 정한다.

`AddInstanceSpotFactory`의 type 이름은 비어 있을 수 없고 UTF-8로 255 byte 이하여야 한다. Type별 active와
pending limit은 생략할 수 있지만 명시한 값은 1..`int.MaxValue`다.
같은 MeshNode에서 같은 stable type 또는 같은 implementation class를
User Spot factory와 Instance factory에 중복 등록할 수 없다. `TSpot`이 닫힌 generic
`IZLinkUserSpotActorLifecycle<TActor>`도 구현하면
actor-free 계약과 충돌하므로 startup이 실패한다. 두 option은 local MeshNode와 Instance type별로 적용한다.
등록한 type set은 descriptor를 처음 게시하기 전에 고정하며 startup 이후 변경하지 않는다.

Factory configure callback은 option과 relocation policy를 한 builder에서 설정한다. Callback은
`DisableRelocation()`, `RecreateOnRelocation()`, `PreserveStateWith<TAdapter>()` 중 정확히 하나를 호출해야 한다.
하나도 선택하지 않거나 둘 이상 선택하면 socket bind 전에 startup configuration error다. Actor builder는
`IZLinkActorRelocationAdapter<TActor>`, User·Instance Spot builder는 `IZLinkSpotRelocationAdapter<TSpot>`만
받는다. Factory 대상과 adapter 종류가 맞지 않아도 같은 오류로 실패한다. 별도 등록 API는 없다.

Framework는 등록 호출 안에서 callback을 동기적으로 한 번 실행한다. Callback이 반환되면 builder 구성을
고정한다. Application이 callback 밖에 builder를 보관했다가 다시 호출하면 configuration error다.
Callback이 예외를 던지면 factory를 등록하지 않고 같은 예외를 호출자에게 전달한다.

PerActor relocation policy는 [Spot 모델](../../../03-spot-actor/01-spot-model.ko.md)이 정한다. .NET option 이름은 `ZLinkUserSpotExecutionMode.PerActor`다.

Execution·coordination mode의 기본값과 허용 범위는 [Spot 모델](../../../03-spot-actor/01-spot-model.ko.md)이 정한다. .NET은 `SpotWide`, `FrameworkManaged`, `ApplicationSignaled`를 제공한다.

expected RID를 생략하면 admission handshake가 remote identity를 결정한다. expected RID를 지정한 경우
handshake identity가 다르면 연결을 admission하지 않는다. Manual 연결도 자동 discovery 연결과 같은
[MeshName](../../../00-foundation/02-glossary.ko.md#meshname)·RID·ChannelName·security 검증을 사용한다.

## 4. Handler와 filter의 dispatch scope

Node direct·Channel send/request와 classic fanout 구독 handler를 실행할 때마다 DI scope를 하나
만든다. Handler와 filter는 이 scope에서 Framework가 한 번씩 만들며 같은 scoped dependency를
사용한다. Classic fanout message가 여러 구독 handler와 일치하면 구독 handler마다 별도 scope를
만든다.
Application이 handler나 filter type을 singleton·scoped·transient로 등록해도 이
수명은 바뀌지 않는다. Dispatch가 끝나면 Framework가 만든 instance를 먼저 정리하고
scope를 정리한다.

Channel handler는 `(ChannelName, message kind, packet name)`으로 구분한다. RID direct route
handler는 MeshNode builder에 등록하며 source RID를 제공하는 route handler context를 사용한다. 같은 key의
중복 등록은 startup 오류이고, 서로 다른 channel이나 route family에 같은 packet name을 등록할 수 있다.

`AddHandlerGroup(groupName)`은 scan으로 찾은 handler 중 같은 `ZLinkHandlerGroupAttribute` 값을 가진
send/request handler를 해당 ChannelName에 노출한다. TicTacToe의 수동 topology는 handler를
수동 등록한다는 뜻이 아니다. .NET 샘플은 assembly scan과 `AddHandlerGroup(...)`으로 handler를
노출하며, typed `AddSendHandler(...)`·`AddRequestHandler(...)`는 별도의 직접 등록 계약을
보여 주는 경우에만 사용한다.

`IZLinkMeshChannelServerBuilder`와 `IZLinkClientServerChannelServerBuilder`의 weight는 0부터 10000까지이고
기본값은 100이다. 범위 밖 값은 startup 설정과 runtime 변경에서 `ZLinkConfigurationException`이다.
Node placement를 포함한 weighted selection은 후보 weight 합계를 최소 64-bit 정수로 계산한다.
0은 해당 channel의 새 select-one과 RouteMesh Logical Multicast remote target에서만
제외한다. RID direct route, 다른 membership과 이미 제출한 operation에는 영향을 주지 않는다.

## 5. Publisher와 runtime option

```csharp
public interface IZLinkSpotPublisherConfig
{
    ulong SendHighWaterMark { get; set; }
    TimeSpan? SendTimeout { get; set; }
    TimeSpan? Linger { get; set; }
}

public interface IZLinkSpotSubscriberConfig
{
    ulong ReceiveHighWaterMark { get; set; }
    TimeSpan? ReceiveTimeout { get; set; }
    TimeSpan? Linger { get; set; }
}

public interface IZLinkSocketConfig
{
    long MaxMessageSize { get; set; }
    ulong SendHighWaterMark { get; set; }
    ulong ReceiveHighWaterMark { get; set; }
    int SendBufferSize { get; set; }
    int ReceiveBufferSize { get; set; }
    TimeSpan? Linger { get; set; }
    TimeSpan? ReceiveTimeout { get; set; }
    TimeSpan? SendTimeout { get; set; }
    TimeSpan? ConnectTimeout { get; set; }
    TimeSpan? HandshakeInterval { get; set; }
    bool IPv6 { get; set; }
    bool TcpNoDelay { get; set; }
    bool Immediate { get; set; }
    int Weight { get; set; }
}

public interface IZLinkStreamSocketConfig
{
    ulong SendHighWaterMark { get; set; }
    ulong ReceiveHighWaterMark { get; set; }
    int SendBufferSize { get; set; }
    int ReceiveBufferSize { get; set; }
    TimeSpan? Linger { get; set; }
    TimeSpan? ReceiveTimeout { get; set; }
    TimeSpan? SendTimeout { get; set; }
    TimeSpan? ConnectTimeout { get; set; }
    TimeSpan? HandshakeInterval { get; set; }
    bool IPv6 { get; set; }
    bool TcpNoDelay { get; set; }
    bool Immediate { get; set; }
}

public interface IZLinkRouteConfig
{
    bool RequireKnownPeer { get; set; }
    bool AllowPeerHandover { get; set; }
    bool EnablePeerProbe { get; set; }
    RoutingId ConnectRoutingId { get; set; }
}

public interface IZLinkOutboundRouteConfig
{
    bool ProbeRouterOnConnect { get; set; }
}

public interface IZLinkRouteMeshRuntimeOptions
{
    IZLinkMeshPlacementRuntimeOptions Mesh(string meshName);
    IZLinkMeshChannelRuntimeOptions Channel(string channelName);
}

public interface IZLinkMeshPlacementRuntimeOptions
{
    int PlacementWeight { get; set; }
}

public interface IZLinkMeshChannelRuntimeOptions
{
    int Weight { get; set; }
}

public interface IZLinkMeshNodeSocketConfig
{
    ulong SendHighWaterMark { get; set; }
    ulong ReceiveHighWaterMark { get; set; }
    TimeSpan? ReceiveTimeout { get; set; }
    TimeSpan? SendTimeout { get; set; }
}
```

`MaxMessageSize`는 .NET ClientServer application listener option이다. 기본 한도, `0`의 의미와 RouteMesh 제외 범위는 [Framework API §4](../../../00-foundation/06-framework-api.ko.md#4-routemesh-등록)가 정한다.

`ConfigureSpotPublisher()`는 publish 전용 전달 정책 option을 제공하지 않는다.
Logical Multicast의 완료 경계는 [Submit과 completion §6](../../../01-execution/01-submit-and-completion.ko.md)이 정한다.

`IZLinkRouteMeshRuntimeOptions`는 public DI singleton이다. 등록되지 않은 membership을 조회하면
`ZLinkConfigurationException`이다.

실행 중에는 `Mesh(meshName).PlacementWeight`와 `Channel(channelName).Weight`를 변경할 수 있다.
두 weight는 서로 독립적이며 node weight는 object create·relocation target selection에만 사용한다.
ChannelName은 local RouteMesh 또는 ClientServer Server 등록을 유일하게 고른다. HWM과 timeout은
`ConfigureRouterSocket()`에서 startup 전에 설정한다.

`IZLinkMeshNodeSocketConfig`는 .NET socket 설정 표면이다. Message 한도와 자원 guard는 [Application job queue와 backpressure](../../../01-execution/04-application-job-queue-and-backpressure.ko.md)가 정한다.

`ConfigureInboundDispatch()`는 `IZLinkInboundDispatchOptions`를 반환한다. Core HWM, capacity와 threshold 판정은 [Application job queue와 backpressure](../../../01-execution/04-application-job-queue-and-backpressure.ko.md)가 정한다.

## 6. 메시징 metadata

Node direct, ChannelName, Spot direct, Actor send/request와 Logical Multicast call builder는 다음 overload를 공통으로 가진다.
handler context는 변경할 수 없는 `ZLinkMessageMetadata` snapshot을 제공한다.

```csharp
public interface IZLinkMetadataCall<TSelf>
{
    TSelf Metadata(string key, string value);
    TSelf Metadata(ZLinkMessageMetadata metadata);
}
```

같은 key를 여러 번 설정하면 마지막 값이 전송된다. metadata 전체의 UTF-8 encoded 크기는 1024 bytes를
넘을 수 없다. reply는 request metadata를 자동 복사하지 않으며 일반 reply에는 metadata setter를 두지
않는다. STREAM session과 Actor relay에 적용할 allowlist는 root `ConfigureMetadata()`가 소유한다.

## 7. Location store와 startup

자동 discovery, 분산 Spot·Actor 주소 또는 Actor relocation을 사용하는 host는 location store를 명시적으로
등록해야 한다. 공식 Redis location store package가 production 기본 구현이다. 등록이 없으면 host startup이
실패한다. process-local in-memory 구현은 단일 process contract test에서만 등록할 수 있다.
정확한 store capability와 Redis 생성자·option은
[.NET Location과 maintenance](08-location-maintenance.ko.md)가 소유한다.
