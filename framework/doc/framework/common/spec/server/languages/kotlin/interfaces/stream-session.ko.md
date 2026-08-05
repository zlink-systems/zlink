# Kotlin STREAM session 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Java STREAM session](../../java/interfaces/stream-session.ko.md) ·
[session Actor dispatch](../../../../20-session-actor-dispatch.ko.md)

Kotlin session lifecycle과 coroutine handler는 Java session 계약을 그대로 사용한다. Actor dispatch를 켜는
builder member는 `enableActorDispatch()`이며 MeshName 인자를 받지 않는다. Startup에는 object role이 Client
또는 Server인 Mesh와 Location Store가 필요하다. Global ActorId가 current authority와 Mesh를 결정한다.

Session bind는 exact `ActorRef`를 한 번 받는다. Local Actor instance나 ActorId만 받는 bind overload는 없다.
Bind 시 current mapping이 없으면 `NotFound`, generation이 다르면 `InvalidOperation`, pre-commit
seal 구간이면 `Unavailable`이다. Framework는 hidden retry나 local fallback을 수행하지 않는다.

Session send·reply, bound session send와 Session Actor relay는 Kotlin one-way wrapper를 반환한다. Application은
`await(): Unit`으로 local STREAM queue admission만 기다리며 Java `CompletionStage`와 submission result type을
직접 사용하지 않는다. Queue가 가득 차면 send timeout까지 기다리고 timeout, cancellation, route 단절과
runtime 종료는 exception으로 완료한다.

Java `ZLinkSessionActor.notifyDisconnected()`는 connection이 유지된 상태의 logical notification으로
사용한다. Bind 뒤 relay·disconnect는 Actor별 저장 route를 사용하며 message마다 Location Store를 조회하지
않는다. Physical disconnect는 Framework가 current binding 전체에 automatic all-settled 통지를 수행하고
exact binding identity마다 Spot callback을 최대 한 번 실행한다. Relocation route update는 같은
ObjectGeneration에만 허용한다. Target Actor가 복원되어 message 처리를 시작한 뒤 target
runtime이 `sessionActorLocationUpdateReqMsg`를 send하여 해당 Actor route와 Java
`ZLinkSessionActor.ref()`가 반환하는 bound-session의 current `ActorRef` location snapshot을
함께 바꾼다. Snapshot은 같은 ActorId·ObjectGeneration과 target MeshName·NodeRid를 반영한다.
응답이 없어도 Target Actor 처리를 멈추지 않으며 정해진 간격으로 같은 요청을 다시 보낸다. 같은 Session에서 relocation 대상에
포함되지 않은 다른 Actor의 route와 physical STREAM connection은 유지한다. Application은 relocation을 알기 위해 rebind하지 않는다.

## STREAM socket message 크기

Kotlin은 Java `configureSocket().setMaxMessageSize(...)` 계약을 그대로 사용한다. 기본값은
`64 KiB`이며 StreamNode의 Core STREAM inbound에서 client→server complete message에만 적용한다.
크기는 6-byte prefix를 제외한 header와 payload의 합이다. `0`은 상한을 사용하지 않는 값이고
음수는 startup error다. 상한 초과 message는 handler에 전달하지 않고 server가 `EMSGSIZE`를 기록한
뒤 연결을 종료한다. server→client outbound에는 이 Framework 상한을 적용하지 않는다.

## Kotlin source signature

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

## Exact generated JVM signature

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
