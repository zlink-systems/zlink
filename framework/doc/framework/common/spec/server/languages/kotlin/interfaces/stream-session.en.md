# Kotlin STREAM Session Public Interface

[Interface table of contents](README.en.md) · [Java STREAM Session](../../java/interfaces/stream-session.en.md) ·
[Session Actor Dispatch](../../../../20-session-actor-dispatch.en.md)

The Kotlin session lifecycle and coroutine handler use the Java session
contract unchanged. The builder member that turns on Actor dispatch is
`enableActorDispatch()`, which takes no MeshName argument. Startup
requires a Mesh whose object role is Client or Server, and a Location
Store. The global ActorId determines current authority and Mesh.

Session bind takes an exact `ActorRef` once. There's no bind overload
taking only a local Actor instance or ActorId. If there's no current
mapping at bind, `NotFound`; if the generation differs, `InvalidOperation`;
if in the pre-commit seal window, `Unavailable`. The framework doesn't
perform hidden retry or a local fallback.

Session send/reply, bound session send, and Session Actor relay return a
Kotlin one-way wrapper. The application only waits for local STREAM
queue admission with `await(): Unit`, and doesn't directly use Java's
`CompletionStage` and submission result type. If the queue is full, it
waits until the send timeout, and timeout, cancellation, route break,
and runtime shutdown complete with an exception.

Java's `ZLinkSessionActor.notifyDisconnected()` is used as a logical
notification while the connection is kept. After bind, relay/disconnect
use the per-Actor stored route and don't query the Location Store per
message. A physical disconnect has the framework perform an automatic
all-settled notification to every current binding, running the Spot
callback at most once per exact binding identity. A relocation route
update is only allowed on the same ObjectGeneration. After the target
Actor is restored and starts message processing, the target runtime
sends `sessionActorLocationUpdateReqMsg` and changes that Actor's route
and the bound-session's current `ActorRef` location snapshot Java's
`ZLinkSessionActor.ref()` returns, together. The snapshot reflects the
same ActorId/ObjectGeneration and the target MeshName/NodeRid. Even
without a response, target Actor processing doesn't stop, and the same
request is resent at a fixed interval. The route and physical STREAM
connection of a different Actor on the same Session that isn't included
in the relocation target are kept. The application doesn't rebind to
learn about relocation.

## STREAM Socket Message Size

Kotlin uses Java's `configureSocket().setMaxMessageSize(...)` contract
unchanged. The default is `64 KiB`, and it applies only to complete
client-to-server messages received by a StreamNode through Core STREAM. The
size is header bytes plus payload bytes, excluding the 6-byte prefix. `0`
means no Framework limit and a negative value is a startup error. An
over-limit message isn't delivered to the handler; the server records
`EMSGSIZE` and closes the connection. The Framework limit doesn't apply to
server-to-client outbound messages.

## Kotlin Source Signature

```kotlin
interface ZLinkSuspendingTypedSessionPacketHandler<
    TSessionContext : ZLinkSessionContext,
    TMessage : Any,
> {
    fun packetName(): String
    fun messageType(): Class<TMessage>
    suspend fun handle(
        context: TSessionContext,
        dispatch: ZLinkSessionDispatchContext,
        message: TMessage,
    )
}

abstract class ZLinkSuspendingSession : ZLinkSession {
    abstract override fun context(): ZLinkSessionContext
    protected open suspend fun onConnectedSuspending()
    protected open suspend fun onDisconnectedSuspending()
    protected open suspend fun onErrorSuspending(error: ZLinkStreamError)
    protected open suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    )
}

suspend fun ZLinkSessionActors.bindOrGetActor(
    actor: ActorRef,
): ZLinkSessionActor

interface ZLinkKotlinSessionSendCall {
    fun metadata(key: String, value: String): ZLinkKotlinSessionSendCall
    fun compress(): ZLinkKotlinSessionSendCall
    suspend fun await()
}

interface ZLinkKotlinSessionReplyCall {
    fun compress(): ZLinkKotlinSessionReplyCall
    suspend fun await()
}

interface ZLinkKotlinSessionClient {
    fun send(message: Any): ZLinkKotlinSessionSendCall
    fun reply(message: Any): ZLinkKotlinSessionReplyCall
}

interface ZLinkKotlinSessionActor {
    fun relay(message: ZLinkMessage): ZLinkKotlinSubmissionCall
    fun relay(
        dispatch: ZLinkSessionDispatchContext,
        message: ZLinkMessage,
    ): ZLinkKotlinSubmissionCall
}

interface ZLinkKotlinBoundSession {
    fun send(message: Any): ZLinkKotlinMessageSendCall
}
```

## Exact Generated JVM Signature

```java
public interface systems.zlink.framework.kotlin.ZLinkSuspendingTypedSessionPacketHandler<TSessionContext extends systems.zlink.framework.streams.ZLinkSessionContext, TMessage> {
  public abstract java.lang.String packetName();
  public abstract java.lang.Class<TMessage> messageType();
  public abstract java.lang.Object handle(TSessionContext, systems.zlink.framework.streams.ZLinkSessionDispatchContext, TMessage, kotlin.coroutines.Continuation<? super kotlin.Unit>);
}
public abstract class systems.zlink.framework.kotlin.ZLinkSuspendingSession implements systems.zlink.framework.streams.ZLinkSession {
  public systems.zlink.framework.kotlin.ZLinkSuspendingSession();
  public abstract systems.zlink.framework.streams.ZLinkSessionContext context();
  public final java.util.concurrent.CompletionStage<java.lang.Void> onConnected();
  public final java.util.concurrent.CompletionStage<java.lang.Void> onDisconnected();
  public final java.util.concurrent.CompletionStage<java.lang.Void> onError(systems.zlink.framework.streams.ZLinkStreamError);
  public final java.util.concurrent.CompletionStage<java.lang.Void> onDispatch(systems.zlink.framework.streams.ZLinkSessionDispatchContext, systems.zlink.framework.messaging.ZLinkMessage);
}
public final class systems.zlink.framework.kotlin.ZLinkFrameworkExtensionsKt {
  public static final java.lang.Object bindOrGetActor(systems.zlink.framework.streams.ZLinkSessionActors, systems.zlink.framework.actors.ActorRef, kotlin.coroutines.Continuation<? super systems.zlink.framework.streams.ZLinkSessionActor>);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinSessionSendCall {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinSessionSendCall metadata(java.lang.String, java.lang.String);
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinSessionSendCall compress();
  public abstract java.lang.Object await(kotlin.coroutines.Continuation<? super kotlin.Unit>);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinSessionReplyCall {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinSessionReplyCall compress();
  public abstract java.lang.Object await(kotlin.coroutines.Continuation<? super kotlin.Unit>);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinSessionClient {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinSessionSendCall send(java.lang.Object);
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinSessionReplyCall reply(java.lang.Object);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinSessionActor {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinSubmissionCall relay(systems.zlink.framework.messaging.ZLinkMessage);
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinSubmissionCall relay(systems.zlink.framework.streams.ZLinkSessionDispatchContext, systems.zlink.framework.messaging.ZLinkMessage);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinBoundSession {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinMessageSendCall send(java.lang.Object);
}
```
