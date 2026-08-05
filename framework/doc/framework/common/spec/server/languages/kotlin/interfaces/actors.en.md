# Kotlin Actor Public Interface

A Spot relocation, including an Actor bound to a session, restores the
Actor and queue on the target, commits owner and membership, and then
starts message processing. The target runtime sends
`sessionActorLocationUpdateReqMsg` to update the binding route and the
bound-session current Actor location snapshot. Even without a response,
Actor processing doesn't stop, and the same request is resent at a fixed
interval. The snapshot provides the target MeshName/NodeRid. Since
relocation itself isn't a physical/logical disconnect, it doesn't run
the Actor disconnect callback. The route and physical connection of a
different Actor not included in the relocation target aren't changed.

[Interface table of contents](README.en.md) · [Java Actor](../../java/interfaces/actors.en.md) ·
[Common Actor Contract](../../../../14-actor-model.en.md)

Kotlin uses Java's global Actor identity and fluent operation unchanged.
`ActorId` is unique across the whole Location Store transaction domain,
UTF-8 encoded size 1..255 bytes. It's case-sensitive and not
normalized. A regular send/request only takes ActorId and resolves
current authority. `ActorRef(actorId, objectGeneration, meshName,
nodeRid)` is only used to destroy an exact incarnation or bind to a
session. `objectGeneration` is `1..Long.MAX_VALUE`, and a decimal string
in JSON.

`ZLinkKotlinActorManager.create(actorId, actorType)` and
`getOrCreate(actorId, actorType)` return a Kotlin-only single-use
wrapper. After setting `inMesh`, `request`, `timeout`, terminal
`await()` or `yield()` is called exactly once. Setting the same option
twice or calling terminal twice is `InvalidOperation`. If `inMesh` is
omitted and there's one object-role Mesh, it's auto-selected; with 0,
`NotConfigured`; with two or more, `InvalidOperation`. If the specified
Mesh doesn't exist, `NotFound`. A placement API taking a target RID or
predicate callback isn't provided.

Both `await()` and `yield()` of Create and GetOrCreate return
`ZLinkActorCreateResult`. `yield()` only returns the current Spot gate
in a `SPOT_WIDE` User Spot and Instance Spot application callback. In a
different context, it ends with `InvalidOperation` before reservation,
factory execution, and queue change. Actor send only provides a one-way
`await(): Unit` and doesn't provide `yield()`.

Actor type is a stable exact value of UTF-8 1..255 bytes. If `Create`
finds a Ready object, it's `AlreadyExists`, and a new attempt returns
Java `ZLinkActorCreateResult`'s `Created` or `Rejected`.
`GetOrCreate` returns a Ready object of the same type as `Existing`,
without a callback. If Creating, it waits for the authority change, and
a CAS loser doesn't start a separate factory or callback. A different
operation receives `Existing` after Ready, competes for a new
reservation after cleanup, and doesn't share an earlier application
reply. Only a resend of the same source Node RID/lifecycle
generation/`OperationId` reads the correlation-free
`creation-operation-terminal-v1` envelope and re-encodes the reply with
the current correlation/reply route. The terminal is kept for 5 minutes
after the original deadline. A callback exception isn't `Rejected` —
it's a typed creation failure. A different type is `TypeMismatch`.
Kotlin doesn't add a local Actor create, directory, resolver, or hidden
remote retry.

Kotlin uses Java's `ZLinkActorRelocationAdapter<TActor>` and factory
builder unchanged. The opaque Java `byte[]` appears as Kotlin
`ByteArray`, and `capture` and `restore`'s asynchronous completion is
`CompletionStage`. A separate suspending adapter, `TState`,
`stateContractId`, state class, or `ZLinkMessage`-based relocation API
isn't created. The state-preservation policy is configured with
`preserveStateWith(ActorAdapter::class.java)`, and the match between
factory and adapter target is validated before socket bind. A policy
passing a null adapter class through Java interop is also rejected as a
startup configuration error before bind.

An Actor adapter registered with `preserveStateWith(...)` is used for
maintenance cross-node materialization, remote User/Entry Spot join, and
each Actor participant of a whole
[User Spot](../../../../01-glossary.en.md#entry-user-instance-spot)
relocation. It isn't called on a same-node join or on a factory that
selected `disableRelocation()` or `recreateOnRelocation()`. The
`ByteArray` capture returns is at most 64 MiB, and the adapter owns it
until completion. The Java runtime copies it at completion. Restore
receives a fresh defensive copy per call and doesn't keep it after
completion. An empty `ByteArray` is also a valid preserved state. The
[factory](../../../../01-glossary.en.md#factory) creates a fresh Actor
instance per target attempt and doesn't reuse the source or a previous
attempt's instance. Restore of the same attempt can be repeated. A
capture exception keeps source
[authority](../../../../01-glossary.en.md#authority) and admission, and
a restore exception keeps the target sealed while allowing a retry with
the same payload on the same target process. A different target isn't
automatically selected. A null stage and null capture payload are
contract violations. Host relocation's precommit adapter exception/
contract violation is `Blocked/StateIncompatible` if a deadline hasn't
been fixed yet, and `Blocked/DeadlineExceeded` once the
[deadline](../../../../01-glossary.en.md#deadline) is fixed. Stale
attempt cancellation can't commit a terminal result. Both callbacks are
at-least-once and can overlap with a stale attempt, so they must be
retry-safe. Inside a Kotlin coroutine, an exception isn't turned into
normal completion, and an empty `ByteArray` isn't returned as a failure
fallback.

## Kotlin Source Signature

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

    // connects the Java accessor to the same exact Context property.
    final override fun context(): ZLinkActorContext = context

    // a final bridge connecting the Java callback to a coroutine.
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

## Exact Generated JVM Signature

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

The factory callback must call one of `disableRelocation()`,
`recreateOnRelocation()`, `preserveStateWith(...)`. Kotlin doesn't
generate a reified helper for state-preservation configuration and
adapter registration, or an overload/default argument that omits the
policy. The only public operations taking an exact `ActorRef` are
destroy and session bind. A missing exact ref is `false`, a generation
mismatch is `InvalidOperation`, and a sealed handoff window is
`Unavailable`.

A coroutine terminal isn't added to Actor Join. The Java exact
interface's synchronous `defer()` is called once during handler
execution, and doesn't return the Spot gate or Actor FIFO claim. The
request/worker/create wrapper's `yield()` keeps the Actor FIFO claim on
a `SPOT_WIDE` User Spot member Actor and only returns the User Spot
gate. On an Entry Actor and a `PER_ACTOR` Actor, it completes with
`InvalidOperation` before the underlying Java operation submission. An
awaited request the same Actor sends to itself is also rejected before
suspending the coroutine or changing the queue. A `SPOT_WIDE` member
Actor's Join leaving the current User Spot is also registered with
`defer()` and runs after the handler's last continuation. The callback
isn't called inline or in a re-entrant way.

`defer()` only registers an immutable Join intent and an inactive
barrier, without a target lookup or Store I/O. If the handler fails, the
barrier is discarded, and the result after a normal finish is received
in `onJoinCompletedSuspending(...)`. The overload with no request fixes
an empty `ZLinkMessage`. The default timeout is 5 seconds, and an
explicit value is a finite `1..Int.MAX_VALUE` ms rounded up to
milliseconds. The monotonic absolute deadline is fixed at the moment
`defer()` is called.

The completion operation ID is an idempotency ID distinct from
`RelocationId`, reservation ID, or aggregate commit ID. Same-node and
cross-node completion retry are limited to the current source and
target process lifetime. After the process ends, a different runtime
doesn't automatically replay completion.
