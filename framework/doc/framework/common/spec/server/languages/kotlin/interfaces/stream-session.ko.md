# Kotlin STREAM session 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Java STREAM session](../../java/interfaces/stream-session.ko.md) ·
[session Actor dispatch](../../../20-session-actor-dispatch.ko.md)

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
ObjectGeneration에만 허용한다. Logical notification도 exact binding callback을 최대 한 번 실행하고 terminal
뒤 binding을 tombstone으로 확정하여 제거한다. Physical STREAM connection과 Actor·Spot membership은
유지하며 새 public Unbind API는 제공하지 않는다. Rebind는 새 identity를 current로 등록한 즉시 완료되며
이전 session의 처리를 기다리지 않는다. 이전 exact session의 `onActorBindingReplacedSuspending(...)`에서
client 안내를 보낼 수 있다. Callback이 성공 또는 실패로 terminal이 되면 Framework가 `100 ms` 뒤 connection을 닫는다.
Callback이나 close 실패는 새 binding을 제거하거나 이전 binding을 복원하지 않는다. 같은 generation의
relocation route update는 rebind가 아니므로 disconnect callback을
실행하지 않는다. Target Actor가 복원되어 message 처리를 시작한 뒤 target
runtime이 `sessionActorLocationUpdateReqMsg`를 send하여 해당 Actor route와 Java
`ZLinkSessionActor.ref()`가 반환하는 bound-session의 current `ActorRef` location snapshot을
함께 바꾼다. Snapshot은 같은 ActorId·ObjectGeneration과 target MeshName·NodeRid를 반영한다.
응답이 없어도 Target Actor 처리를 멈추지 않으며 정해진 간격으로 같은 요청을 다시 보낸다. 같은 Session에서 relocation 대상에
포함되지 않은 다른 Actor의 route와 physical STREAM connection은 유지한다. Application은 relocation을 알기 위해 rebind하지 않는다.

| 구현 차이 | 현재 상태 |
|---|---|
| Session Actor binding 교체 | 없음. Kotlin bridge는 suspending callback(`onActorBindingReplacedSuspending`)을 연결하며, command 51 codec과 non-blocking 100 ms close timer는 JVM runtime이 제공한다. |

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
    protected open suspend fun onActorBindingReplacedSuspending(actorId: String)
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
    fun timeout(timeout: Duration): ZLinkKotlinSessionSendCall
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
  public final java.util.concurrent.CompletionStage<java.lang.Void> onActorBindingReplaced(java.lang.String);
  public final java.util.concurrent.CompletionStage<java.lang.Void> onError(systems.zlink.framework.streams.ZLinkStreamError);
  public final java.util.concurrent.CompletionStage<java.lang.Void> onDispatch(systems.zlink.framework.streams.ZLinkSessionDispatchContext, systems.zlink.framework.messaging.ZLinkMessage);
}
public final class systems.zlink.framework.kotlin.ZLinkFrameworkExtensionsKt {
  public static final java.lang.Object bindOrGetActor(systems.zlink.framework.streams.ZLinkSessionActors, systems.zlink.framework.actors.ActorRef, kotlin.coroutines.Continuation<? super systems.zlink.framework.streams.ZLinkSessionActor>);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinSessionSendCall {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinSessionSendCall metadata(java.lang.String, java.lang.String);
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinSessionSendCall compress();
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinSessionSendCall timeout-LRDsOJo(long);
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

`ZLinkKotlinSessionSendCall.timeout(...)`은 이 send의 admission 대기만 줄인다. 생략하면 Java STREAM socket
send timeout을 사용하고 지정하면 두 값 중 짧은 값을 사용하므로 socket timeout을 늘릴 수 없다. Duration은
양수이며 milliseconds로 올림한 값이 `1..Int.MAX_VALUE` 범위여야 한다. 만료되면
`ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED`로 terminal-once 완료하고 이후 admission이나 replay를 시작하지
않는다. Coroutine cancellation은 기존 Kotlin wait cancellation 의미를 유지하며 reply call에는 이 modifier를
제공하지 않는다.
