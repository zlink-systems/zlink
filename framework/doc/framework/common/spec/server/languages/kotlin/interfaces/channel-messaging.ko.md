# Kotlin Channel messaging 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Java Channel](../../java/interfaces/channel-messaging.ko.md)

Kotlin application은 Java Channel call을 직접 사용하지 않는다. Kotlin 전용 client와 call wrapper가 Java
call을 내부에 보관하며 일반 완료는 `await()`, 현재 Spot turn을 반납하는 완료는 `yield()`로 투영한다.
One-way `await()`의 정상 결과는 `Unit`이고 실패는 Java stage의 exception을 그대로 전달한다.
Typed request의 reified entry method는 wrapper를 만들 때 `KClass<TReply>.java`를 내부 Java
`Class<TReply>`로 고정한다. Application은 terminal에서 reply type을 다시 넘기거나
`.submit().await()`를 작성하지 않는다.

동기 blocking 종결자는 Kotlin 전용 wrapper를 추가하지 않고 Java 표면의 `submit_sync()`를 그대로
노출한다. blocking 호출은 suspend가 아니므로 `await()`로 감싸지 않으며, application thread 전용이고
runtime 실행 문맥에서 부르면 `InvalidOperation`으로 실패한다([Submit과 완료 §4 F2-a](../../../01-execution/01-submit-and-completion.ko.md#4-one-way-submit--admission-경계)).

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

Spot context의 `outbound()`은 `outbound().kotlin()`으로 얻은 Kotlin wrapper를 통해 사용하며, wrapper의
호출·terminal·JVM signature는 [Spot 인터페이스](spots.ko.md)가 소유한다.

Channel Server builder의 handler 등록은 [Kotlin 구성](configuration-host.ko.md)의 등록 규칙을 따른다.
아래 source signature와 JVM signature가 그 확장이다.

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

interface ZLinkKotlinRequestCall<TReply : Any> {
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

public inline fun <reified T : Any> messageOf(value: T): ZLinkMessage
public fun messageOf(value: Any, declaredType: KClass<*>): ZLinkMessage
public inline fun <reified T> ZLinkMessage.decode(): T
```

첫 번째 `messageOf(...)` overload는 호출 지점의 `T`를 message의 선언 type으로 보존한다. Runtime
instance가 subtype이더라도 codec selector는 이 선언 type을 사용한다. Java reflection이나 공통 base
type처럼 호출 지점에서 `T`를 유지할 수 없으면 두 번째 overload에 선언 type을 명시한다.

받은 message에서 첫 `decode<T>()`가 값 또는 실패를 확정한다. 이후 호출은 Java
`ZLinkMessage.decode(Class<T>)`와 같은 단일 결과를 사용하며 serializer를 다시 호출하지 않는다.
다른 `T`가 첫 값과 맞지 않으면 `TYPE_MISMATCH`로 끝나고, 첫 호출이 실패했다면 그 실패를
다시 전달한다.

`ZLinkKotlinRequestCall.yield()`는 Java call의 coroutine bridge다. 유효 문맥과 수락은 [Execution gate](../../../01-execution/02-handler-turn-and-execution-gate.ko.md)가 정한다.

One-way 수락과 terminal 결과는 [Submit과 completion](../../../01-execution/01-submit-and-completion.ko.md)이 정한다. Kotlin은 취소를 coroutine cancellation으로 표현한다.

Topic을 받는 `publishToTopic(...)`에 전달하거나 Java builder의 `subscribe`에 등록하는 topic이
[Channel messaging §7](../../../02-channel-transport/02-channel-messaging.ko.md#7-classic-fanout과의-경계liveness-beacon-topic-예약)이 금지한 값이면 Java runtime의 `ZLinkConfigurationException`을 발생시킨다.
[Topic](../../../00-foundation/02-glossary.ko.md#topic)을 생략한 overload는 typed
event의 packet name을 topic으로 사용하며 같은 규칙을 적용한다.

RouteMesh DSL은 Java builder의 의미를 바꾸지 않고 receiver와 lambda만 제공한다. MeshNode 하나의 physical
connection 위에 [ChannelName](../../../00-foundation/02-glossary.ko.md#channelname)별 role을 구성한다.

Kotlin runtime은 Java의 `ZLinkRouteMeshRuntimeOptions`를 직접 사용한다.
`channel(meshName, channelName)`은 대상 Mesh와 ChannelName을 함께 지정하고,
`mesh(meshName)`은 placement option을 선택한다. `channel(channelName)`은 한 Mesh에서만
등록된 ChannelName을 선택한다. Kotlin DSL의 `routeMesh`와 `channelName`은 이 runtime option에
새 overload를 추가하지 않는다. 따라서 이 세 method는 Java 언어별 interface와 Kotlin
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

fun ZLinkMeshNodeBuilder.channelName(
 channelName: String,
 configure: ZLinkMeshChannelBuilder.() -> Unit = {},
): ZLinkMeshChannelBuilder

fun ZLinkMeshPeerConnections.connect(
 expectedRoutingId: RoutingId,
 endpoint: String,
)

inline fun <reified THandler : Any, reified TMessage : Any>
 ZLinkMeshNodeBuilder.addRouteSendHandler(): ZLinkMeshNodeBuilder

inline fun <reified THandler : Any, reified TRequest : Any, reified TReply : Any>
 ZLinkMeshNodeBuilder.addRouteRequestHandler(): ZLinkMeshNodeBuilder

inline fun <reified THandler : ZLinkSendHandler<TMessage>, reified TMessage : Any>
 ZLinkMeshChannelServerBuilder.addSendHandler(): ZLinkMeshChannelServerBuilder

inline fun <reified THandler : ZLinkRouteSendHandler<TMessage>, reified TMessage : Any>
 ZLinkMeshChannelServerBuilder.addRouteSendHandler(): ZLinkMeshChannelServerBuilder

inline fun <reified THandler : ZLinkRequestHandler<TRequest, TReply>, reified TRequest : Any, reified TReply : Any>
 ZLinkMeshChannelServerBuilder.addRequestHandler(): ZLinkMeshChannelServerBuilder

inline fun <reified THandler : ZLinkSendHandler<TMessage>, reified TMessage : Any>
 ZLinkClientServerChannelServerBuilder.addSendHandler(): ZLinkClientServerChannelServerBuilder

inline fun <reified THandler : ZLinkRequestHandler<TRequest, TReply>, reified TRequest : Any, reified TReply : Any>
 ZLinkClientServerChannelServerBuilder.addRequestHandler(): ZLinkClientServerChannelServerBuilder
```

```kotlin
val reply = routeClient
 .requestToChannel<InventoryReply>("inventory", request)
 .await()
```

## generated JVM signature

아래 JVM signature는 Kotlin source contract의 generated form이다.

```java
public final class systems.zlink.framework.kotlin.ZLinkMessageExtensionsKt {
 public static final <T> systems.zlink.framework.messaging.ZLinkMessage messageOf(T);
 public static final systems.zlink.framework.messaging.ZLinkMessage messageOf(java.lang.Object, kotlin.reflect.KClass<?>);
 public static final <T> T decode(systems.zlink.framework.messaging.ZLinkMessage);
}
public final class systems.zlink.framework.kotlin.ZLinkRouteMeshExtensionsKt {
 public static final systems.zlink.framework.configuration.ZLinkMeshNodeBuilder routeMesh(systems.zlink.framework.configuration.ZLinkFrameworkOptions, java.lang.String, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkMeshNodeBuilder, kotlin.Unit>);
 public static final systems.zlink.framework.configuration.ZLinkMeshChannelBuilder channelName(systems.zlink.framework.configuration.ZLinkMeshNodeBuilder, java.lang.String, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkMeshChannelBuilder, kotlin.Unit>);
 public static systems.zlink.framework.configuration.ZLinkMeshChannelBuilder channelName$default(systems.zlink.framework.configuration.ZLinkMeshNodeBuilder, java.lang.String, kotlin.jvm.functions.Function1, int, java.lang.Object);
 public static final void connect(systems.zlink.framework.configuration.ZLinkMeshPeerConnections, systems.zlink.contracts.core.RoutingId, java.lang.String);
}
public final class systems.zlink.framework.kotlin.ZLinkFrameworkExtensionsKt {
 public static final <THandler, TMessage> systems.zlink.framework.configuration.ZLinkMeshNodeBuilder addRouteSendHandler(systems.zlink.framework.configuration.ZLinkMeshNodeBuilder);
 public static final <THandler, TRequest, TReply> systems.zlink.framework.configuration.ZLinkMeshNodeBuilder addRouteRequestHandler(systems.zlink.framework.configuration.ZLinkMeshNodeBuilder);
 public static final <THandler extends systems.zlink.framework.channels.ZLinkSendHandler<TMessage>, TMessage> systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder addSendHandler(systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder);
 public static final <THandler extends systems.zlink.framework.channels.ZLinkRouteSendHandler<TMessage>, TMessage> systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder addRouteSendHandler(systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder);
 public static final <THandler extends systems.zlink.framework.channels.ZLinkRequestHandler<TRequest, TReply>, TRequest, TReply> systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder addRequestHandler(systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder);
 public static final <THandler extends systems.zlink.framework.channels.ZLinkSendHandler<TMessage>, TMessage> systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder addSendHandler(systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder);
 public static final <THandler extends systems.zlink.framework.channels.ZLinkRequestHandler<TRequest, TReply>, TRequest, TReply> systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder addRequestHandler(systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder);
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
 public abstract systems.zlink.framework.kotlin.ZLinkKotlinRequestCall<TReply> timeout(java.time.Duration);
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
