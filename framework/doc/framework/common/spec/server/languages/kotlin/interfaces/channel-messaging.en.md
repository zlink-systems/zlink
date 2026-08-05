# Kotlin Channel Messaging Public Interface

[Interface table of contents](README.en.md) · [Java Channel](../../java/interfaces/channel-messaging.en.md)

A Kotlin application doesn't directly use the Java Channel call. A
Kotlin-only client and call wrapper keep the Java call internally,
projecting regular completion as `await()`, and completion that returns
the current Spot turn as `yield()`. One-way `await()`'s normal result is
`Unit`, and failure propagates the Java stage's exception unchanged. The
typed request's reified entry method fixes `KClass<TReply>.java` as the
internal Java `Class<TReply>` when building the wrapper. The application
doesn't pass the reply type again at the terminal, or write
`.submit().await()`.

The Kotlin filter uses the same applicable scope as the Java runtime.
It's applied to RouteMesh/ClientServer Channel send/request, Node direct
send/request, and classic fanout subscription handlers, and isn't
applied to Spot/Actor/Logical Multicast/STREAM handlers.
`ZLinkHandlerFilterContext.dispatchKind()` distinguishes the five paths.
RouteMesh and Node direct provide MeshName, and ClientServer and classic
fanout don't.

The handler and filter are each created once per dispatch and use the
same scoped dependency — application DI registration can't change this
lifetime. Calling `next.invoke()` twice is `IllegalStateException`, and
not calling it on a request is `REJECTED`. Even if the filter returns an
arbitrary value, it doesn't substitute for the handler reply. Coroutine
suspension doesn't extend the dispatch scope beyond terminal completion.

A Spot direct send/request isn't reduced to a Channel call. Since a
Kotlin-only Spot wrapper configures `instanceSpot` and `inMesh` before
terminal `await()`/`yield()`, it keeps the fluent state of Missing
Instance cold activation. Kotlin's Spot-dedicated wrapper and exact JVM
signature are owned by the [Spot Interface](spots.en.md).

## Kotlin Source Signature

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

`ZLinkKotlinRequestCall.yield()` is only a coroutine bridge for Java's
`yield(...)` — it doesn't turn an arbitrary suspension into Yield. If
it's not a `SPOT_WIDE` User Spot or Instance Spot application handler,
it completes with `InvalidOperation` before suspending the coroutine or
submitting the underlying operation. The same rule applies to a Node
direct request, Entry/`PER_ACTOR`, Channel handler, and outside the
owner context. A regular `await()` that waits on a target needing the
current Spot gate is also rejected before submission. The one-way
wrapper keeps FIFO queue admission and doesn't call the handler inline or
reentrantly.

If the queue is full, it waits until the send timeout. Timeout completes
with `DeadlineExceeded`, a route break with `Unavailable`, and runtime
shutdown with `ShuttingDown`. Absence of target or session binding is
`NotFound`. If cancellation is triggered first, it completes as coroutine
cancellation.

Passing the internal liveness-dedicated exact byte `01 5A 4C 46 31` to
`publishToTopic(...)`, which takes a topic, raises the Java runtime's
`ZLinkConfigurationException` without starting transport. The overload
that omits [topic](../../../../01-glossary.en.md#topic) uses the typed
event's packet name, so it doesn't create this internal topic.

The RouteMesh DSL doesn't change the Java builder's meaning — it only
provides a receiver and lambda. It configures a per-[ChannelName](../../../../01-glossary.en.md#channelname)
role on one MeshNode's physical connection.

When the Kotlin runtime directly uses Java's
`ZLinkRouteMeshRuntimeOptions`, it projects the four public selections
unchanged. `meshNode(meshName)` and `channel(meshName, channelName)`
specify the target Mesh, and `mesh(meshName)` and `channel(channelName)`
use the Mesh of the current runtime context. The Kotlin DSL's
`routeMesh` and `channel` don't add a new overload to this runtime
option. So these four methods must be confirmed with the same name,
arguments, and return type in both the Java exact interface and the
Kotlin package consumer.

RouteMesh Channel Server and ClientServer Server weight use the Java
builder's signed `int`. The allowed range is `0..10000`, defaulting to
`100`, and 0 is excluded from new target selection. Logical Multicast
includes each positive member exactly once, and doesn't increase submit
count by weight magnitude. An out-of-range startup/runtime setting is a
configuration error. Logical Multicast's remote target is submitted
once to the local transport queue of the MeshNode route fixed on the
source, and the local target is submitted once to the matching local
Spot queue. Per-target success/drop/unreachable results aren't returned
as `await()`'s result or aggregated into public monitoring. Remote Spot
queue admission and remote/local handler execution or completion aren't
the coroutine bridge's completion condition.

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

## Exact Generated JVM Signature

The JVM signature below is the generated form of the Kotlin source
contract.

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
