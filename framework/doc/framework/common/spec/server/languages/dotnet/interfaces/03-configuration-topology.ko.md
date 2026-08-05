# .NET RouteMesh·MeshNode 공개 인터페이스

[.NET exact interface 목차](README.ko.md) · [공통 topology](../../../../07-channel-topology.ko.md) ·
[MeshNode](../../../../13-mesh-node.ko.md) · [메시지 모델](../../../../04-message-model.ko.md)

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
    IZLinkInboundDispatchOptions ConfigureInboundDispatch();
    IZLinkNetworkOptions ConfigureNetwork();
    IZLinkDispatchOptions ConfigureDispatch();
    IZLinkStreamCompressionBuilder ConfigureStreamCompression();
    void UseFilter<TFilter>() where TFilter : class, IZLinkHandlerFilter;

    IZLinkMeshNodeBuilder AddRouteMesh(string meshName);
    IZLinkClientServerChannelRoleBuilder AddClientServerChannel(string channelName);
    IZLinkFanoutChannelBuilder AddFanoutChannel(string channelName);
    IZLinkStreamNodeBuilder AddStreamNode(string streamNodeName);
}

public enum ZLinkApplicationHwmProfile
{
    Compact = 0,
    LowLatency = 1,
    Balanced = 2,
    Throughput = 3
}

public interface IZLinkInboundDispatchOptions
{
    ulong? ApplicationHwmBytes { get; set; }
    ZLinkApplicationHwmProfile ApplicationHwmProfile { get; set; }
    ulong? ProcessMemoryLimitBytes { get; set; }
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

public enum ZLinkSpotRelocationReadinessMode
{
    AnyTurnBoundary = 0,
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
    IZLinkUserSpotFactoryBuilder<TSpot> RelocationReadiness(
        ZLinkSpotRelocationReadinessMode mode);
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
    IZLinkFanoutChannelBuilder EnableSubscriber();
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
    IZLinkSocketConfig ConfigureSocket();
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

`IZLinkStreamNodeBuilder.ConfigureSocket().MaxMessageSize`의 기본값은 `64 KiB`다. 이 값은
StreamNode의 Core STREAM inbound에서 client→server complete message를 검사할 때만 사용하며,
6-byte prefix를 제외한 header와 payload의 합으로 계산한다. `0`은 Core `-1`로 변환되어
Framework 상한을 사용하지 않고, 음수는 startup configuration error다. 상한을 넘은 message는
handler에 일부도 전달하지 않으며 server는 `EMSGSIZE`와 진단 trace를 남기고 연결을 종료한다.
raw client는 별도 wire error code가 아니라 연결 종료를 관찰한다. server→client outbound에는
이 Framework 상한을 적용하지 않는다. ClientServer와 RouteMesh SS에는 이 설정을 추가하지 않는다.

`IZLinkCodecRegistryBuilder`와 codec extension의 정확한 선언은
[Serialization](11-serialization.ko.md)이 소유한다.

`AddRouteMesh(meshName)`은 process-local [MeshNode](../../../../01-glossary.ko.md#meshnode) 하나를 등록한다. 같은 process에서 같은 `meshName`을
두 번 등록하면 host startup이 `ZLinkConfigurationException`으로 실패한다. `Channel(channelName)` 뒤에는
`Client()` 또는 `Server()`를 정확히 한 번 호출한다. `Client()`는 송신 경로만 만들고, `Server()`만
[weight](../../../../01-glossary.ko.md#weight)와 handler 등록을 제공한다. Server [membership](../../../../01-glossary.ko.md#membership)이 없는 MeshNode도 시작할 수 있다.

Automatic [RouteMesh](../../../../01-glossary.ko.md#routemesh)는 RID를 canonical byte order로 비교하고 더 작은 RID의 MeshNode만 상대 endpoint로
connect한다. Local과 remote의 object role이 모두 `Client`이고 양쪽 모두 RouteMesh Channel Server
membership이 없을 때만 connection intent를 만들지 않는다. Channel Client membership만으로는 연결하지
않는다. 어느 한쪽에라도 Channel Server membership이 있으면 weight가 `0`이어도 connection이 필요하다.
Manual topology는 application endpoint 구성에 따라 한쪽 또는 양쪽에서 connect할 수 있다.
양쪽 연결이나 automatic discovery 경합·오래된 snapshot으로 중복 후보가 생기면 handshake와 admission이
같은 RID와 lifecycle generation을 확인해 하나만 ready 상태로 유지한다.

Manual endpoint의 remote object role과 RouteMesh Server membership을 connect 전에 알 수 없으면
handshake에서 확인한다. 양쪽이 Object Client이고 양쪽 모두 RouteMesh Channel Server membership이
없을 때만 admission을 `NotRequired` terminal로 끝내고 ready 전에 socket을 닫는다. 같은 endpoint와
configuration generation에서는 background reconnect를 반복하지 않는다. Endpoint, expected RID 또는
configuration generation이 바뀌면 새 intent로 한 번 다시 확인한다.

`Listen(string endpoint)`, `Bind(string endpoint)`와 `EnablePublisher(string endpoint)`를 제공하며,
host·port 조합 overload도 같은 listener 설정을 표현한다.

`AddClientServerChannel(channelName)`은 `Client()`와 `Server()` 중 하나 또는 둘 다 등록할 수 있으며 각
역할은 최대 한 번만 등록한다. Registration key는 `(ChannelName, Role)`이며 Client와 Server는 별도
registration으로 하나의 ClientServer topology를 공유한다. 같은 역할을 두 번 등록하면 startup이 실패한다.
RouteMesh ChannelName 충돌 규칙은 그대로
유지한다. Client는 등록한 manual endpoint와 location store에서 자동 발견한 같은 [ChannelName](../../../../01-glossary.ko.md#channelname)의 server
endpoint를 모두 연결 대상으로 사용할 수 있다.
두 source가 같은 Server RID와 [lifecycle generation](../../../../01-glossary.ko.md#lifecycle-generation)을 가리키면 connection intent와 ready target을 하나로
합친다. Automatic과 manual 모두 Client만 server로 connect하며 Server는 client endpoint를 찾거나 outbound
connect를 시작하지 않는다. Server는 받은 send/request handler와 request reply만 제공하며 연결된 client로
새 업무 호출을 시작하지 않는다.

같은 process에 Server 역할도 등록되어 있으면 listener와 service admission을 마친 local Server를 remote
Server와 같은 candidate 집합에 넣는다. [Ready](../../../../01-glossary.ko.md#ready), positive weight, non-draining 조건을 동일하게 적용하고
local 우선순위나 remote 제외 규칙을 두지 않는다. 선택 뒤에는 Client DEALER에서 Server ROUTER로 실제
transport message를 전달하며 handler를 직접 호출하지 않는다.

`ConfigureNetwork()`의 기본 BindHost는 `127.0.0.1`이고 AdvertiseHost를 생략하면 non-wildcard [BindHost](../../../../01-glossary.ko.md#bindhost)를
사용한다. [Automatic discovery](../../../../01-glossary.ko.md#automatic-discovery) listener는 `Listen()`·`Bind()`·`EnablePublisher()`의 port를 생략하거나
listener 호출 자체를 생략하면 port `0`으로 bind한다. Manual mode에서 endpoint를 다른 discovery source로
얻지 못하면 listen port와 remote endpoint를 명시한다. Listener별 host 설정은 root 기본값보다 우선한다.

[Location store](../../../../01-glossary.ko.md#location-store)를 등록한 fanout publisher는 Framework가 lifecycle별 RID를 만들고 전용 descriptor를 게시한다.
Store가 없는 publisher는 fixed RID와 listener endpoint를 수동으로 전달하는 대상으로 계속 사용할 수 있다.
Endpoint를
받지 않는 `EnableSubscriber()`는 location store에서 같은 ChannelName의 유효한 publisher를 모두 발견한다.
`Connect(endpoint)`는 명시한 endpoint만 사용하는 manual subscriber를 구성한다. 한 fanout
channel에서 automatic subscriber와 manual subscriber를 함께 설정하면 startup이 실패한다. Automatic
subscriber는 location store가 필요하지만 manual publisher와 manual subscriber만 사용하는 host에는
필요하지 않다.
Publisher는 [descriptor](../../../../01-glossary.ko.md#descriptor)만 게시하고 subscriber endpoint로 outbound connect를 시작하지 않는다. Subscriber만
publisher endpoint로 connect하며 automatic subscriber는 Publisher RID와 lifecycle generation마다 connection
intent 하나를 만든다.

Automatic RID는 `prefix-<lowercase-canonical-uuid-v4>` 형식이다. UUID v4는 `8-4-4-4-12` 자리의
lowercase canonical 문자열로 표현한다. Prefix는 ASCII `[A-Za-z0-9._-]` 1..64자이고 full RID는 UTF-8
255 bytes 이하다. Active owner와 충돌하면 새 UUID로 다시 시도하지 않고 즉시 `RoutingIdConflict`로
실패한다. Fixed `SetRoutingId(...)`는 object role과 Store descriptor가 없는 manual topology에서만
허용한다. Slot count, allocation group과 public allocation provider는 제공하지 않는다.

Object Server의 Entry Spot ID에도 같은 prefix를 사용하지만 MeshNode RID와 별도로 생성한 UUID v4를
붙인다. 형식은 `<prefix>-entry-<lowercase-canonical-uuid-v4>`이며 caller가 fixed Entry Spot ID를 지정하지
않는다. 이 ID의 전역 충돌과 caller가 지정한 Spot ID의 예약 형식 검증은
[Spot model](../../../../11-spot-model.ko.md)이 정의한다. Prefix와 생성된 RID·Spot ID를 placement, shard 또는
stable application identity로 해석하지 않는다.

등록한 MeshNode descriptor는 1 MiB 이하여야 한다. [Spot](../../../../01-glossary.ko.md#spot) type과 stateful object capability collection은 각각
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

`DefaultRequestTimeout`의 기본값은 30초, `DefaultSocketSendTimeout`의 기본값은 1초다. `Worker`는 worker의
최소·최대 thread 수, idle timeout과 queue 상한을 host startup 전에 설정한다.

`ConfigureStreamCompression()`과 `IZLinkStreamCompressionBuilder`는 STREAM payload compression을 고른다.
이 builder는 service transport lifecycle이나 relocation codec을 설정하지 않는다.

`ApplicationVersion`은 host 전체에 한 번 설정하며 `0..long.MaxValue` 범위이고 기본값은 `0`이다. 모든 local
MeshNode가 이 값을 게시하며 음수는 startup 전에 `ZLinkConfigurationException`으로 거부한다.
`MaintenanceWave`는 `null`이면 wave exclusion을 사용하지 않는 stable ID다.

`Objects()`를 호출하지 않은 MeshNode의 object role은 `None`이다. `Client()`는 manager와 ID-only message
client를 제공하지만 placement target이 되지 않는다. `Server()`는 Client capability를 포함하며 Entry Spot과
factory를 등록한다. 두 role은 Location Store가 필수다. Role은 한 번만 선택할 수 있다.

`Objects().Client()`를 선택한 MeshNode에도 `Channel(...).Server()`를 등록할 수 있다. 이 조합은
Channel request·send를 처리하기 위한 peer connection이 필요하다. Server weight가 `0`이어도 Server
capability와 연결 필요성은 유지되며 새 Channel operation의 선택 후보에서만 제외된다.
`AddRouteSendHandler(...)`·`AddRouteRequestHandler(...)` 같은 application Node direct handler는 Object
Client에 등록할 수 없고 socket bind 전에 `ZLinkConfigurationException`으로 실패한다.

두 Object Client 사이에는 양쪽 모두 RouteMesh Channel Server membership이 없을 때만 automatic 또는
manual peer connection이 필요하지 않다. Channel Client membership만 등록한 경우도 같다. 어느 한쪽에라도
RouteMesh Channel Server membership이 있으면 연결을 유지한다. ClientServer와 classic fanout은 별도 물리
topology이므로 이 판정에 포함하지 않는다. Object Client와 Object Server, Object Server끼리의 connection은
유지한다.

Actor·User Spot·Instance Spot [factory](../../../../01-glossary.ko.md#factory)는 stable type, object 종류별 factory option과 explicit relocation
policy를 같은 registration에서 고정한다. Policy를 생략하는 overload는 없다. [Stable type](../../../../01-glossary.ko.md#stable-type)은 UTF-8
1..255 bytes이고 중복 type은 startup 오류다. Entry Spot ID는 Framework가 발급한다.

Node placement weight는 0..10000이고 기본값은 100이다. 범위 밖 값은 startup 설정과 runtime 변경에서
`ZLinkConfigurationException`이다. Actor·Spot population limit의 기본값 `0`은 제한 없음이며,
pending activation concurrency 기본값은 128이다.
Type별 limit은 `null`이면 node limit을 공유하고 값이 있으면 1..`int.MaxValue`이며 node limit보다 작은 값을
적용한다. Capacity를 weight보다 먼저 적용하고 eligible node가 없으면 `CapacityExceeded`다.

`SetInstanceSpotIdleTimeout(...)`은 유휴 Instance Spot 정리 기준 시간이다. 기본값은 `TimeSpan.Zero`이고
`TimeSpan.Zero`는 정리하지 않음을 뜻한다. 허용 범위는 `TimeSpan.Zero`와 양수이며 음수는 startup 전에
`ZLinkConfigurationException`이다. 값은 MeshNode lifecycle
시작 전에 고정하고 실행 중 setter를 제공하지 않는다. `ZLinkWorkerOptions.IdleTimeout`과는 별개의 설정이며
서로 값을 상속하지 않는다. 정리 대상은 Instance Spot뿐이고 Entry Spot과 User Spot은 이 설정의 영향을
받지 않는다. 유휴 판정 조건, `ZLinkSpotCloseReason.IdleEvicted` 전달과 정리 뒤 cold activation 규칙은
[Spot 모델 §6.2](../../../../11-spot-model.ko.md#62-유휴-instance-spot-정리)가 소유한다.

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

`Connect(...)`로 지정한 양쪽 MeshNode가 Object Client이고 양쪽 모두 RouteMesh Channel Server
membership이 없으면 configuration intent는 목록에 남을 수 있지만 ready peer가 되지 않는다. Handshake가
`NotRequired`로 끝난 뒤 같은 configuration generation에는 다시 연결하지 않으며 public RouteMesh status의
ready peer 수와 liveness 대상에 포함하지 않는다. 어느 한쪽에라도 weight `0`을 포함한 Channel Server
membership이 있으면 일반 peer admission과 liveness 규칙을 적용한다.

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

`ChannelSend`와 `ChannelRequest`는 RouteMesh와 ClientServer를 모두 포함한다. RouteMesh와 Node direct
context는 MeshName을 제공하고 ClientServer와 `ClassicFanout`은 `null`을 제공한다. Filter는 `next`를
최대 한 번 호출한다. 두 번째 호출은 `ZLinkFrameworkErrorKind.InvalidOperation`으로 실패하며 handler를
다시 실행하지 않는다. Request에서 `next`를 호출하지 않으면
`ZLinkFrameworkErrorKind.Rejected` reply를 보낸다. Filter가 업무 reply를 대체하는 동작은 호환
overload나 adapter로 제공하지 않는다.

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
받는다. Factory 대상과 adapter 종류가 맞지 않아도 같은 오류로 실패한다.

Framework는 등록 호출 안에서 callback을 동기적으로 한 번 실행한다. Callback이 반환되면 builder 구성을
고정한다. Application이 callback 밖에 builder를 보관했다가 다시 호출하면 configuration error다.
Callback이 예외를 던지면 factory를 등록하지 않고 같은 예외를 호출자에게 전달한다.

`ZLinkUserSpotExecutionMode.PerActor`를 선택한 User Spot은
`RecreateOnRelocation()`만 허용한다. `DisableRelocation()`이나 `PreserveStateWith<TAdapter>()`를
함께 등록하면 socket bind 전에 startup configuration error다. PerActor Spot은
stateless execution shell이며 member Actor의 relocation policy와 adapter가 Actor
state를 각각 처리한다. 유지해야 하는 shared state와 Spot-level schedule은
application의 Redis·database·service 같은 외부 저장소에 둔다.

Execution mode의 기본값은 `SpotWide`, relocation readiness의 기본값은 `AnyTurnBoundary`다.
`ApplicationSignaled`는 `SpotWide`에서만 허용한다. `PerActor`와 함께 등록하면
socket bind 전에 startup configuration error다. Callback은 `IZLinkSpot`의 기본
no-op 구현을 사용하므로 application override는 필수가 아니다.

expected RID를 생략하면 admission handshake가 remote identity를 결정한다. expected RID를 지정한 경우
handshake identity가 다르면 연결을 admission하지 않는다. Manual 연결도 자동 discovery 연결과 같은
[MeshName](../../../../01-glossary.ko.md#meshname)·RID·ChannelName·security 검증을 사용한다.

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
send/request handler를 해당 ChannelName에 노출한다. TicTacToe처럼 수동 등록을 보여 주는 경우에만
typed `AddSendHandler(...)`·`AddRequestHandler(...)`를 직접 사용한다.

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
    long MaxMessageSize { get; set; }
    ulong SendHighWaterMark { get; set; }
    ulong ReceiveHighWaterMark { get; set; }
    ulong MailboxMessageBudget { get; set; }
    ulong MailboxByteBudget { get; set; }
    TimeSpan? ReceiveTimeout { get; set; }
    TimeSpan? SendTimeout { get; set; }
}
```

Application listener의 `MaxMessageSize` 기본값은 `16 MiB`다. Application HWM을 Auto 또는 양수로
사용할 때 `0`을 명시하면 startup configuration error다. `ApplicationHwmBytes = 0`일 때만
`MaxMessageSize = 0`을 사용할 수 있다.

`ConfigureSpotPublisher()`는 publish 전용 전달 정책 option을 제공하지 않는다. [Logical Multicast](../../../../01-glossary.ko.md#logical-multicast)는
source-local 실행 용량을 send timeout 안에 확보하면 시작하고 결과값 없이 정상 완료한다. Target별
수락·실패 결과를 기다리거나 public monitoring에 집계하지 않으며 일부 target 실패 때문에 전체 publish를
자동 재시도하지 않는다. Target이 없어도 정상 완료한다.

`IZLinkRouteMeshRuntimeOptions`는 public DI singleton이다. 등록되지 않은 membership을 조회하면
`ZLinkConfigurationException`이다. `MailboxMessageBudget`와 `MailboxByteBudget`은 [owner](../../../../01-glossary.ko.md#owner)별 application
mailbox의 메시지 수와 byte 합계 상한이다. Byte 회계는 payload 크기만 세지 않는다 —
`payload 크기 + metadata 크기 + 작업당 고정 비용`을 더한다. Payload가 비어 있어도 작업 하나는 0 byte가
아니며, 큰 payload에서도 고정 비용은 그대로 더한다. 합이 `ulong` 표현 범위를 넘으면 `ulong.MaxValue`로
고정하고 그 제출을 거절한다. 회계 규칙은
[Framework API §8.2](../../../../06-framework-api.ko.md#82-handler-실행-객체와-dependency-수명)가 소유한다.
0은 Framework profile의 유한 기본값을 사용한다. 두 값은
`ConfigureRouterSocket()`에서 startup 전에 설정하며, Logical Multicast의 local target drop도 이 공개
용량 설정을 따른다.

실행 중에는 `Mesh(meshName).PlacementWeight`와 `Channel(channelName).Weight`를 변경할 수 있다.
두 weight는 서로 독립적이며 node weight는 object create·relocation target selection에만 사용한다.
ChannelName은 local RouteMesh 또는 ClientServer Server 등록을 유일하게 고른다. HWM과 timeout은
`ConfigureRouterSocket()`에서 startup 전에 설정한다.

`MaxMessageSize`는 startup 전에만 설정하며 실행 중 setter를 제공하지 않는다. `0`은 Framework가 지원하는
최대 complete message 크기를 사용한다. 양수는 public protocol의 표현 한계를 넘을 수 없으며 넘으면
`ZLinkConfigurationException`으로 startup을 실패시킨다. Peer마다 두 endpoint가 허용한 값 중 작은 값을
적용한다. Framework application listener의 기본값은 `16_777_216` bytes다.

`ConfigureInboundDispatch()`는 host 전체 inbound 설정 하나를 반환한다. `ApplicationHwmBytes`의 기본값은
`null`이며 Auto mode를 뜻한다. `0`은 제한 없음, 양수는 정확한 byte 상한이다.
`ApplicationHwmProfile` 기본값은 `Balanced`이고 Auto mode에서만
계산에 사용한다. `ProcessMemoryLimitBytes`는 `null` 또는 양수만 허용한다. Auto mode에서 이 값을
생략하면 process에 적용된 유한한 container·cgroup·Windows Job Object와 같은 OS 상한과 .NET GC managed
heap 상한(`GC.GetGCMemoryInfo().TotalAvailableMemoryBytes`)을 확인한다. 두 값을 모두 확인하면 더 작은
값을 사용하고, 하나만 확인하면 그 값을 사용한다. 둘 다 확인할 수 없으면 시스템 물리 메모리 총량을 사용한다.
따라서 Auto mode는 설정 없이도 기동한다.

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
