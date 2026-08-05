# Kotlin Actor 공개 인터페이스

Session에 bind된 Actor를 포함한 Spot relocation은 target에서 Actor와 queue를 복원하고 owner와
membership을 commit한 뒤 message 처리를 시작한다. Target runtime은
`sessionActorLocationUpdateReqMsg`를 send하여 binding route와 bound-session current Actor
location snapshot을 갱신한다. 응답이 없어도 Actor 처리를 멈추지 않으며 정해진 간격으로
같은 요청을 다시 보낸다. Snapshot은 target MeshName·NodeRid를 제공한다. Relocation 자체는 physical·logical disconnect가
아니므로 Actor disconnect callback을 실행하지 않는다. relocation 대상에 포함되지 않은 다른 Actor의 route와 physical connection은
변경하지 않는다.

[인터페이스 목차](README.ko.md) · [Java Actor](../../java/interfaces/actors.ko.md) ·
[Actor 공통 계약](../../../../14-actor-model.ko.md)

Kotlin은 Java의 global Actor identity와 fluent operation을 그대로 사용한다. `ActorId`는 Location Store
transaction domain 전체에서 유일하며 UTF-8 encoded 크기는 1..255 bytes다. 대소문자를 구분하고
normalization하지 않는다. 일반 send/request는 ActorId만 받으며 current authority를 resolve한다.
`ActorRef(actorId, objectGeneration, meshName, nodeRid)`는 exact incarnation을 destroy하거나 session에
bind할 때만 사용한다. `objectGeneration`은 `1..Long.MAX_VALUE`이고 JSON에서는 decimal string이다.

`ZLinkKotlinActorManager.create(actorId, actorType)`와 `getOrCreate(actorId, actorType)`는 Kotlin 전용
single-use wrapper를 반환한다. `inMesh`, `request`, `timeout`을 설정한 뒤 terminal `await()` 또는
`yield()`를 한 번만 호출한다. 같은 option을 두 번 설정하거나 terminal을 두 번 호출하면
`InvalidOperation`이다. `inMesh`를 생략했을 때 object role Mesh가 하나면 자동 선택하고, 0개면
`NotConfigured`, 둘 이상이면 `InvalidOperation`이다. 지정한 Mesh가 없으면 `NotFound`다.
Target RID나 predicate callback을 받는 placement API는 제공하지 않는다.

Create와 GetOrCreate의 `await()`·`yield()`는 모두 `ZLinkActorCreateResult`를 반환한다. `yield()`는
`SPOT_WIDE` User Spot과 Instance Spot application callback에서만 현재 Spot gate를 반납한다. 다른 문맥에서는
reservation, factory 실행과 queue 변경 전에 `InvalidOperation`으로 끝낸다. Actor send는 one-way
`await(): Unit`만 제공하고 `yield()`를 제공하지 않는다.

Actor type은 UTF-8 1..255 bytes의 stable exact value다. `Create`에서 Ready object가 있으면
`AlreadyExists`이며 새 attempt에서는 Java `ZLinkActorCreateResult`의 `Created`
또는 `Rejected`를 반환한다. `GetOrCreate`는 같은 type의
[Ready](../../../../01-glossary.ko.md#ready) object를 callback 없이 `Existing`으로
반환한다. Creating이면 authority 변경을 기다리며 CAS loser는
별도 factory나 callback을 시작하지 않는다. 서로 다른 operation은 Ready 뒤 `Existing`을
받고 cleanup 뒤 새 reservation을 경쟁하며 앞선 application reply를 공유하지 않는다.
같은 source Node RID·lifecycle generation·`OperationId`의 재전송만 correlation-free
`creation-operation-terminal-v1` envelope를 읽고 현재 correlation·reply route로 reply를
다시 encode한다. Terminal은 original deadline 뒤 5분 동안 유지한다. Callback exception은 `Rejected`가 아니라
typed creation failure다. 다른 type이면 `TypeMismatch`다. Kotlin은 local Actor
create, directory, resolver 또는 hidden remote retry를 추가하지 않는다.

Kotlin은 Java `ZLinkActorRelocationAdapter<TActor>`와 factory builder를 그대로 사용한다.
Opaque Java `byte[]`는 Kotlin `ByteArray`로 보이며 `capture`와 `restore`의 asynchronous completion은
`CompletionStage`다. 별도 suspending adapter, `TState`, `stateContractId`, state class와 `ZLinkMessage` 기반
relocation API를 만들지 않는다. State 보존 policy는
`preserveStateWith(ActorAdapter::class.java)`로 구성하며 factory와 adapter target의 일치는 socket
bind 전에 검증한다. Java interop에서 null adapter class를 전달한 policy도 bind 전에 startup configuration error로
거부한다.

`preserveStateWith(...)`로 등록한 Actor adapter는 maintenance cross-node materialization, remote User·Entry Spot join과 whole [User Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot)
relocation의 각 Actor participant에 사용한다. Same-node join과 `disableRelocation()` 또는 `recreateOnRelocation()`을 선택한 factory에서는 호출하지 않는다.
Capture가 반환한 `ByteArray`는 최대 64 MiB이며 adapter가 completion까지 소유한다. Java runtime은 completion에서
복사한다. Restore는 호출마다
fresh defensive copy를 받고 completion 뒤 보관하지 않는다. Empty `ByteArray`도 유효한 보존 state다.
[Factory](../../../../01-glossary.ko.md#factory)는 target attempt마다 fresh Actor instance를 만들며 source나 이전 attempt instance를 재사용하지 않는다.
같은 attempt의 restore는 반복될 수 있다. Capture exception은 source [authority](../../../../01-glossary.ko.md#authority)와 admission을 유지하고, restore
exception은 target을 sealed 상태로 유지한 채 같은 target process에서 동일한 payload로 다시 시도할 수 있다.
다른 target을 자동 선택하지 않는다. Null stage와
null capture payload는 contract 위반이다. Host relocation의 precommit adapter exception·contract violation은 deadline이
먼저 확정되지 않았으면 `Blocked/StateIncompatible`, [deadline](../../../../01-glossary.ko.md#deadline)이 먼저 확정되면 `Blocked/DeadlineExceeded`다.
Stale attempt cancellation은 terminal result를 commit하지 못한다. 두 callback은 at-least-once이고 stale attempt와
겹칠 수 있으므로 retry-safe해야 한다. Kotlin coroutine 안에서 exception을 정상 completion으로 바꾸거나 empty
`ByteArray`를 failure fallback으로 반환하지 않는다.

## Kotlin source signature

```kotlin
interface ZLinkSuspendingEntrySpotActorSendHandler<
    TEntrySpot : ZLinkEntrySpot<*>, TActor : ZLinkActor, TMessage,
> {
    suspend fun handle(
        entrySpot: TEntrySpot,
        actor: TActor,
        context: ZLinkMessageContext,
        message: TMessage,
    )
}

interface ZLinkSuspendingEntrySpotActorRequestHandler<
    TEntrySpot : ZLinkEntrySpot<*>, TActor : ZLinkActor, TRequest, TReply,
> {
    suspend fun handle(
        entrySpot: TEntrySpot,
        actor: TActor,
        context: ZLinkMessageContext,
        request: TRequest,
    ): TReply
}

interface ZLinkSuspendingSpotActorSendHandler<
    TSpot : ZLinkSpot<*>, TActor : ZLinkActor, TMessage,
> {
    suspend fun handle(
        spot: TSpot,
        actor: TActor,
        context: ZLinkMessageContext,
        message: TMessage,
    )
}

interface ZLinkSuspendingSpotActorRequestHandler<
    TSpot : ZLinkSpot<*>, TActor : ZLinkActor, TRequest, TReply,
> {
    suspend fun handle(
        spot: TSpot,
        actor: TActor,
        context: ZLinkMessageContext,
        request: TRequest,
    ): TReply
}

abstract class ZLinkSuspendingActor : ZLinkActor {
    abstract val context: ZLinkActorContext

    // Java accessor를 같은 exact Context property에 연결한다.
    final override fun context(): ZLinkActorContext = context

    // Java callback을 coroutine으로 연결하는 final bridge다.
    final override fun onJoinCompleted(
        completion: ZLinkActorJoinCompletion,
    ): CompletionStage<Void>

    abstract suspend fun onJoinCompletedSuspending(
        completion: ZLinkActorJoinCompletion,
    )
}

abstract class ZLinkSuspendingActorFactory : ZLinkActorFactory {
    protected abstract suspend fun createActor(
        context: ZLinkActorContext,
    ): ZLinkActor
}

interface ZLinkKotlinActorCreateCall {
    fun inMesh(meshName: String): ZLinkKotlinActorCreateCall
    fun request(request: Any): ZLinkKotlinActorCreateCall
    fun timeout(timeout: Duration): ZLinkKotlinActorCreateCall
    suspend fun await(): ZLinkActorCreateResult
    suspend fun yield(): ZLinkActorCreateResult
}

interface ZLinkKotlinActorManager {
    fun create(actorId: String, actorType: String): ZLinkKotlinActorCreateCall
    fun getOrCreate(
        actorId: String,
        actorType: String,
    ): ZLinkKotlinActorCreateCall
}

interface ZLinkKotlinActorClient {
    fun sendToActor(
        actorId: String,
        message: Any,
    ): ZLinkKotlinMessageSendCall

    fun <TReply : Any> requestToActor(
        actorId: String,
        request: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinRequestCall<TReply>
}

inline fun <reified TReply : Any> ZLinkKotlinActorClient.requestToActor(
    actorId: String,
    request: Any,
): ZLinkKotlinRequestCall<TReply> =
    requestToActor(actorId, request, TReply::class)

interface ZLinkKotlinWorkerCall<T> {
    suspend fun await(): T
    suspend fun yield(): T
}

```

## Exact generated JVM signature

```java
public abstract class systems.zlink.framework.kotlin.ZLinkSuspendingActorFactory implements systems.zlink.framework.actors.ZLinkActorFactory {
  public systems.zlink.framework.kotlin.ZLinkSuspendingActorFactory();
  public final java.util.concurrent.CompletionStage<systems.zlink.framework.actors.ZLinkActor> create(systems.zlink.framework.actors.ZLinkActorContext);
}
public abstract class systems.zlink.framework.kotlin.ZLinkSuspendingActor implements systems.zlink.framework.actors.ZLinkActor {
  public abstract systems.zlink.framework.actors.ZLinkActorContext getContext();
  public final systems.zlink.framework.actors.ZLinkActorContext context();
  public final java.util.concurrent.CompletionStage<java.lang.Void> onJoinCompleted(systems.zlink.framework.actors.ZLinkActorJoinCompletion);
  public abstract java.lang.Object onJoinCompletedSuspending(systems.zlink.framework.actors.ZLinkActorJoinCompletion, kotlin.coroutines.Continuation<? super kotlin.Unit>);
}
public interface systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorSendHandler<TEntrySpot extends systems.zlink.framework.spots.ZLinkEntrySpot<?>, TActor extends systems.zlink.framework.actors.ZLinkActor, TMessage> {
  public abstract java.lang.Object handle(TEntrySpot, TActor, systems.zlink.framework.ZLinkMessageContext, TMessage, kotlin.coroutines.Continuation<? super kotlin.Unit>);
}
public interface systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler<TEntrySpot extends systems.zlink.framework.spots.ZLinkEntrySpot<?>, TActor extends systems.zlink.framework.actors.ZLinkActor, TRequest, TReply> {
  public abstract java.lang.Object handle(TEntrySpot, TActor, systems.zlink.framework.ZLinkMessageContext, TRequest, kotlin.coroutines.Continuation<? super TReply>);
}
public interface systems.zlink.framework.kotlin.ZLinkSuspendingSpotActorSendHandler<TSpot extends systems.zlink.framework.spots.ZLinkSpot<?>, TActor extends systems.zlink.framework.actors.ZLinkActor, TMessage> {
  public abstract java.lang.Object handle(TSpot, TActor, systems.zlink.framework.ZLinkMessageContext, TMessage, kotlin.coroutines.Continuation<? super kotlin.Unit>);
}
public interface systems.zlink.framework.kotlin.ZLinkSuspendingSpotActorRequestHandler<TSpot extends systems.zlink.framework.spots.ZLinkSpot<?>, TActor extends systems.zlink.framework.actors.ZLinkActor, TRequest, TReply> {
  public abstract java.lang.Object handle(TSpot, TActor, systems.zlink.framework.ZLinkMessageContext, TRequest, kotlin.coroutines.Continuation<? super TReply>);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinActorCreateCall {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinActorCreateCall inMesh(java.lang.String);
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinActorCreateCall request(java.lang.Object);
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinActorCreateCall timeout-LRDsOJo(long);
  public abstract java.lang.Object await(kotlin.coroutines.Continuation<? super systems.zlink.framework.actors.ZLinkActorCreateResult>);
  public abstract java.lang.Object yield(kotlin.coroutines.Continuation<? super systems.zlink.framework.actors.ZLinkActorCreateResult>);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinWorkerCall<T> {
  public abstract java.lang.Object await(kotlin.coroutines.Continuation<? super T>);
  public abstract java.lang.Object yield(kotlin.coroutines.Continuation<? super T>);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinActorManager {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinActorCreateCall create(java.lang.String, java.lang.String);
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinActorCreateCall getOrCreate(java.lang.String, java.lang.String);
}
public interface systems.zlink.framework.kotlin.ZLinkKotlinActorClient {
  public abstract systems.zlink.framework.kotlin.ZLinkKotlinMessageSendCall sendToActor(java.lang.String, java.lang.Object);
  public abstract <TReply> systems.zlink.framework.kotlin.ZLinkKotlinRequestCall<TReply> requestToActor(java.lang.String, java.lang.Object, kotlin.reflect.KClass<TReply>);
}
```

Factory callback은 `disableRelocation()`, `recreateOnRelocation()`, `preserveStateWith(...)` 중 하나를 반드시 호출한다. Kotlin은 state 보존 설정과
adapter registration을 위한 reified helper, policy를 생략하는 overload와 default argument를 생성하지 않는다.
Exact `ActorRef`를 받는 public operation은
destroy와 session bind뿐이다. Missing exact ref는 `false`, generation 불일치는 `InvalidOperation`, seal된
이관 구간은 `Unavailable`로 처리한다.

Actor Join에는 coroutine terminal을 추가하지 않는다. Java exact interface의 동기 `defer()`를 handler
실행 중 한 번 호출하며 Spot gate나 Actor FIFO claim을 반납하지 않는다. Request·worker·create
wrapper의 `yield()`는 `SPOT_WIDE` User Spot member Actor에서 Actor FIFO claim을 유지하고 User Spot gate만
반환한다. Entry Actor와 `PER_ACTOR` Actor에서는 underlying Java operation submission 전에
`InvalidOperation`으로 완료한다. 같은 Actor 자신에게 보내는 awaited request도 coroutine을 suspend하거나
queue를 변경하기 전에 거부한다.
`SPOT_WIDE` member Actor가 현재 User Spot을 떠나는 Join도 `defer()`로 등록하고 handler의 마지막
continuation 뒤 실행한다. Callback을 inline 또는 재진입 방식으로 호출하지 않는다.

`defer()`는 target 조회나 Store I/O 없이 immutable Join intent와 비활성 barrier만
등록한다. Handler가 실패하면 barrier를 폐기하며, 정상 종료 뒤의 결과는
`onJoinCompletedSuspending(...)`에서 받는다. Request 없는 overload는 empty `ZLinkMessage`를
고정한다. Timeout 기본값은 5초이고 명시 값은 millisecond 올림 기준 유한한
`1..Int.MAX_VALUE` ms다. `defer()`를 호출한 시점에 monotonic absolute deadline을
고정한다.

Completion operation ID는 `RelocationId`, reservation ID나 aggregate commit ID와
다른 idempotency ID다. Same-node와 cross-node completion retry는 현재 source와
target process lifetime으로 제한한다. Process 종료 뒤 다른 runtime이 completion을
자동 replay하지 않는다.
