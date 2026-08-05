# Java 구성과 host 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Transport liveness](../../../../29-transport-liveness.ko.md)

이 문서는 Java application이 Mesh, Channel role, handler와 Framework host를 구성할 때 사용하는 공개
인터페이스를 고정한다. Application은 아래 builder로 구성을 선언하고, Framework는 host를 시작할 때
그 구성이 계약에 맞는지 검사한다.

```java
public interface ZLinkFrameworkOptions {
    Duration defaultRequestTimeout();
    void setDefaultRequestTimeout(Duration timeout);
    ZLinkCodecRegistryBuilder codecs();
    void addHandlersFromPackageOf(Class<?> markerType);
    ZLinkMetadataPolicyBuilder configureMetadata();
    void addLocationStore(ZLinkLocationStore store);
    void addRelocationStore(ZLinkRelocationStore store);
    void setApplicationVersion(long version);
    void setMaintenanceWave(String waveId);
    ZLinkLocationOptions configureLocations();
    ZLinkInboundDispatchOptions configureInboundDispatch();
    ZLinkNetworkOptions configureNetwork();
    ZLinkMeshNodeBuilder addRouteMesh(String meshName);
    ClientServerChannelBuilder addClientServerChannel(String channelName);
    FanoutChannelBuilder addFanoutChannel(String channelName);
    ZLinkStreamNodeBuilder addStreamNode(String name);
    void useFilter(Class<? extends ZLinkHandlerFilter> filterType);
    ZLinkDispatchOptions configureDispatch();
    ZLinkStreamCompressionBuilder configureStreamCompression();
    ZLinkWorkerOptions configureWorkers();
    void useVirtualThreadHandlers();
    void useHandlerExecutor(Executor executor);
}

public enum ZLinkApplicationHwmProfile {
    COMPACT,
    LOW_LATENCY,
    BALANCED,
    THROUGHPUT
}

public interface ZLinkInboundDispatchOptions {
    java.util.OptionalLong applicationHwmBytes();
    void setApplicationHwmBytes(long value);
    ZLinkApplicationHwmProfile applicationHwmProfile();
    void setApplicationHwmProfile(ZLinkApplicationHwmProfile value);
    java.util.OptionalLong processMemoryLimitBytes();
    void setProcessMemoryLimitBytes(long value);
}

public interface ZLinkLocationOptions {
    Duration ownerLeaseRenewInterval();
    void setOwnerLeaseRenewInterval(Duration value);
    Duration ownerLeaseTtl();
    void setOwnerLeaseTtl(Duration value);
    Duration pollingInterval();
    void setPollingInterval(Duration value);
    Duration storeFailureGrace();
    void setStoreFailureGrace(Duration value);
    Duration ownerLeaseFencingMargin();
    void setOwnerLeaseFencingMargin(Duration value);
    Duration ownerLeaseRenewTimeout();
    void setOwnerLeaseRenewTimeout(Duration value);
    Duration routeCacheMaxAge();
    void setRouteCacheMaxAge(Duration value);
    Duration messageFollowDuration();
    void setMessageFollowDuration(Duration value);
    int maxActiveOutboundRelocations();
    void setMaxActiveOutboundRelocations(int value);
    int maxActiveInboundRelocations();
    void setMaxActiveInboundRelocations(int value);
    int maxConcurrentRelocationCaptures();
    void setMaxConcurrentRelocationCaptures(int value);
    int maxConcurrentRelocationRestores();
    void setMaxConcurrentRelocationRestores(int value);
    long maxRelocationPayloadInFlightBytes();
    void setMaxRelocationPayloadInFlightBytes(long value);
}

public interface ZLinkNetworkOptions {
    String bindHost();
    void setBindHost(String host);
    Optional<String> advertiseHost();
    void setAdvertiseHost(String host);
}

public interface ZLinkMeshNodeSocketConfig {
    long maxMessageSize();
    void setMaxMessageSize(long value);
    long sendHighWaterMark();
    void setSendHighWaterMark(long value);
    long receiveHighWaterMark();
    void setReceiveHighWaterMark(long value);
    long mailboxMessageBudget();
    void setMailboxMessageBudget(long value);
    long mailboxByteBudget();
    void setMailboxByteBudget(long value);
    Optional<Duration> receiveTimeout();
    void setReceiveTimeout(Duration value);
    Optional<Duration> sendTimeout();
    void setSendTimeout(Duration value);
}

@FunctionalInterface
public interface ZLinkFrameworkConfigurer {
    void configure(ZLinkFrameworkOptions framework);
}

public interface ZLinkMeshNodeBuilder {
    ZLinkMeshChannelBuilder channel(String channelName);
    ZLinkMeshNodeBuilder listen(String endpoint);
    ZLinkMeshNodeBuilder listen();
    ZLinkMeshNodeBuilder listen(int port);
    ZLinkMeshNodeBuilder setBindHost(String host);
    ZLinkMeshNodeBuilder setAdvertiseHost(String host);
    ZLinkMeshNodeBuilder setRoutingId(RoutingId routingId);
    ZLinkMeshNodeBuilder setRoutingIdPrefix(String prefix);
    ZLinkMeshNodeBuilder setPlacementWeight(int weight);
    ZLinkMeshNodeBuilder setActorCapacity(int maxActors);
    ZLinkMeshNodeBuilder setSpotCapacity(int maxSpots);
    ZLinkMeshNodeBuilder setActivationConcurrency(int maxConcurrentActivations);
    ZLinkMeshNodeBuilder setInstanceSpotIdleTimeout(Duration timeout);
    ZLinkMeshObjectRoleBuilder objects();
    ZLinkMeshNodeSocketConfig configureRouterSocket();
    ZLinkSpotPublisherConfig configureSpotPublisher();
    ZLinkMeshPeerConnections peerConnections();
    ZLinkMeshNodeBuilder setDefaultRequestTimeout(Duration timeout);

    <THandler, TMessage>
    ZLinkMeshNodeBuilder addRouteSendHandler(
        Class<THandler> handlerType,
        Class<TMessage> messageType);
    <THandler, TRequest, TReply>
    ZLinkMeshNodeBuilder addRouteRequestHandler(
        Class<THandler> handlerType,
        Class<TRequest> requestType,
        Class<TReply> replyType);

}

public interface ZLinkMeshObjectRoleBuilder {
    ZLinkMeshObjectClientBuilder client();
    ZLinkMeshObjectServerBuilder server();
}

public interface ZLinkMeshObjectClientBuilder {}

public interface ZLinkMeshObjectServerBuilder {
    ZLinkMeshObjectServerBuilder addEntrySpot(Class<? extends ZLinkEntrySpot> entrySpotClass);
    <TSpot extends ZLinkSpot> ZLinkMeshObjectServerBuilder addSpotFactory(
        String spotType, Class<TSpot> spotClass,
        Consumer<ZLinkUserSpotFactoryBuilder<TSpot>> configure);
    <TSpot extends ZLinkInstanceSpot> ZLinkMeshObjectServerBuilder addInstanceSpotFactory(
        String instanceSpotType, Class<TSpot> spotClass,
        Consumer<ZLinkInstanceSpotFactoryBuilder<TSpot>> configure);
    <TActor extends ZLinkActor> ZLinkMeshObjectServerBuilder addActorFactory(
        String actorType,
        Class<TActor> actorClass,
        Class<? extends ZLinkActorFactory> factoryClass,
        Consumer<ZLinkActorFactoryBuilder<TActor>> configure);
}

public enum ZLinkUserSpotExecutionMode {
    SPOT_WIDE(0), PER_ACTOR(1);
    private final int value;
    ZLinkUserSpotExecutionMode(int value) { this.value = value; }
    public int value() { return value; }
}

public enum ZLinkSpotRelocationReadinessMode {
    ANY_TURN_BOUNDARY(0), APPLICATION_SIGNALED(1);
    private final int value;
    ZLinkSpotRelocationReadinessMode(int value) { this.value = value; }
    public int value() { return value; }
}

public interface ZLinkActorFactoryBuilder<TActor extends ZLinkActor> {
    void disableRelocation();
    void recreateOnRelocation();
    void preserveStateWith(
        Class<? extends ZLinkActorRelocationAdapter<TActor>> adapterClass);
}

public interface ZLinkUserSpotFactoryBuilder<TSpot extends ZLinkSpot> {
    ZLinkUserSpotFactoryBuilder<TSpot> stableTypeLimit(int limit);
    ZLinkUserSpotFactoryBuilder<TSpot> executionMode(ZLinkUserSpotExecutionMode mode);
    ZLinkUserSpotFactoryBuilder<TSpot> relocationReadiness(
        ZLinkSpotRelocationReadinessMode mode);
    void disableRelocation();
    void recreateOnRelocation();
    void preserveStateWith(
        Class<? extends ZLinkSpotRelocationAdapter<TSpot>> adapterClass);
}

public interface ZLinkInstanceSpotFactoryBuilder<TSpot extends ZLinkInstanceSpot> {
    ZLinkInstanceSpotFactoryBuilder<TSpot> stableTypeLimit(int limit);
    void disableRelocation();
    void recreateOnRelocation();
    void preserveStateWith(
        Class<? extends ZLinkSpotRelocationAdapter<TSpot>> adapterClass);
}

public interface FanoutChannelBuilder {
    FanoutChannelBuilder enablePublisher(String endpoint);
    FanoutChannelBuilder enablePublisher();
    FanoutChannelBuilder enablePublisher(int port);
    FanoutChannelBuilder setBindHost(String host);
    FanoutChannelBuilder setAdvertiseHost(String host);
    FanoutChannelBuilder setRoutingId(RoutingId publisherRoutingId);
    FanoutChannelBuilder setRoutingIdPrefix(String prefix);
    FanoutChannelBuilder enableSubscriber();
    FanoutChannelBuilder connect(String endpoint);
    ZLinkEndpointConnections subscriberConnections();
    FanoutChannelBuilder addHandlerGroup(String groupName);
}
```

`applicationHwmBytes()`가 비어 있으면 Auto mode다. Setter의 `0`은 제한 없음, 양수는 정확한 host 전체
byte 상한이며 음수는 startup configuration error다. Profile 기본값은 `BALANCED`다.
`processMemoryLimitBytes()`가 비어 있으면 process에 적용된 유한한 container·cgroup·Windows Job Object와
같은 OS 상한과 JVM managed heap 상한(`Runtime.maxMemory()`)을 확인한다. 두 값을 모두 확인하면 더 작은
값을 사용하고, 하나만 확인하면 그 값을 사용한다. 둘 다 확인할 수 없으면 시스템 물리 메모리 총량을 사용한다.
Auto mode는 설정 없이도 기동하며, 계산 결과가 양수가 아닐 때만 socket bind 전에 startup이 실패한다.
Application listener의 `maxMessageSize()` 기본값은 `16_777_216L` bytes다.

`configureNetwork()`은 process의 RouteMesh, ClientServer, classic fanout과 stream listener가 사용하는
기본 host를 반환한다. 기본 BindHost는 `127.0.0.1`이다. Listener별 `setBindHost(...)`와
`setAdvertiseHost(...)`를 호출하면 해당 listener에서만 root 기본값을 덮어쓴다. Port `0`을 사용한 경우
Framework는 bind 뒤 확정된 port와 AdvertiseHost를 결합해 discovery descriptor에 기록한다. Stream
listener는 discovery 대상이 아니므로 같은 규칙으로 계산한 advertised endpoint를 운영 정보에 사용하며,
remote connector endpoint를 자동 게시하지 않는다.

다음 예제는 서로 다른 host 구성에서 같은 RouteMesh에 호출만 시작하는 node와 요청을 처리하는 node를
각각 등록하는 최소 형태다. `clientOptions`와 `serverOptions`는 각각 별도 host의
`ZLinkFrameworkOptions`다. 예제의 이름과 weight는 설명을 위한 값이며 계약 기본값을 뜻하지 않는다.

```java
clientOptions.addRouteMesh("orders")
    .channel("checkout")
    .client(); // 이 node는 checkout 호출을 시작하지만 server 후보에는 포함되지 않는다.

serverOptions.addRouteMesh("orders")
    .channel("checkout")
    .server()
    .setWeight(100)
    .addRequestHandler(
        CheckoutHandler.class,
        CheckoutRequest.class,
        CheckoutReply.class); // 이 node를 checkout 요청 처리 후보로 등록한다.
```

Automatic [RouteMesh](../../../../01-glossary.ko.md#routemesh)는 RID를 canonical byte order로 비교하고 더 작은 RID의 MeshNode만 상대 endpoint로
connect한다. Manual topology는 application endpoint 구성에 따라 한쪽 또는 양쪽에서 connect할 수 있다.
양쪽 연결이나 automatic discovery 경합·오래된 snapshot으로 중복 후보가 생기면 handshake와 admission이
같은 RID와 lifecycle generation을 확인해 하나만 ready 상태로 유지한다.

두 MeshNode의 object role이 모두 `Client`이고 양쪽 모두 RouteMesh Channel Server membership이 없을
때만 peer connection이 필요하지 않다. Channel Client membership만 등록한 경우도 같다. 어느 한쪽에라도
Channel Server membership이 있으면 weight가 `0`이어도 connection을 만들고 liveness를 유지한다.
ClientServer와 classic fanout registration은 별도 물리 topology이므로 이 판정에 포함하지 않는다.

ClientServer는 manual endpoint와 location store [automatic discovery](../../../../01-glossary.ko.md#automatic-discovery)를 함께 사용할 수 있다. 두 source가 같은
Server RID와 [lifecycle generation](../../../../01-glossary.ko.md#lifecycle-generation)을 가리키면 connection intent와 ready target을 하나로 합친다. Automatic과
manual 모두 Client만 server로 connect하며 Server는 client endpoint를 찾거나 outbound connect를 시작하지
않는다. 같은 ChannelName에는 Client와 Server를 각각 한 번 등록할 수 있고 `(ChannelName, Role)` key의
별도 registration으로 하나의 ClientServer topology를 공유한다. 같은 역할을 두 번 등록하면 startup이
실패하며 RouteMesh [ChannelName](../../../../01-glossary.ko.md#channelname) 충돌 규칙은
유지한다. Local Server도 listener와 service admission 뒤 remote Server와 같은 readiness·[weight](../../../../01-glossary.ko.md#weight)·drain
조건으로 선택하며 local 우선순위나 direct handler 호출을 사용하지 않는다.

Fanout에서는 Publisher가 descriptor만 게시하고 outbound connect를 시작하지 않는다. Subscriber만 publisher
endpoint로 connect하며 automatic subscriber는 Publisher RID와 lifecycle generation마다 connection intent
하나를 만든다. 한 ChannelName에 automatic subscriber와 manual subscriber endpoint를 함께 구성하면 startup이
실패한다.

Object role을 생략하면 `None`이다. `client()`는 global object operation만 제공하고 placement target이 되지
않으며 `server()`는 Client capability와 Entry Spot·factory registration을 제공한다. Client와 Server는
[Location Store](../../../../01-glossary.ko.md#location-store)가 필수다. Actor·User Spot·Instance Spot [factory](../../../../01-glossary.ko.md#factory)는 stable type과 configure callback을
반드시 받으며 relocation 동작 선택을 생략하는 overload는 없다.

Object Client에도 RouteMesh Channel Server를 등록할 수 있다. Application Node direct handler는 등록할
수 없으며 Object Client RID를 Node direct target으로 지정하면 다른 RID로 바꾸지 않고 not-found로 끝낸다.

Configure callback은 `disableRelocation()`, `recreateOnRelocation()`, `preserveStateWith(...)` 중 정확히 하나를
호출해야 한다. 누락하거나 둘 이상 호출하면 socket bind 전에 startup configuration error다. Actor builder는
같은 Actor type의 `ZLinkActorRelocationAdapter`, User·Instance Spot builder는 같은 Spot type의
`ZLinkSpotRelocationAdapter`만 받는다.

Framework는 configure callback을 등록 호출 안에서 동기적으로 한 번만 실행한다. Callback이 반환된 뒤
보관한 builder를 다시 호출하면 configuration error다. Callback이 예외를 던지면 해당 factory를 등록하지
않고 같은 예외를 호출자에게 전달한다.

`ZLinkUserSpotExecutionMode.PER_ACTOR`는 `recreateOnRelocation()`만
허용한다. 다른 policy를 함께 등록하면 startup configuration
error다. PerActor Spot은 stateless execution shell이며 Actor policy와 adapter가
Actor state를 각각 처리한다.

`relocationReadiness`의 기본값은 `ANY_TURN_BOUNDARY`다.
`APPLICATION_SIGNALED`는 `SPOT_WIDE`에서만 허용하며 `PER_ACTOR`와 함께 등록하면
socket bind 전에 startup configuration error다. Spot callback은 default no-op
method이므로 application override는 필수가 아니다.

Node placement weight는 0..10000이고 기본값은 100이다. 범위 밖 값은 startup 설정과 runtime 변경에서
configuration error다. Node capacity 기본값은 active 10,000, pending 128이다.
Type별 limit을 생략하면 node limit을 공유한다. 명시한 값은 1..`Integer.MAX_VALUE`여야 하며 node limit보다
작은 값을 적용한다. `stableTypeLimit(0)`과 음수는 callback 실행 중 configuration error다. Capacity filter를
weight보다 먼저 적용한다.
`enableActorDispatch()`는 인자가 없으며 global ActorId가 Mesh를 resolve한다.

Object Server의 Entry Spot ID는 MeshNode diagnostic prefix를 사용한
`<prefix>-entry-<lowercase-canonical-uuid-v4>` 형식이며 MeshNode와 별도로 생성한 UUID v4를 사용한다.
Framework 내부 descriptor의 `entrySpotId`가 같은 lifecycle의 exact mapping을 제공한다. Global Spot ID가
active owner와 충돌하면 새 UUID로 다시 시도하지 않고 즉시 configuration exception으로 startup을
실패시킨다. Caller가 지정한 User·Instance Spot ID가 이 예약 형식과 일치하면 Store와 factory를 시작하기
전에 `INVALID_OPERATION`으로 거부한다.

Location provider는 `ZLinkLocationStore`를 통해 Framework의 opaque record read, version 조건부 atomic
batch와 bounded snapshot scan을 제공한다. 별도 domain별 Store instance를 host에 등록하지 않는다.
`recreateOnRelocation()` 또는 `preserveStateWith(...)`를
하나라도 선택했거나 Instance Spot factory를 하나라도 등록한 host는 `ZLinkRelocationStore`를 정확히 하나 등록한다.
Instance Spot factory가 없고 모든 factory가 `disableRelocation()`을 선택하며 same-node join만 사용하는 host는 Relocation Store가 없어도 된다.
Missing 또는 duplicate Store registration은 socket bind 전에 startup
configuration error다. Location과 Relocation capability를 함께 등록하는 API와 Redis 전용 registration helper는
제공하지 않는다.

`ApplicationVersion`은 `0..Long.MAX_VALUE` 범위의 배포 순번이다. 음수는 startup validation에서 거부한다.
Application traffic과 무관한 5초 periodic probe와 같은 current connection의 matching ACK 15초 deadline은
JVM service runtime의 고정 liveness profile이다. 다른 inbound frame은 [deadline](../../../../01-glossary.ko.md#deadline)을 충족하지 않는다. 이 값을
Channel·handler·peer별 public option으로 노출하지 않는다. Location owner lease option은 별도 store 계약이며
transport liveness를 대신하지 않는다.

`maxMessageSize`는 startup 전에만 설정하며 runtime setter를 제공하지 않는다. `0`은 bindings 또는 transport가
수신할 수 있는 최대 complete message 크기로 정규화한다. Transport가 unlimited이면 service wire의 `uint32`
표현 한계에서 envelope overhead를 뺀 값을 사용한다. 양수는 그 표현 한계를 넘을 수 없으며 넘으면 startup
설정 오류로 거부한다. Peer는 정규화한 값을 내부 handshake로 교환하고 sender와 receiver는 두 값 중 작은
effective bound를 complete message allocation 전에 적용한다. 이 negotiation을 위한 public option은 제공하지
않는다.

`mailboxMessageBudget`와 `mailboxByteBudget`은 owner별 application mailbox의 메시지 수와 byte 합계
상한이다. Byte 회계는 payload 크기만 세지 않는다 — `payload 크기 + metadata 크기 + 작업당 고정 비용`을
더한다. Payload가 비어 있어도 작업 하나는 `0` byte가 아니며, 큰 payload에서도 고정 비용은 그대로
더한다. 합이 `long` 표현 범위를 넘으면 `Long.MAX_VALUE`로 고정하고 그 제출을 거절한다. 회계 규칙은
[Framework API §8.2](../../../../06-framework-api.ko.md#82-handler-실행-객체와-dependency-수명)가 소유한다.
두 값은 startup 전에만 설정한다. `0`은 unlimited가 아니라 Framework profile의 유한 기본값을
선택한다. 음수는 startup 설정 오류다. Logical Multicast의 local target도 이 용량 제한으로 admission을
판단한다.

`setInstanceSpotIdleTimeout(...)`은 유휴 Instance Spot 정리 기준 시간이다. 기본값은 `Duration.ZERO`이고
`Duration.ZERO`는 정리하지 않음을 뜻한다. 허용 범위는 `Duration.ZERO`와 양수이며 `null`과 음수는
startup 설정 오류다. 값은 MeshNode lifecycle 시작 전에 고정하고 runtime setter를 제공하지 않는다.
`ZLinkWorkerOptions.idleTimeout(...)`과는 별개의 설정이며 서로 값을 상속하지 않는다. 정리 대상은
Instance Spot뿐이고 Entry Spot과 User Spot은 이 설정의 영향을 받지 않는다. 유휴 판정 조건,
`ZLinkSpotCloseReason.IDLE_EVICTED` 전달과 정리 뒤 cold activation 규칙은
[Spot 모델 §6.2](../../../../11-spot-model.ko.md#62-유휴-instance-spot-정리)가 소유한다.

Automatic RID는 `prefix-<lowercase-canonical-uuid-v4>` 형식이다. UUID v4는 `8-4-4-4-12` 자리의
lowercase canonical 문자열로 표현한다. Prefix는 ASCII `[A-Za-z0-9._-]` 1..64자이며 active
[owner](../../../../01-glossary.ko.md#owner)와 충돌하면 새 UUID로 다시 시도하지 않고 즉시
`ROUTING_ID_CONFLICT`로 실패한다. Fixed RID는 object role과 Store descriptor가 없는 manual
topology에서만 허용한다. Slot count, allocation group과 public allocation provider는 제공하지 않는다.

Framework가 모든 registration에서 만든 fully encoded MeshNode descriptor는 1 MiB 이하여야 한다.
[Spot](../../../../01-glossary.ko.md#spot) type과 object capability collection은 각각 최대 1024개다. Relocation adapter class와 opaque application
bytes는 peer descriptor에 게시하지 않는다. Runtime은 완성된 descriptor를 socket bind 전에 한 번에 검증한다.
Bound를 넘으면
startup을 실패시키며 collection을 truncate·split하거나 descriptor 일부를 게시하지 않는다.

## Exact public member `javap` inventory

아래 선언은 `javap`가 출력하는 binary signature 형식으로 이 category의 Java public type과 member를 고정한다.

```java
public interface systems.zlink.framework.spring.ZLinkFrameworkConfigurer {
  public abstract void configure(systems.zlink.framework.configuration.ZLinkFrameworkOptions);
}
public interface systems.zlink.framework.configuration.ZLinkNetworkOptions {
  public abstract java.lang.String bindHost();
  public abstract void setBindHost(java.lang.String);
  public abstract java.util.Optional<java.lang.String> advertiseHost();
  public abstract void setAdvertiseHost(java.lang.String);
}
public interface systems.zlink.framework.configuration.FanoutChannelBuilder {
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder enablePublisher(java.lang.String);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder enablePublisher();
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder enablePublisher(int);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder setBindHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder setAdvertiseHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder setRoutingId(systems.zlink.contracts.core.RoutingId);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder setRoutingIdPrefix(java.lang.String);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder enableSubscriber();
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder connect(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkEndpointConnections subscriberConnections();
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder addHandlerGroup(java.lang.String);
  public abstract void addPublishHandler(java.lang.Class<?>, java.lang.Class<?>);
  public abstract void addPublishHandler(java.lang.Class<?>, java.lang.Class<?>, java.lang.String);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder addPublishHandler(java.lang.Class<?>);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder addPublishHandler(java.lang.Class<?>, java.lang.String);
}
public interface systems.zlink.framework.configuration.ZLinkCodecExtension {
  public abstract void register(systems.zlink.framework.configuration.ZLinkCodecRegistrar);
}
public interface systems.zlink.framework.configuration.ZLinkCodecRegistrar {
  public abstract void addSerializer(java.lang.String, systems.zlink.framework.ZLinkMessageSerializer);
  public abstract void addSerializer(java.lang.String, systems.zlink.framework.ZLinkMessageSerializer, java.util.function.Predicate<java.lang.Class<?>>);
  public abstract void addStreamCodec(java.lang.String, systems.zlink.framework.streams.ZLinkStreamCodec);
}
public interface systems.zlink.framework.configuration.ZLinkCodecRegistryBuilder {
  public abstract void use(systems.zlink.framework.configuration.ZLinkCodecExtension);
}
public interface systems.zlink.framework.configuration.ZLinkDiagnosticsOptions {
  public abstract systems.zlink.framework.configuration.ZLinkMessageFlowLogMode messageFlow();
  public abstract double sampleRate();
  public abstract boolean includeMessageSizes();
  public abstract boolean includeNativeDiagnostics();
  public abstract java.lang.String logFile();
  public abstract java.lang.String label();
  public abstract systems.zlink.framework.configuration.ZLinkMessageFlowLogMode effectiveMessageFlow();
}
public interface systems.zlink.framework.configuration.ZLinkDispatchOptions {
  public abstract systems.zlink.framework.configuration.ZLinkUnhandledDispatchOptions unhandled();
  public abstract systems.zlink.framework.configuration.ZLinkDiagnosticsOptions diagnostics();
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions setMessageFlowObserver(java.lang.Class<? extends systems.zlink.framework.configuration.ZLinkMessageFlowObserver>);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions setMessageFlowObserver(systems.zlink.framework.configuration.ZLinkMessageFlowObserver);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions traceSampleRate(double);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions includeMessageSizes(boolean);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions traceLogFile(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions traceLabel(java.lang.String);
}
public interface systems.zlink.framework.configuration.ZLinkEndpointConnections {
  public abstract void connect(java.lang.String);
  public abstract void disconnect(java.lang.String);
  public abstract java.util.List<java.lang.String> listConnections();
}
public interface systems.zlink.framework.configuration.ZLinkMeshNodeSocketConfig {
  public abstract long maxMessageSize();
  public abstract void setMaxMessageSize(long);
  public abstract long sendHighWaterMark();
  public abstract void setSendHighWaterMark(long);
  public abstract long receiveHighWaterMark();
  public abstract void setReceiveHighWaterMark(long);
  public abstract long mailboxMessageBudget();
  public abstract void setMailboxMessageBudget(long);
  public abstract long mailboxByteBudget();
  public abstract void setMailboxByteBudget(long);
  public abstract java.util.Optional<java.time.Duration> receiveTimeout();
  public abstract void setReceiveTimeout(java.time.Duration);
  public abstract java.util.Optional<java.time.Duration> sendTimeout();
  public abstract void setSendTimeout(java.time.Duration);
}
public final class systems.zlink.framework.configuration.ZLinkMeshPeerConnection extends java.lang.Record {
  public systems.zlink.framework.configuration.ZLinkMeshPeerConnection(java.lang.String, java.util.Optional<systems.zlink.contracts.core.RoutingId>);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public java.lang.String endpoint();
  public java.util.Optional<systems.zlink.contracts.core.RoutingId> expectedRoutingId();
}
public interface systems.zlink.framework.configuration.ZLinkMeshPeerConnections {
  public abstract void connect(java.lang.String);
  public abstract void connect(systems.zlink.contracts.core.RoutingId, java.lang.String);
  public abstract void disconnect(java.lang.String);
  public abstract java.util.List<systems.zlink.framework.configuration.ZLinkMeshPeerConnection> listConnections();
}
public interface systems.zlink.framework.configuration.ZLinkMessageFlowControl {
  public abstract void setMessageFlowMode(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode);
  public abstract systems.zlink.framework.configuration.ZLinkMessageFlowLogMode messageFlowMode();
}
public final class systems.zlink.framework.configuration.ZLinkMessageFlowEvent extends java.lang.Record {
  public systems.zlink.framework.configuration.ZLinkMessageFlowEvent(systems.zlink.framework.configuration.ZLinkMessageFlowOutcome, systems.zlink.framework.configuration.ZLinkDispatchErrorSurface, systems.zlink.framework.configuration.ZLinkDispatchMessageKind, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.Long, systems.zlink.framework.configuration.ZLinkDispatchErrorReason, systems.zlink.framework.configuration.ZLinkDispatchErrorAction, java.lang.String, java.lang.String, java.lang.String, systems.zlink.framework.monitoring.ZLinkFlowOrigin);
  public systems.zlink.framework.configuration.ZLinkMessageFlowEvent(systems.zlink.framework.configuration.ZLinkMessageFlowOutcome, systems.zlink.framework.configuration.ZLinkDispatchErrorSurface, systems.zlink.framework.configuration.ZLinkDispatchMessageKind, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.Long, systems.zlink.framework.configuration.ZLinkDispatchErrorReason, systems.zlink.framework.configuration.ZLinkDispatchErrorAction, java.lang.String, java.lang.String);
  public systems.zlink.framework.configuration.ZLinkMessageFlowEvent(systems.zlink.framework.configuration.ZLinkMessageFlowOutcome, systems.zlink.framework.configuration.ZLinkDispatchErrorSurface, systems.zlink.framework.configuration.ZLinkDispatchMessageKind, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.Long);
  public systems.zlink.framework.configuration.ZLinkMessageFlowEvent withFlow(java.lang.String, systems.zlink.framework.monitoring.ZLinkFlowOrigin);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public systems.zlink.framework.configuration.ZLinkMessageFlowOutcome outcome();
  public systems.zlink.framework.configuration.ZLinkDispatchErrorSurface surface();
  public systems.zlink.framework.configuration.ZLinkDispatchMessageKind messageKind();
  public java.lang.String packetName();
  public java.lang.String channelName();
  public java.lang.String topic();
  public java.lang.String correlationId();
  public java.lang.String sourceRid();
  public java.lang.String spotId();
  public java.lang.String actorId();
  public java.lang.Long messageSize();
  public systems.zlink.framework.configuration.ZLinkDispatchErrorReason errorReason();
  public systems.zlink.framework.configuration.ZLinkDispatchErrorAction errorAction();
  public java.lang.String errorType();
  public java.lang.String errorMessage();
  public java.lang.String flowId();
  public systems.zlink.framework.monitoring.ZLinkFlowOrigin flowOrigin();
}
public final class systems.zlink.framework.configuration.ZLinkMessageFlowLogMode extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkMessageFlowLogMode> {
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowLogMode OFF;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowLogMode ERRORS_ONLY;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowLogMode KEY_TRANSITIONS;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowLogMode VERBOSE;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowLogMode DIAGNOSTIC;
  public static systems.zlink.framework.configuration.ZLinkMessageFlowLogMode[] values();
  public static systems.zlink.framework.configuration.ZLinkMessageFlowLogMode valueOf(java.lang.String);
  public int value();
}
public interface systems.zlink.framework.configuration.ZLinkMessageFlowObserver {
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> onMessageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowEvent);
}
public interface systems.zlink.framework.configuration.ZLinkMetadataPolicyBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkMetadataPolicyBuilder allowSessionToActor(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMetadataPolicyBuilder allowActorToSession(java.lang.String);
}
public interface systems.zlink.framework.configuration.ZLinkSpotPublisherConfig {
  public abstract long sendHighWaterMark();
  public abstract void setSendHighWaterMark(long);
  public abstract java.util.Optional<java.time.Duration> sendTimeout();
  public abstract void setSendTimeout(java.time.Duration);
  public abstract java.util.Optional<java.time.Duration> linger();
  public abstract void setLinger(java.time.Duration);
}
public interface systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder useDefault();
  public abstract systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder useLz4();
  public abstract systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder use(systems.zlink.framework.streams.ZLinkStreamCompressionCodec);
  public abstract systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder disable();
}
public interface systems.zlink.framework.configuration.ZLinkStreamNodeBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder bind(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder bind();
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder bind(int);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder setBindHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder setAdvertiseHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkStreamSocketConfig configureSocket();
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder setTlsServer(java.lang.String, java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder setTlsServer(java.lang.String, java.lang.String, boolean);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder registerSession(java.lang.Class<? extends systems.zlink.framework.streams.ZLinkSession>);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder enableActorDispatch();
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder addSessionPacketHandler(java.lang.Class<?>);
}
public interface systems.zlink.framework.configuration.ZLinkStreamSocketConfig {
  public abstract long maxMessageSize();
  public abstract void setMaxMessageSize(long);
}
public final class systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction> {
  public static final systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction REPLY_ERROR;
  public static final systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction LOG_AND_DROP;
  public static final systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction DROP;
  public static final systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction THROW;
  public static systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction[] values();
  public static systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction valueOf(java.lang.String);
  public int value();
}
public interface systems.zlink.framework.configuration.ZLinkUnhandledDispatchOptions {
  public abstract void setRequest(systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction);
  public abstract void setSend(systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction);
  public abstract void setPublish(systems.zlink.framework.configuration.ZLinkUnhandledDispatchAction);
  public abstract void setSendLogLevel(systems.zlink.framework.configuration.ZLinkLogLevel);
  public abstract void setPublishLogLevel(systems.zlink.framework.configuration.ZLinkLogLevel);
}
public interface systems.zlink.framework.configuration.ZLinkWorkerOptions {
  public abstract systems.zlink.framework.configuration.ZLinkWorkerOptions minThreads(int);
  public abstract systems.zlink.framework.configuration.ZLinkWorkerOptions maxThreads(int);
  public abstract systems.zlink.framework.configuration.ZLinkWorkerOptions idleTimeout(java.time.Duration);
  public abstract systems.zlink.framework.configuration.ZLinkWorkerOptions maxQueueLength(int);
}
```

`ZLinkStreamNodeBuilder.configureSocket().setMaxMessageSize(...)`의 기본값은 `64 KiB`다. 이
설정은 StreamNode의 Core STREAM inbound에서 client→server complete message를 검사할 때만
사용하며, 크기는 6-byte prefix를 제외한 header와 payload의 합이다. `0`은 Core `-1`로
변환되어 Framework 상한을 사용하지 않고, 음수는 startup configuration error다. 상한을 넘은
message는 handler에 일부도 전달하지 않으며 server는 `EMSGSIZE`와 진단 trace를 남긴 뒤 연결을
종료한다. raw client는 별도 wire error code가 아니라 연결 종료를 관찰한다. server→client
outbound에는 이 Framework 상한을 적용하지 않으며 ClientServer와 RouteMesh SS에는 이 설정이 없다.

## 나머지 구성 public member `javap` inventory

아래 선언은 위 inventory에 포함하지 않은 application-facing configuration type을 `javap`가 출력하는
binary signature 형식으로 고정한다.

```java
public final class systems.zlink.framework.configuration.ZLinkDispatchErrorAction extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkDispatchErrorAction> {
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorAction REPLY_ERROR;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorAction DROP;
  public static systems.zlink.framework.configuration.ZLinkDispatchErrorAction[] values();
  public static systems.zlink.framework.configuration.ZLinkDispatchErrorAction valueOf(java.lang.String);
  public int value();
}
public final class systems.zlink.framework.configuration.ZLinkDispatchErrorReason extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkDispatchErrorReason> {
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorReason HANDLER_MISSING;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorReason PAYLOAD_DECODE_FAILED;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorReason HANDLER_EXCEPTION;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorReason INVALID_FRAME;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorReason REPLY_PATH_MISSING;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorReason UNEXPECTED_REPLY;
  public static systems.zlink.framework.configuration.ZLinkDispatchErrorReason[] values();
  public static systems.zlink.framework.configuration.ZLinkDispatchErrorReason valueOf(java.lang.String);
  public int value();
}
public final class systems.zlink.framework.configuration.ZLinkDispatchErrorSurface extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkDispatchErrorSurface> {
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorSurface CHANNEL;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorSurface ROUTE_MESH_CHANNEL;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorSurface SPOT_ROUTE;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorSurface SPOT_SUBSCRIPTION;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorSurface SPOT_ACTOR;
  public static final systems.zlink.framework.configuration.ZLinkDispatchErrorSurface STREAM_SESSION;
  public static systems.zlink.framework.configuration.ZLinkDispatchErrorSurface[] values();
  public static systems.zlink.framework.configuration.ZLinkDispatchErrorSurface valueOf(java.lang.String);
  public int value();
}
public final class systems.zlink.framework.configuration.ZLinkDispatchFailure extends java.lang.Record {
  public systems.zlink.framework.configuration.ZLinkDispatchFailure(systems.zlink.framework.configuration.ZLinkDispatchErrorSurface, systems.zlink.framework.configuration.ZLinkDispatchMessageKind, systems.zlink.framework.configuration.ZLinkDispatchErrorReason, systems.zlink.framework.configuration.ZLinkDispatchErrorAction, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String, java.lang.String);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public systems.zlink.framework.configuration.ZLinkDispatchErrorSurface surface();
  public systems.zlink.framework.configuration.ZLinkDispatchMessageKind messageKind();
  public systems.zlink.framework.configuration.ZLinkDispatchErrorReason reason();
  public systems.zlink.framework.configuration.ZLinkDispatchErrorAction action();
  public java.lang.String packetName();
  public java.lang.String channelName();
  public java.lang.String topic();
  public java.lang.String spotId();
  public java.lang.String actorId();
  public java.lang.String sourceRid();
  public java.lang.String correlationId();
  public java.lang.String errorType();
  public java.lang.String errorMessage();
}
public final class systems.zlink.framework.configuration.ZLinkDispatchMessageKind extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkDispatchMessageKind> {
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind REQUEST;
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind SEND;
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind PUBLISH;
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind RESPONSE;
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind ERROR;
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind ACTOR_REQUEST;
  public static final systems.zlink.framework.configuration.ZLinkDispatchMessageKind ACTOR_SEND;
  public static systems.zlink.framework.configuration.ZLinkDispatchMessageKind[] values();
  public static systems.zlink.framework.configuration.ZLinkDispatchMessageKind valueOf(java.lang.String);
  public int value();
}
public interface systems.zlink.framework.configuration.ZLinkFrameworkOptions {
  public abstract java.time.Duration defaultRequestTimeout();
  public abstract void setDefaultRequestTimeout(java.time.Duration);
  public abstract systems.zlink.framework.configuration.ZLinkCodecRegistryBuilder codecs();
  public abstract void addHandlersFromPackageOf(java.lang.Class<?>);
  public abstract systems.zlink.framework.configuration.ZLinkMetadataPolicyBuilder configureMetadata();
  public abstract void addRelocationStore(systems.zlink.framework.locations.ZLinkRelocationStore);
  public abstract void setApplicationVersion(long);
  public abstract void setMaintenanceWave(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder addRouteMesh(java.lang.String);
  public abstract systems.zlink.framework.configuration.ClientServerChannelBuilder addClientServerChannel(java.lang.String);
  public abstract systems.zlink.framework.configuration.FanoutChannelBuilder addFanoutChannel(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder addStreamNode(java.lang.String);
  public abstract void addLocationStore(systems.zlink.framework.locations.ZLinkLocationStore);
  public abstract systems.zlink.framework.locations.ZLinkLocationOptions configureLocations();
  public abstract systems.zlink.framework.configuration.ZLinkInboundDispatchOptions configureInboundDispatch();
  public abstract systems.zlink.framework.configuration.ZLinkNetworkOptions configureNetwork();
  public abstract void useFilter(java.lang.Class<? extends systems.zlink.framework.ZLinkHandlerFilter>);
  public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions configureDispatch();
  public abstract systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder configureStreamCompression();
  public abstract systems.zlink.framework.configuration.ZLinkWorkerOptions configureWorkers();
  public abstract void useVirtualThreadHandlers();
  public abstract void useHandlerExecutor(java.util.concurrent.Executor);
}
public interface systems.zlink.framework.locations.ZLinkLocationOptions {
  public abstract java.time.Duration ownerLeaseRenewInterval();
  public abstract void setOwnerLeaseRenewInterval(java.time.Duration);
  public abstract java.time.Duration ownerLeaseTtl();
  public abstract void setOwnerLeaseTtl(java.time.Duration);
  public abstract java.time.Duration pollingInterval();
  public abstract void setPollingInterval(java.time.Duration);
  public abstract java.time.Duration storeFailureGrace();
  public abstract void setStoreFailureGrace(java.time.Duration);
  public abstract java.time.Duration ownerLeaseFencingMargin();
  public abstract void setOwnerLeaseFencingMargin(java.time.Duration);
  public abstract java.time.Duration ownerLeaseRenewTimeout();
  public abstract void setOwnerLeaseRenewTimeout(java.time.Duration);
  public abstract java.time.Duration routeCacheMaxAge();
  public abstract void setRouteCacheMaxAge(java.time.Duration);
  public abstract java.time.Duration messageFollowDuration();
  public abstract void setMessageFollowDuration(java.time.Duration);
  public abstract int maxActiveOutboundRelocations();
  public abstract void setMaxActiveOutboundRelocations(int);
  public abstract int maxActiveInboundRelocations();
  public abstract void setMaxActiveInboundRelocations(int);
  public abstract int maxConcurrentRelocationCaptures();
  public abstract void setMaxConcurrentRelocationCaptures(int);
  public abstract int maxConcurrentRelocationRestores();
  public abstract void setMaxConcurrentRelocationRestores(int);
  public abstract long maxRelocationPayloadInFlightBytes();
  public abstract void setMaxRelocationPayloadInFlightBytes(long);
}
public final class systems.zlink.framework.configuration.ZLinkLogLevel extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkLogLevel> {
  public static final systems.zlink.framework.configuration.ZLinkLogLevel TRACE;
  public static final systems.zlink.framework.configuration.ZLinkLogLevel DEBUG;
  public static final systems.zlink.framework.configuration.ZLinkLogLevel INFO;
  public static final systems.zlink.framework.configuration.ZLinkLogLevel WARN;
  public static final systems.zlink.framework.configuration.ZLinkLogLevel ERROR;
  public static systems.zlink.framework.configuration.ZLinkLogLevel[] values();
  public static systems.zlink.framework.configuration.ZLinkLogLevel valueOf(java.lang.String);
  public int value();
}
public interface systems.zlink.framework.configuration.ZLinkMeshNodeBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkMeshChannelBuilder channel(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder listen(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder listen();
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder listen(int);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setBindHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setAdvertiseHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setRoutingId(systems.zlink.contracts.core.RoutingId);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setRoutingIdPrefix(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setPlacementWeight(int);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setActorCapacity(int);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setSpotCapacity(int);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setActivationConcurrency(int);
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setInstanceSpotIdleTimeout(java.time.Duration);
  public abstract systems.zlink.framework.configuration.ZLinkMeshObjectRoleBuilder objects();
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeSocketConfig configureRouterSocket();
  public abstract systems.zlink.framework.configuration.ZLinkSpotPublisherConfig configureSpotPublisher();
  public abstract systems.zlink.framework.configuration.ZLinkMeshPeerConnections peerConnections();
  public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setDefaultRequestTimeout(java.time.Duration);
  public abstract <THandler, TMessage> systems.zlink.framework.configuration.ZLinkMeshNodeBuilder addRouteSendHandler(java.lang.Class<THandler>, java.lang.Class<TMessage>);
  public abstract <THandler, TRequest, TReply> systems.zlink.framework.configuration.ZLinkMeshNodeBuilder addRouteRequestHandler(java.lang.Class<THandler>, java.lang.Class<TRequest>, java.lang.Class<TReply>);
}
public interface systems.zlink.framework.configuration.ZLinkMeshObjectRoleBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkMeshObjectClientBuilder client();
  public abstract systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder server();
}
public interface systems.zlink.framework.configuration.ZLinkMeshObjectClientBuilder {
}
public interface systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder addEntrySpot(java.lang.Class<? extends systems.zlink.framework.spots.ZLinkEntrySpot<?>>);
  public abstract <TSpot extends systems.zlink.framework.spots.ZLinkSpot<?>> systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder addSpotFactory(java.lang.String, java.lang.Class<TSpot>, java.util.function.Consumer<systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder<TSpot>>);
  public abstract <TSpot extends systems.zlink.framework.spots.ZLinkInstanceSpot> systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder addInstanceSpotFactory(java.lang.String, java.lang.Class<TSpot>, java.util.function.Consumer<systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder<TSpot>>);
  public abstract <TActor extends systems.zlink.framework.actors.ZLinkActor> systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder addActorFactory(java.lang.String, java.lang.Class<TActor>, java.lang.Class<? extends systems.zlink.framework.actors.ZLinkActorFactory>, java.util.function.Consumer<systems.zlink.framework.configuration.ZLinkActorFactoryBuilder<TActor>>);
}
public final class systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode> {
  public static final systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode SPOT_WIDE;
  public static final systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode PER_ACTOR;
  public static systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode[] values();
  public static systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode valueOf(java.lang.String);
  public int value();
}
public final class systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode> {
  public static final systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode ANY_TURN_BOUNDARY;
  public static final systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode APPLICATION_SIGNALED;
  public static systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode[] values();
  public static systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode valueOf(java.lang.String);
  public int value();
}
public interface systems.zlink.framework.configuration.ZLinkActorFactoryBuilder<TActor extends systems.zlink.framework.actors.ZLinkActor> {
  public abstract void disableRelocation();
  public abstract void recreateOnRelocation();
  public abstract void preserveStateWith(java.lang.Class<? extends systems.zlink.framework.actors.ZLinkActorRelocationAdapter<TActor>>);
}
public interface systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder<TSpot extends systems.zlink.framework.spots.ZLinkSpot<?>> {
  public abstract systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder<TSpot> stableTypeLimit(int);
  public abstract systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder<TSpot> executionMode(systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode);
  public abstract systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder<TSpot> relocationReadiness(systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode);
  public abstract void disableRelocation();
  public abstract void recreateOnRelocation();
  public abstract void preserveStateWith(java.lang.Class<? extends systems.zlink.framework.spots.ZLinkSpotRelocationAdapter<TSpot>>);
}
public interface systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder<TSpot extends systems.zlink.framework.spots.ZLinkInstanceSpot> {
  public abstract systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder<TSpot> stableTypeLimit(int);
  public abstract void disableRelocation();
  public abstract void recreateOnRelocation();
  public abstract void preserveStateWith(java.lang.Class<? extends systems.zlink.framework.spots.ZLinkSpotRelocationAdapter<TSpot>>);
}
public final class systems.zlink.framework.configuration.ZLinkMessageFlowOutcome extends java.lang.Enum<systems.zlink.framework.configuration.ZLinkMessageFlowOutcome> {
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome RECEIVED;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome DISPATCHED;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome REPLIED;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome DROPPED;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome SENT;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome REPLY_RECEIVED;
  public static final systems.zlink.framework.configuration.ZLinkMessageFlowOutcome ERROR;
  public static systems.zlink.framework.configuration.ZLinkMessageFlowOutcome[] values();
  public static systems.zlink.framework.configuration.ZLinkMessageFlowOutcome valueOf(java.lang.String);
  public int value();
}
public interface systems.zlink.framework.spring.EnableZLinkFramework extends java.lang.annotation.Annotation {
}
public interface systems.zlink.framework.spring.ZLinkMetricsCustomizer {
  public abstract void customize(io.micrometer.core.instrument.MeterRegistry);
}
```

## Spring bean 계약

Spring starter는 `ZLinkFrameworkRuntime`, `ZLinkRouteMeshRuntime`, `ZLinkClientServerRuntime`과
`ZLinkFanoutRuntime`을 singleton bean으로 제공한다. 세 topology bean은 runtime의 대응 accessor가 반환한
객체를 그대로 등록하므로 새 adapter나 별도 runtime을 만들지 않는다. Public client와 나머지 runtime service
bean도 같은 `ZLinkFrameworkRuntime`이 소유한 객체를 사용한다.

Bean을 생성하는 동안 service socket, discovery loop와 application worker를 시작하지 않는다.
`SmartLifecycle.start()`가 같은 runtime의 start를 한 번 호출한다. Application에 보장하는 계약은 public bean의
type, singleton 수명과 reference identity다. Auto-configuration class, bean factory method, lifecycle adapter와
그 constructor는 implementation detail이며 application public signature가 아니다.

Core의 internal bootstrap package는 starter module에만 qualified export한다. Application이 module path나
classpath에서 `ZLinkFrameworkRuntime.start(...)`를 호출하면 compile에 실패해야 한다. Contract test는
실제 module path에서 bootstrap을 호출하고 runtime이 `SERVING`에 도달하는지 확인한다.

Contract test는 네 runtime bean을 각각 두 번 resolve해 singleton인지 확인한다. 이어서 세 topology bean을
`runtime.routeMeshRuntime()`, `runtime.clientServerRuntime()`과 `runtime.fanoutRuntime()`의 반환값과
`assertSame`으로 비교한다. Lifecycle 회귀 test는 bean 생성 전후에 service socket이 시작되지 않고, start와
shutdown이 같은 `ZLinkFrameworkRuntime`에 한 번씩 전달되는지 확인한다.
