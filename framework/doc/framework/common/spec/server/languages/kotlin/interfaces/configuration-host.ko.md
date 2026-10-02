# Kotlin 구성과 host 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Java 구성](../../java/interfaces/configuration-host.ko.md) ·
[MeshNode 공통 계약](../../../03-spot-actor/03-mesh-node.ko.md)

Kotlin은 Java `client()`와 `server()` builder를 재사용한다. ClientServer와 fanout 연결은 [ClientServer channel](../../../02-channel-transport/03-client-server-channel.ko.md)과 [Channel topology](../../../02-channel-transport/01-channel-topology.ko.md)이 정한다.

RouteMesh 연결 방향은 [Channel topology §8](../../../02-channel-transport/01-channel-topology.ko.md)가 정한다.

Object Client peer 필요성은 [Channel topology §8](../../../02-channel-transport/01-channel-topology.ko.md)가 정한다.

[MeshNode](../../../00-foundation/02-glossary.ko.md#meshnode)의 object role은 `None`, `Client`, `Server` 중 하나다. `objects()`를 호출하지 않으면 `None`,
`client()`는 outbound manager와 resolve를 제공하고 `server()`는 Client 기능과 [factory](../../../00-foundation/02-glossary.ko.md#factory)·Entry registration을
함께 제공한다. Client와 Server는 Location Store가 필요하다. None에는 object manager나 factory가 없다.
한 node에서 role을 중복 선택하면 startup configuration error다.
Object Client에도 RouteMesh Channel Server를 등록할 수 있지만 application Node direct handler는 등록할
수 없다. Object Client RID를 Node direct target으로 지정하면 다른 RID로 바꾸지 않고 not-found로 끝낸다.

`ZLinkFrameworkOptions.addLocationStore(...)`와 `addRelocationStore(...)`는 Java public member를 그대로 사용한다.
`recreateOnRelocation()` 또는 `preserveStateWith(...)`를 선택한 factory가 하나라도 있거나 Instance Spot
factory가 하나라도 있으면 Relocation Store를 정확히 하나 등록해야 한다. Missing·duplicate registration은
socket bind 전에 configuration error다. [Instance Spot](../../../00-foundation/02-glossary.ko.md#entry-spot-user-spot과-instance-spot) factory가 없고
`disableRelocation()`만 선택한 same-node 구성에는 Relocation Store가 필수가 아니다. 두 capability를 묶는
Kotlin DSL이나 Redis 전용 registration helper는 제공하지 않는다.
Cross-node Actor·[Spot](../../../00-foundation/02-glossary.ko.md#spot) 이동의 application state·queue·timer handoff
payload는 Relocation Store에 저장하지 않는다. Source가 payload를 memory에 유지한 채 source–target
ordered mesh 연결로 직접 chunk 전송하며, source memory가 복원 원본이다. Relocation Store는 Instance
Spot cold activation 기록과 relocation 뒤 완료되는 pending request의 terminal 기록을 계속 소유하므로
위 등록 요구는 유지된다. Same-node Actor join은 relocation payload를 만들지 않고,
`disableRelocation()`을 선택한 cross-node 이동은 capture 전에 거부한다.

다음 Java builder member는 Kotlin에서 property 변환 없이 같은 JVM signature로 직접 호출한다.

```java
public enum systems.zlink.framework.configuration.ZLinkCoreHwmProfile {
 COMPACT,
 LOW_LATENCY,
 BALANCED,
 THROUGHPUT
}
public enum systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile {
 COMPACT,
 LOW_LATENCY,
 BALANCED,
 THROUGHPUT
}
public interface systems.zlink.framework.configuration.ZLinkDispatchOptions {
 public abstract systems.zlink.framework.configuration.ZLinkUnhandledDispatchOptions unhandled();
 public abstract systems.zlink.framework.configuration.ZLinkDiagnosticsOptions diagnostics();
 public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode);
 public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions traceSampleRate(double);
 public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions includeMessageSizes(boolean);
}
public interface systems.zlink.framework.configuration.ZLinkInboundDispatchOptions {
 public abstract java.util.OptionalLong coreHwmMemoryLimitBytes();
 public abstract void setCoreHwmMemoryLimitBytes(long);
 public abstract java.util.OptionalLong coreHwmBudgetBytes();
 public abstract void setCoreHwmBudgetBytes(long);
 public abstract systems.zlink.framework.configuration.ZLinkCoreHwmProfile coreHwmProfile();
 public abstract void setCoreHwmProfile(systems.zlink.framework.configuration.ZLinkCoreHwmProfile);
 public abstract systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile applicationJobQueueProfile();
 public abstract void setApplicationJobQueueProfile(systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile);
 public abstract java.util.OptionalLong maxQueuedApplicationJobs();
 public abstract void setMaxQueuedApplicationJobs(long);
 public abstract int applicationJobQueuePauseThresholdPercent();
 public abstract void setApplicationJobQueuePauseThresholdPercent(int);
 public abstract int applicationJobQueueResumeThresholdPercent();
 public abstract void setApplicationJobQueueResumeThresholdPercent(int);
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
 public abstract java.time.Duration sessionRelocationSealTimeout();
 public abstract void setSessionRelocationSealTimeout(java.time.Duration);
 public abstract long relocationPayloadChunkLimitBytes();
 public abstract void setRelocationPayloadChunkLimitBytes(long);
 public abstract long relocationInFlightPayloadBudgetBytes();
 public abstract void setRelocationInFlightPayloadBudgetBytes(long);
 public abstract long relocationNodeInFlightPayloadBudgetBytes();
 public abstract void setRelocationNodeInFlightPayloadBudgetBytes(long);
 public abstract java.time.Duration relocationCutoverWaitTimeout();
 public abstract void setRelocationCutoverWaitTimeout(java.time.Duration);
}
public interface systems.zlink.framework.configuration.ZLinkMeshNodeBuilder {
 public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setRoutingIdPrefix(java.lang.String);
 public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setPlacementWeight(int);
 public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setActorCapacity(int);
 public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setSpotCapacity(int);
 public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setActivationConcurrency(int);
 public abstract systems.zlink.framework.configuration.ZLinkMeshObjectRoleBuilder objects();
}
public interface systems.zlink.framework.configuration.ZLinkMeshObjectRoleBuilder {
 public abstract systems.zlink.framework.configuration.ZLinkMeshObjectClientBuilder client();
 public abstract systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder server();
}
public interface systems.zlink.framework.configuration.ZLinkStreamNodeBuilder {
 public abstract systems.zlink.framework.configuration.ZLinkStreamSocketConfig configureSocket();
 public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder enableActorDispatch();
}
public interface systems.zlink.framework.configuration.ZLinkStreamSocketConfig {
 public abstract long maxMessageSize();
 public abstract void setMaxMessageSize(long);
}
```

Core HWM과 application job queue 구성은 `options.configureInboundDispatch()`가 반환하는
`ZLinkInboundDispatchOptions`에서 직접 설정한다. `configureDispatch { ... }`는 diagnostics DSL이며
HWM이나 job queue 설정을 전달하지 않는다.

`sessionRelocationSealTimeout()`은 Java와 같은 startup-only 양수 `Duration`이고 기본값은 3초다.
Millisecond 변환 불가, 0, 음수와 무한대는 socket bind 전에 configuration error다.

`relocationPayloadChunkLimitBytes()`, `relocationInFlightPayloadBudgetBytes()`,
`relocationNodeInFlightPayloadBudgetBytes()`과 `relocationCutoverWaitTimeout()`도 Java 공개 계약을 그대로
사용한다. Chunk limit은 relocation payload를 나눈 encoded chunk 하나의 최대 크기(byte)로 기본값
256 KiB이며 transport가 협상한 frame 한도를 넘게 설정하면 socket bind 전에 startup configuration
error다. In-flight budget은 peer 연결당 동시 전송 chunk byte 합계 상한으로 기본값 16 MiB, `0`은
미적용이다. Node in-flight budget은 같은 규칙의 node 전체 합계이고 기본값 `0`은 미적용이다.
Cutover wait timeout은 cutover 대기 Warning 임계값으로 기본값 1초다. Source 사본 유지와
authority CAS는 [공통 relocation §4.4](../../../05-location-relocation/04-relocation-flow.ko.md#44-ordered-relay와-one-way-cutover)를 따른다. 네 값 모두 startup-only이며 음수는 socket bind 전에 configuration error다.

Kotlin은 Java `ZLinkStreamNodeBuilder.configureSocket()`와
`ZLinkStreamSocketConfig.setMaxMessageSize(...)`를 그대로 사용한다. 기본값은 `64 KiB`이며,
StreamNode의 Core STREAM inbound에서 client→server complete message에만 적용한다. 크기는
6-byte prefix를 제외한 header와 payload의 합이다. `0`은 Core `-1`로 변환되어 Framework
상한을 사용하지 않고, 음수는 startup configuration error다. 상한을 넘은 message는 handler에
일부도 전달하지 않으며 server는 `EMSGSIZE`와 진단 trace를 남기고 연결을 종료한다. raw client는
별도 wire error code가 아니라 연결 종료를 관찰한다. server→client outbound에는 상한을 적용하지
않으며 ClientServer와 RouteMesh SS에는 이 설정을 추가하지 않는다.

Kotlin binding은 Java runtime의 양수 유한 `Runtime.maxMemory()`를 Core runtime memory hint로 전달한다.
Core profile과 Application job queue profile은 Java 공개 계약의 독립된 enum과 계산을 그대로 사용한다.
두 profile의 기본값은 `BALANCED`이고 pressure threshold 기본값은 pause `80`, resume `60`이다. Pause는
`1..100`, resume은 `0..99`의 정수이며 resume은 pause보다 작아야 한다. Manual job cap, startup CPU
snapshot과 bind 전 범위·순서·overflow 검증도 Java 공개 계약과 같다.

## Kotlin source signature

아래 reified 등록 확장은 모두 type parameter를 `T::class.java`로 같은 이름의 Java member에 넘기고, 다른 인자는 그대로 전달하며, 검증·기본값·오류를 더하지 않는다.

```kotlin
fun ZLinkFrameworkOptions.useCoroutineHandlers(dispatcher: CoroutineDispatcher)
fun ZLinkFrameworkOptions.useCoroutineHandlers(
 scope: CoroutineScope,
 dispatcher: CoroutineDispatcher,
)

inline fun ZLinkFrameworkOptions.configureDispatch(
 block: ZLinkDispatchOptions.() -> Unit,
): ZLinkDispatchOptions

fun ZLinkFrameworkOptions.configureStreamCompression(
 configure: ZLinkStreamCompressionBuilder.() -> Unit,
): ZLinkFrameworkOptions

inline fun <reified TMarker : Any> ZLinkFrameworkOptions.addHandlersFromPackageOf()

inline fun <reified TFilter : ZLinkHandlerFilter> ZLinkFrameworkOptions.useFilter()

inline fun <reified TEntrySpot : ZLinkEntrySpot<*>>
 ZLinkMeshObjectServerBuilder.addEntrySpot(): ZLinkMeshObjectServerBuilder

inline fun <reified TSpot : ZLinkSpot<*>> ZLinkMeshObjectServerBuilder.addSpotFactory(
 stableType: String,
 noinline configure: ZLinkUserSpotFactoryBuilder<TSpot>.() -> Unit,
): ZLinkMeshObjectServerBuilder

inline fun <reified TSpot : ZLinkInstanceSpot> ZLinkMeshObjectServerBuilder.addInstanceSpotFactory(
 stableType: String,
 noinline configure: ZLinkInstanceSpotFactoryBuilder<TSpot>.() -> Unit,
): ZLinkMeshObjectServerBuilder

inline fun <reified TActor : ZLinkActor, reified TFactory : ZLinkActorFactory>
 ZLinkMeshObjectServerBuilder.addActorFactory(
 actorType: String,
 noinline configure: ZLinkActorFactoryBuilder<TActor>.() -> Unit,
): ZLinkMeshObjectServerBuilder

inline fun <TActor : ZLinkActor, reified TAdapter : ZLinkActorRelocationAdapter<TActor>>
 ZLinkActorFactoryBuilder<TActor>.preserveStateWith()

inline fun <TSpot : ZLinkSpot<*>, reified TAdapter : ZLinkSpotRelocationAdapter<TSpot>>
 ZLinkUserSpotFactoryBuilder<TSpot>.preserveStateWith()

inline fun <TSpot : ZLinkInstanceSpot, reified TAdapter : ZLinkSpotRelocationAdapter<TSpot>>
 ZLinkInstanceSpotFactoryBuilder<TSpot>.preserveStateWith()

@JvmName("addPublishHandlerForMessage")
inline fun <reified THandler : Any, reified TMessage : Any> FanoutChannelBuilder.addPublishHandler()

@JvmName("addPublishHandlerForMessageAndPacketName")
inline fun <reified THandler : Any, reified TMessage : Any>
 FanoutChannelBuilder.addPublishHandler(packetName: String)

inline fun <reified THandler : Any> FanoutChannelBuilder.addPublishHandler(): FanoutChannelBuilder

inline fun <reified THandler : Any>
 FanoutChannelBuilder.addPublishHandler(packetName: String): FanoutChannelBuilder

inline fun <reified TSession : ZLinkSession>
 ZLinkStreamNodeBuilder.registerSession(): ZLinkStreamNodeBuilder

inline fun <reified THandler : Any>
 ZLinkStreamNodeBuilder.addSessionPacketHandler(): ZLinkStreamNodeBuilder
```

Factory configure callback에는 default가 없다. Actor factory builder에는 relocation 동작 선택 외의 설정이 없다. Node placement
[weight](../../../00-foundation/02-glossary.ko.md#weight)는 0..10000이고 기본값은 100이다. 범위 밖 값은 startup 설정과
runtime 변경에서 configuration error다. Channel weight와 별개이며 runtime update와 descriptor
[snapshot](../../../00-foundation/02-glossary.ko.md#snapshot)에 같은 값을 사용한다.
RouteMesh Channel Server와 ClientServer Server weight도 같은 범위와 기본값을 사용한다. Weighted
selection은 후보 weight 합계를 최소 64-bit 정수로 계산한다.

MeshNode와 Store-backed fanout publisher의 automatic RID는
`prefix-<lowercase-canonical-uuid-v4>` 형식이다. UUID v4는 `8-4-4-4-12` 자리의 lowercase canonical
문자열로 표현한다. Prefix는 ASCII `[A-Za-z0-9._-]` 1..64자이고 full RID는 UTF-8 255 bytes 이하다.
Active owner와 충돌하면 새 UUID로 다시 시도하지 않고 즉시 `RoutingIdConflict`로 실패한다. Fixed RID의 사용 범위와 재시작 충돌은 [공통 MeshNode §3.3](../../../03-spot-actor/03-mesh-node.ko.md#33-fixed-rid)이 정한다. Slot count, allocation group과 public allocation provider는 없다.

Object Server의 Entry Spot ID는 같은 prefix의
`<prefix>-entry-<lowercase-canonical-uuid-v4>` 형식이며 MeshNode와 별도로 생성한 UUID v4를 사용한다.
Java `ZLinkMeshNodeDescriptor.entrySpotId()`가 같은 lifecycle의 mapping을 제공한다. Global Spot
ID가 active owner와 충돌하면 새 UUID로 다시 시도하지 않고 즉시 `SpotIdConflict`로 startup을
실패시킨다. Caller가 지정한 User·Instance Spot ID가 예약 형식과 일치하면 Store와 factory 전에
startup configuration error로 거부한다.

모든 factory는 Java builder를 Kotlin receiver callback으로 구성한다. Callback은
`disableRelocation()`, `recreateOnRelocation()`, `preserveStateWith(...)` 중 정확히 하나를 호출한다. 누락하거나
둘 이상 호출하면 socket bind 전에 startup configuration error다. Kotlin 전용 policy value와 suspending
adapter는 추가하지 않는다. 별도 등록 API는 없다.

Framework는 receiver callback을 등록 호출 안에서 동기적으로 한 번만 실행한다. Callback이 반환된 뒤 보관한
builder를 다시 호출하면 configuration error다. Callback이 예외를 던지면 해당 factory를 등록하지 않고 같은
예외를 호출자에게 전달한다. `stableTypeLimit(...)`을 생략하면 node limit을 공유하며 명시한 값은
1..`Int.MAX_VALUE`여야 한다. 0과 음수는 callback 실행 중 configuration error다.

`recreateOnRelocation()` 또는 `preserveStateWith(...)`를 선택한 factory가 하나라도 있거나 Instance Spot factory가 하나라도 등록된 Object Server는
Java root의 `addRelocationStore(...)`로 Relocation Store를 정확히 하나 등록한다. Instance Spot factory가 없고
모든 factory가 `disableRelocation()`을 선택한 same-node 구성만 이를 생략할 수 있다.

## generated JVM signature

```java
public final class systems.zlink.framework.kotlin.ZLinkCoroutineHandlerOptionsKt {
 public static final void useCoroutineHandlers(systems.zlink.framework.configuration.ZLinkFrameworkOptions, kotlinx.coroutines.CoroutineDispatcher);
 public static final void useCoroutineHandlers(systems.zlink.framework.configuration.ZLinkFrameworkOptions, kotlinx.coroutines.CoroutineScope, kotlinx.coroutines.CoroutineDispatcher);
}
public final class systems.zlink.framework.kotlin.ZLinkDispatchOptionsExtensionsKt {
 public static final systems.zlink.framework.configuration.ZLinkDispatchOptions configureDispatch(systems.zlink.framework.configuration.ZLinkFrameworkOptions, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkDispatchOptions, kotlin.Unit>);
}
public final class systems.zlink.framework.kotlin.ZLinkFrameworkExtensionsKt {
 public static final systems.zlink.framework.configuration.ZLinkFrameworkOptions configureStreamCompression(systems.zlink.framework.configuration.ZLinkFrameworkOptions, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder, kotlin.Unit>);
 public static final <TMarker> void addHandlersFromPackageOf(systems.zlink.framework.configuration.ZLinkFrameworkOptions);
 public static final <TFilter extends systems.zlink.framework.ZLinkHandlerFilter> void useFilter(systems.zlink.framework.configuration.ZLinkFrameworkOptions);
 public static final <TEntrySpot extends systems.zlink.framework.spots.ZLinkEntrySpot<?>> systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder addEntrySpot(systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder);
 public static final <TSpot extends systems.zlink.framework.spots.ZLinkSpot<?>> systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder addSpotFactory(systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder, java.lang.String, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder<TSpot>, kotlin.Unit>);
 public static final <TSpot extends systems.zlink.framework.spots.ZLinkInstanceSpot> systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder addInstanceSpotFactory(systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder, java.lang.String, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder<TSpot>, kotlin.Unit>);
 public static final <TActor extends systems.zlink.framework.actors.ZLinkActor, TFactory extends systems.zlink.framework.actors.ZLinkActorFactory> systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder addActorFactory(systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder, java.lang.String, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkActorFactoryBuilder<TActor>, kotlin.Unit>);
 public static final <TActor extends systems.zlink.framework.actors.ZLinkActor, TAdapter extends systems.zlink.framework.actors.ZLinkActorRelocationAdapter<TActor>> void preserveStateWith(systems.zlink.framework.configuration.ZLinkActorFactoryBuilder<TActor>);
 public static final <TSpot extends systems.zlink.framework.spots.ZLinkSpot<?>, TAdapter extends systems.zlink.framework.spots.ZLinkSpotRelocationAdapter<TSpot>> void preserveStateWith(systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder<TSpot>);
 public static final <TSpot extends systems.zlink.framework.spots.ZLinkInstanceSpot, TAdapter extends systems.zlink.framework.spots.ZLinkSpotRelocationAdapter<TSpot>> void preserveStateWith(systems.zlink.framework.configuration.ZLinkInstanceSpotFactoryBuilder<TSpot>);
 public static final <THandler, TMessage> void addPublishHandlerForMessage(systems.zlink.framework.configuration.FanoutChannelBuilder);
 public static final <THandler, TMessage> void addPublishHandlerForMessageAndPacketName(systems.zlink.framework.configuration.FanoutChannelBuilder, java.lang.String);
 public static final <THandler> systems.zlink.framework.configuration.FanoutChannelBuilder addPublishHandler(systems.zlink.framework.configuration.FanoutChannelBuilder);
 public static final <THandler> systems.zlink.framework.configuration.FanoutChannelBuilder addPublishHandler(systems.zlink.framework.configuration.FanoutChannelBuilder, java.lang.String);
 public static final <TSession extends systems.zlink.framework.streams.ZLinkSession> systems.zlink.framework.configuration.ZLinkStreamNodeBuilder registerSession(systems.zlink.framework.configuration.ZLinkStreamNodeBuilder);
 public static final <THandler> systems.zlink.framework.configuration.ZLinkStreamNodeBuilder addSessionPacketHandler(systems.zlink.framework.configuration.ZLinkStreamNodeBuilder);
}
```
