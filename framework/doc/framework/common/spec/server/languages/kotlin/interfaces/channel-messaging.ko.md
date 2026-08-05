# Kotlin Channel messaging 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Java Channel](../../java/interfaces/channel-messaging.ko.md)

Kotlin application은 Java Channel call을 직접 사용하지 않는다. Kotlin 전용 client와 call wrapper가 Java
call을 내부에 보관하며 일반 완료는 `await()`, 현재 Spot turn을 반납하는 완료는 `yield()`로 투영한다.
One-way `await()`의 정상 결과는 `Unit`이고 실패는 Java stage의 exception을 그대로 전달한다.
Typed request의 reified entry method는 wrapper를 만들 때 `KClass<TReply>.java`를 내부 Java
`Class<TReply>`로 고정한다. Application은 terminal에서 reply type을 다시 넘기거나
`.submit().await()`를 작성하지 않는다.

Kotlin filter는 Java runtime과 같은 적용 범위를 사용한다. RouteMesh·ClientServer
Channel send/request, Node direct send/request와 classic fanout 구독 handler에는
적용하고 Spot·Actor·Logical Multicast·STREAM handler에는 적용하지 않는다.
`ZLinkHandlerFilterContext.dispatchKind()`가 다섯 경로를 구분한다. RouteMesh와 Node
direct는 MeshName을 제공하고 ClientServer와 classic fanout은 제공하지 않는다.

Handler와 filter는 dispatch마다 한 번씩 만들어 같은 scoped dependency를 사용하며,
Application DI 등록으로 이 수명을 바꿀 수 없다. `next.invoke()`를 두 번 호출하면
`IllegalStateException`이고, request에서 호출하지 않으면 `REJECTED`다. Filter가
임의 값을 반환해도 handler reply를 대체하지 않는다. Coroutine suspension은 dispatch
scope를 terminal completion 뒤까지 연장하지 않는다.

Spot direct send/request는 Channel call로 축소하지 않는다. Kotlin 전용 Spot wrapper가
`instanceSpot`과 `inMesh`를 terminal `await()`·`yield()` 전에 구성하므로 Missing Instance cold
activation의 fluent state를 유지한다. Kotlin의 Spot 전용 wrapper와 exact JVM signature는
[Spot 인터페이스](spots.ko.md)가 소유한다.

## Kotlin source signature

```kotlin
interface ZLinkSuspendingRequestHandler<TRequest, TReply> {
    suspend fun handle(request: TRequest, context: ZLinkMessageContext): TReply
}

interface ZLinkSuspendingSendHandler<TMessage> {
    suspend fun handle(message: TMessage, context: ZLinkMessageContext)
}

interface ZLinkSuspendingPublishHandler<TMessage> {
    suspend fun handle(message: TMessage, context: ZLinkPublishMessageContext)
}

interface ZLinkSuspendingRouteRequestHandler<TRequest, TReply> {
    suspend fun handle(request: TRequest, context: ZLinkRouteMessageContext): TReply
}

interface ZLinkSuspendingRouteSendHandler<TMessage> {
    suspend fun handle(message: TMessage, context: ZLinkRouteMessageContext)
}

interface ZLinkKotlinMessageSendCall {
    fun metadata(key: String, value: String): ZLinkKotlinMessageSendCall
    suspend fun await()
}

interface ZLinkKotlinSubmissionCall {
    suspend fun await()
}

interface ZLinkKotlinRequestCall<TReply> {
    fun metadata(key: String, value: String): ZLinkKotlinRequestCall<TReply>
    fun timeout(timeout: Duration): ZLinkKotlinRequestCall<TReply>
    suspend fun await(): TReply
    suspend fun yield(): TReply
}

interface ZLinkKotlinClient {
    fun sendToChannel(
        channelName: String,
        message: Any,
    ): ZLinkKotlinMessageSendCall

    fun <TReply : Any> requestToChannel(
        channelName: String,
        request: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply>
}

inline fun <reified TReply : Any> ZLinkKotlinClient.requestToChannel(
    channelName: String,
    request: Any,
): ZLinkKotlinRequestCall<TReply> =
    requestToChannel(channelName, request, TReply::class)

interface ZLinkKotlinFanoutClient {
    fun publish(
        channelName: String,
        topic: String,
        event: Any,
    ): ZLinkKotlinSubmissionCall
    fun publish(
        channelName: String,
        event: Any,
    ): ZLinkKotlinSubmissionCall
}

interface ZLinkKotlinRouteClient {
    fun sendToNode(
        meshName: String,
        target: RoutingId,
        message: Any,
    ): ZLinkKotlinMessageSendCall

    fun <TReply : Any> requestToNode(
        meshName: String,
        target: RoutingId,
        request: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply>

    fun sendToChannel(
        channelName: String,
        message: Any,
    ): ZLinkKotlinMessageSendCall

    fun <TReply : Any> requestToChannel(
        channelName: String,
        request: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply>
}

inline fun <reified TReply : Any> ZLinkKotlinRouteClient.requestToNode(
    meshName: String,
    target: RoutingId,
    request: Any,
): ZLinkKotlinRequestCall<TReply> =
    requestToNode(meshName, target, request, TReply::class)

inline fun <reified TReply : Any> ZLinkKotlinRouteClient.requestToChannel(
    channelName: String,
    request: Any,
): ZLinkKotlinRequestCall<TReply> =
    requestToChannel(channelName, request, TReply::class)

public fun messageOf(value: Any): ZLinkMessage
public inline fun <reified T> ZLinkMessage.decode(): T
```

`ZLinkKotlinRequestCall.yield()`는 Java `yield(...)`의 coroutine bridge일 뿐 임의 suspension을 Yield로
바꾸지 않는다.
`SPOT_WIDE` User Spot 또는 Instance Spot application handler가 아니면 coroutine을 suspend하거나 underlying
operation을 제출하기 전에 `InvalidOperation`으로 완료한다. Node direct request, Entry·`PER_ACTOR`,
Channel handler와 owner context 밖에도 같은 규칙을 적용한다. 현재 Spot gate가 필요한 target을 기다리는
일반 `await()`도 submission 전에 거부한다. One-way wrapper는 FIFO queue admission을 유지하고 handler를
inline 또는 reentrant하게 호출하지 않는다.

Queue가 가득 차면 send timeout까지 기다린다. Timeout은 `DeadlineExceeded`, route 단절은
`Unavailable`, runtime 종료는 `ShuttingDown`으로 완료한다. Target이나 session binding이 없으면
`NotFound`다. Cancellation이 먼저 확정되면 coroutine cancellation로 완료한다.

Topic을 받는 `publishToTopic(...)`에 내부 liveness용 exact byte `01 5A 4C 46 31`을 전달하면 transport를
시작하지 않고 Java runtime의 `ZLinkConfigurationException`을 발생시킨다.
[Topic](../../../../01-glossary.ko.md#topic)을 생략한 overload는 typed
event의 packet name을 사용하므로 이 내부 topic을 만들지 않는다.

RouteMesh DSL은 Java builder의 의미를 바꾸지 않고 receiver와 lambda만 제공한다. MeshNode 하나의 physical
connection 위에 [ChannelName](../../../../01-glossary.ko.md#channelname)별 role을 구성한다.

Kotlin runtime이 Java의 `ZLinkRouteMeshRuntimeOptions`를 직접 사용할 때는 네 가지 public
선택을 그대로 투영한다. `meshNode(meshName)`과 `channel(meshName, channelName)`은 대상
Mesh를 명시하고, `mesh(meshName)`과 `channel(channelName)`은 현재 runtime context의
Mesh를 사용한다. Kotlin DSL의 `routeMesh`와 `channel`은 이 runtime option에 새로운
overload를 추가하지 않는다. 따라서 이 네 method는 Java exact interface와 Kotlin
package consumer에서 같은 이름·인자·반환 type으로 확인해야 한다.

RouteMesh Channel Server와 ClientServer Server weight는 Java builder의 signed `int`를 사용한다. 허용 범위는
`0..10000`, 기본값은 `100`이며 0은 새 target 선택에서 제외한다. Logical Multicast는 positive member를
각각 한 번만 포함하고 weight 크기로 제출 횟수를 늘리지 않는다. 범위 밖 startup·runtime 설정은
configuration error다.
Logical Multicast의 remote target은 source에서 고정한 MeshNode route의 local transport queue에 한 번씩
제출하고 local target은 일치하는 local Spot queue에 한 번씩 제출한다. Target별 성공·drop·unreachable
결과는 `await()`의 결과로 반환하거나 public monitoring에 집계하지 않는다. Remote Spot queue 수락과
remote·local handler 실행 또는 완료는 coroutine bridge 완료 조건이 아니다.

```kotlin
fun ZLinkFrameworkOptions.routeMesh(
    meshName: String,
    configure: ZLinkMeshNodeBuilder.() -> Unit,
): ZLinkMeshNodeBuilder

fun ZLinkMeshNodeBuilder.channel(
    channelName: String,
    configure: ZLinkMeshChannelBuilder.() -> Unit = {},
): ZLinkMeshChannelBuilder

fun ZLinkMeshPeerConnections.connect(
    expectedRoutingId: RoutingId,
    endpoint: String,
)
```

```kotlin
val reply = routeClient
    .requestToChannel<InventoryReply>("inventory", request)
    .await()
```

## Exact generated JVM signature

아래 JVM signature는 Kotlin source contract의 generated form이다.

```java
public final class systems.zlink.framework.kotlin.ZLinkMessageExtensionsKt {
  public static final systems.zlink.framework.messaging.ZLinkMessage messageOf(java.lang.Object);
  public static final <T> T decode(systems.zlink.framework.messaging.ZLinkMessage);
}
public final class systems.zlink.framework.kotlin.ZLinkRouteMeshExtensionsKt {
  public static final systems.zlink.framework.configuration.ZLinkMeshNodeBuilder routeMesh(systems.zlink.framework.configuration.ZLinkFrameworkOptions, java.lang.String, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkMeshNodeBuilder, kotlin.Unit>);
  public static final systems.zlink.framework.configuration.ZLinkMeshChannelBuilder channel(systems.zlink.framework.configuration.ZLinkMeshNodeBuilder, java.lang.String, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkMeshChannelBuilder, kotlin.Unit>);
  public static systems.zlink.framework.configuration.ZLinkMeshChannelBuilder channel$default(systems.zlink.framework.configuration.ZLinkMeshNodeBuilder, java.lang.String, kotlin.jvm.functions.Function1, int, java.lang.Object);
  public static final void connect(systems.zlink.framework.configuration.ZLinkMeshPeerConnections, systems.zlink.contracts.core.RoutingId, java.lang.String);
}
public final class systems.zlink.framework.kotlin.ZLinkSuspendingHandlersKt {
}
public interface systems.zlink.framework.kotlin.ZLinkSuspendingPublishHandler<TMessage> {
  public abstract java.lang.Object handle(TMessage, systems.zlink.framework.channels.ZLinkPublishMessageContext, kotlin.coroutines.Continuation<? super kotlin.Unit>);
}
public interface systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler<TRequest, TReply> {
  public abstract java.lang.Object handle(TRequest, systems.zlink.framework.ZLinkMessageContext, kotlin.coroutines.Continuation<? super TReply>);
}
public interface systems.zlink.framework.kotlin.ZLinkSuspendingRouteRequestHandler<TRequest, TReply> {
  public abstract java.lang.Object handle(TRequest, systems.zlink.framework.channels.ZLinkRouteMessageContext, kotlin.coroutines.Continuation<? super TReply>);
}
public interface systems.zlink.framework.kotlin.ZLinkSuspendingRouteSendHandler<TMessage> {
  public abstract java.lang.Object handle(TMessage, systems.zlink.framework.channels.ZLinkRouteMessageContext, kotlin.coroutines.Continuation<? super kotlin.Unit>);
}
public interface systems.zlink.framework.kotlin.ZLinkSuspendingSendHandler<TMessage> {
  public abstract java.lang.Object handle(TMessage, systems.zlink.framework.ZLinkMessageContext, kotlin.coroutines.Continuation<? super kotlin.Unit>);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinMessageSendCall {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinMessageSendCall metadata(java.lang.String, java.lang.String);
  public abstract java.lang.Object await(kotlin.coroutines.Continuation<? super kotlin.Unit>);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinSubmissionCall {
  public abstract java.lang.Object await(kotlin.coroutines.Continuation<? super kotlin.Unit>);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinRequestCall<TReply> {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinRequestCall<TReply> metadata(java.lang.String, java.lang.String);
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinRequestCall<TReply> timeout-LRDsOJo(long);
  public abstract java.lang.Object await(kotlin.coroutines.Continuation<? super TReply>);
  public abstract java.lang.Object yield(kotlin.coroutines.Continuation<? super TReply>);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinClient {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinMessageSendCall sendToChannel(java.lang.String, java.lang.Object);
  public abstract <TReply> systems.zlink.framework.kotlin.ZLinkKotlinRequestCall<TReply> requestToChannel(java.lang.String, java.lang.Object, kotlin.reflect.KClass<TReply>);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinFanoutClient {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinSubmissionCall publish(java.lang.String, java.lang.String, java.lang.Object);
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinSubmissionCall publish(java.lang.String, java.lang.Object);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinRouteClient {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinMessageSendCall sendToNode(java.lang.String, systems.zlink.contracts.core.RoutingId, java.lang.Object);
  public abstract <TReply> systems.zlink.framework.kotlin.ZLinkKotlinRequestCall<TReply> requestToNode(java.lang.String, systems.zlink.contracts.core.RoutingId, java.lang.Object, kotlin.reflect.KClass<TReply>);
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinMessageSendCall sendToChannel(java.lang.String, java.lang.Object);
  public abstract <TReply> systems.zlink.framework.kotlin.ZLinkKotlinRequestCall<TReply> requestToChannel(java.lang.String, java.lang.Object, kotlin.reflect.KClass<TReply>);
}
```
