# Kotlin 구성과 host 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Java 구성](../../java/interfaces/configuration-host.ko.md) ·
[MeshNode 공통 계약](../../../../13-mesh-node.ko.md)

Kotlin application은 Java builder를 직접 사용한다. Kotlin DSL은 receiver와 reified type으로 실제 중복을
줄이는 경우에만 제공하며 Java contract에 없는 역할, factory default, allocation provider를 만들지 않는다.
따라서 ClientServer의 Client-only connect와 Server RID·lifecycle generation별 intent 통합, fanout의
Subscriber-only connect와 automatic·manual subscriber 혼합 금지는
[Java 구성](../../java/interfaces/configuration-host.ko.md)의 같은 계약을 그대로 적용한다.
같은 ClientServer ChannelName에는 Java builder의 `client()`와 `server()`를 각각 한 번 등록할 수 있으며
별도 Kotlin DSL이나 public API를 추가하지 않는다. 두 역할은 `(ChannelName, Role)` key의 별도
registration으로 하나의 topology를 공유하고 같은 역할의 중복은 startup 오류다. Local Server도 remote
Server와 같은 readiness·weight·drain 조건으로 선택하며 local
우선순위나 handler 직접 호출을 사용하지 않는다.

Automatic RouteMesh는 RID를 canonical byte order로 비교하고 더 작은 RID의 MeshNode만 상대 endpoint로
connect한다. Manual topology는 application endpoint 구성에 따라 한쪽 또는 양쪽에서 connect할 수 있다.
양쪽 연결이나 automatic discovery 경합·오래된 snapshot으로 중복 후보가 생기면 handshake와 admission이
같은 RID와 [lifecycle generation](../../../../01-glossary.ko.md#lifecycle-generation)을 확인해 하나만 ready 상태로 유지한다.

두 MeshNode가 모두 Object Client이고 양쪽 모두 RouteMesh Channel Server membership이 없을 때만 peer
connection이 필요하지 않다. Channel Client membership만 등록한 경우도 같다. 어느 한쪽에라도 weight
`0`을 포함한 Channel Server membership이 있으면 연결이 필요하다. ClientServer와 classic fanout은 별도
물리 topology이므로 이 판정에 포함하지 않는다.

[MeshNode](../../../../01-glossary.ko.md#meshnode)의 object role은 `None`, `Client`, `Server` 중 하나다. `objects()`를 호출하지 않으면 `None`,
`client()`는 outbound manager와 resolve를 제공하고 `server()`는 Client 기능과 [factory](../../../../01-glossary.ko.md#factory)·Entry registration을
함께 제공한다. Client와 Server는 Location Store가 필요하다. None에는 object manager나 factory가 없다.
한 node에서 role을 중복 선택하면 startup configuration error다.
Object Client에도 RouteMesh Channel Server를 등록할 수 있지만 application Node direct handler는 등록할
수 없다. Object Client RID를 Node direct target으로 지정하면 다른 RID로 바꾸지 않고 not-found로 끝낸다.

`ZLinkFrameworkOptions.addLocationStore(...)`와 `addRelocationStore(...)`는 Java public member를 그대로 사용한다.
`recreateOnRelocation()` 또는 `preserveStateWith(...)`를 선택한 factory가 하나라도 있거나 Instance Spot
factory가 하나라도 있으면 Relocation Store를 정확히 하나 등록해야 한다. Missing·duplicate registration은
socket bind 전에 configuration error다. [Instance Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot) factory가 없고
`disableRelocation()`만 선택한 same-node 구성에는 Relocation Store가 필수가 아니다. 두 capability를 묶는
Kotlin DSL이나 Redis 전용 registration helper는 제공하지 않는다.
완료 가능한 모든 cross-node Actor·[Spot](../../../../01-glossary.ko.md#spot) 이동은 Relocation Store를 사용한다.
`recreateOnRelocation()`도 accepted journal과 recovery payload를 저장하고 `preserveStateWith(...)`는 application
state를 추가로 저장한다. Same-node Actor join은 Relocation payload를 만들지 않고,
`disableRelocation()`을 선택한 cross-node 이동은 capture 전에 거부한다.

다음 Java builder member는 Kotlin에서 property 변환 없이 같은 JVM signature로 직접 호출한다.

```java
public enum systems.zlink.framework.configuration.ZLinkApplicationHwmProfile {
  COMPACT,
  LOW_LATENCY,
  BALANCED,
  THROUGHPUT
}
public interface systems.zlink.framework.configuration.ZLinkInboundDispatchOptions {
  public abstract java.util.OptionalLong applicationHwmBytes();
  public abstract void setApplicationHwmBytes(long);
  public abstract systems.zlink.framework.configuration.ZLinkApplicationHwmProfile applicationHwmProfile();
  public abstract void setApplicationHwmProfile(systems.zlink.framework.configuration.ZLinkApplicationHwmProfile);
  public abstract java.util.OptionalLong processMemoryLimitBytes();
  public abstract void setProcessMemoryLimitBytes(long);
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

Kotlin은 Java `ZLinkStreamNodeBuilder.configureSocket()`와
`ZLinkStreamSocketConfig.setMaxMessageSize(...)`를 그대로 사용한다. 기본값은 `64 KiB`이며,
StreamNode의 Core STREAM inbound에서 client→server complete message에만 적용한다. 크기는
6-byte prefix를 제외한 header와 payload의 합이다. `0`은 Core `-1`로 변환되어 Framework
상한을 사용하지 않고, 음수는 startup configuration error다. 상한을 넘은 message는 handler에
일부도 전달하지 않으며 server는 `EMSGSIZE`와 진단 trace를 남기고 연결을 종료한다. raw client는
별도 wire error code가 아니라 연결 종료를 관찰한다. server→client outbound에는 상한을 적용하지
않으며 ClientServer와 RouteMesh SS에는 이 설정을 추가하지 않는다.

Kotlin은 Java runtime의 managed heap 상한(`Runtime.maxMemory()`)을 사용한다. `processMemoryLimitBytes()`가
비어 있으면 process에 적용된 유한한 OS 상한과 JVM managed heap 상한을 함께 확인하고, 둘 다 있으면 더 작은
값을 Auto 계산의 기준으로 사용한다. 둘 다 확인할 수 없으면 시스템 물리 메모리 총량을 사용한다. 이 규칙과
`ApplicationHwmProfile`의 비율은 Java 공개 계약과 같다.

## Kotlin source signature

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

inline fun <reified TActor, reified TFactory>
    ZLinkMeshObjectServerBuilder.actorFactory(
        actorType: String,
        noinline configure: ZLinkActorFactoryBuilder<TActor>.() -> Unit,
    ): ZLinkMeshObjectServerBuilder
    where TActor : ZLinkActor,
          TFactory : ZLinkActorFactory
```

Factory configure callback에는 default가 없다. Actor factory builder에는 relocation 동작 선택 외의 설정이 없다. Node placement
[weight](../../../../01-glossary.ko.md#weight)는 0..10000이고 기본값은 100이다. 범위 밖 값은 startup 설정과
runtime 변경에서 configuration error다. Channel weight와 별개이며 runtime update와 descriptor
[snapshot](../../../../01-glossary.ko.md#snapshot)에 같은 값을 사용한다.
RouteMesh Channel Server와 ClientServer Server weight도 같은 범위와 기본값을 사용한다. Weighted
selection은 후보 weight 합계를 최소 64-bit 정수로 계산한다.

MeshNode와 Store-backed fanout publisher의 automatic RID는
`prefix-<lowercase-canonical-uuid-v4>` 형식이다. UUID v4는 `8-4-4-4-12` 자리의 lowercase canonical
문자열로 표현한다. Prefix는 ASCII `[A-Za-z0-9._-]` 1..64자이고 full RID는 UTF-8 255 bytes 이하다.
Active owner와 충돌하면 새 UUID로 다시 시도하지 않고 즉시 `RoutingIdConflict`로 실패한다. Fixed RID는 object role이나 automatic Store [descriptor](../../../../01-glossary.ko.md#descriptor)가
없는 manual topology에서만 사용할 수 있다. Slot count, allocation group과 public allocation provider는 없다.

Object Server의 Entry Spot ID는 같은 prefix의
`<prefix>-entry-<lowercase-canonical-uuid-v4>` 형식이며 MeshNode와 별도로 생성한 UUID v4를 사용한다.
Java `ZLinkMeshNodeDescriptor.entrySpotId()`가 같은 lifecycle의 exact mapping을 제공한다. Global Spot
ID가 active owner와 충돌하면 새 UUID로 다시 시도하지 않고 즉시 `SpotIdConflict`로 startup을
실패시킨다. Caller가 지정한 User·Instance Spot ID가 예약 형식과 일치하면 Store와 factory 전에
startup configuration error로 거부한다.

모든 factory는 Java builder를 Kotlin receiver callback으로 구성한다. Callback은
`disableRelocation()`, `recreateOnRelocation()`, `preserveStateWith(...)` 중 정확히 하나를 호출한다. 누락하거나
둘 이상 호출하면 socket bind 전에 startup configuration error다. Kotlin 전용 policy value와 suspending
adapter는 추가하지 않는다.

Framework는 receiver callback을 등록 호출 안에서 동기적으로 한 번만 실행한다. Callback이 반환된 뒤 보관한
builder를 다시 호출하면 configuration error다. Callback이 예외를 던지면 해당 factory를 등록하지 않고 같은
예외를 호출자에게 전달한다. `stableTypeLimit(...)`을 생략하면 node limit을 공유하며 명시한 값은
1..`Int.MAX_VALUE`여야 한다. 0과 음수는 callback 실행 중 configuration error다.

`recreateOnRelocation()` 또는 `preserveStateWith(...)`를 선택한 factory가 하나라도 있거나 Instance Spot factory가 하나라도 등록된 Object Server는
Java root의 `addRelocationStore(...)`로 Relocation Store를 정확히 하나 등록한다. Instance Spot factory가 없고
모든 factory가 `disableRelocation()`을 선택한 same-node 구성만 이를 생략할 수 있다.

## Exact generated JVM signature

```java
public final class systems.zlink.framework.kotlin.ZLinkCoroutineHandlerOptionsKt {
  public static final void useCoroutineHandlers(systems.zlink.framework.configuration.ZLinkFrameworkOptions, kotlinx.coroutines.CoroutineDispatcher);
  public static final void useCoroutineHandlers(systems.zlink.framework.configuration.ZLinkFrameworkOptions, kotlinx.coroutines.CoroutineScope, kotlinx.coroutines.CoroutineDispatcher);
}
public final class systems.zlink.framework.kotlin.ZLinkDispatchOptionsExtensionsKt {
  public static final systems.zlink.framework.configuration.ZLinkDispatchOptions configureDispatch(systems.zlink.framework.configuration.ZLinkFrameworkOptions, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkDispatchOptions, kotlin.Unit>);
}
public final class systems.zlink.framework.kotlin.ZLinkFrameworkExtensionsKt {
  public static final <TActor extends systems.zlink.framework.actors.ZLinkActor, TFactory extends systems.zlink.framework.actors.ZLinkActorFactory> systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder actorFactory(systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder, java.lang.String, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkActorFactoryBuilder<TActor>, kotlin.Unit>);
  public static final systems.zlink.framework.configuration.ZLinkFrameworkOptions configureStreamCompression(systems.zlink.framework.configuration.ZLinkFrameworkOptions, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder, kotlin.Unit>);
}
```
