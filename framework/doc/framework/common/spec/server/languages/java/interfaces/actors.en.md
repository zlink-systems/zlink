# Java Actor Public Interface

A Spot relocation, including an Actor bound to a session, restores the
Actor and queue on the target, commits owner and membership, and then
starts message processing. The target runtime sends
`sessionActorLocationUpdateReqMsg` to update the binding route and the
bound-session current Actor location snapshot. Even without a response,
Actor processing doesn't stop, and the same request is resent at a fixed
interval. The snapshot provides the target MeshName/NodeRid. Since
relocation itself isn't a physical/logical disconnect, it doesn't run the
Actor disconnect callback. The route and physical connection of a
different Actor not included in the relocation target aren't changed.

[Interface table of contents](README.en.md) · [Common Actor Contract](../../../../14-actor-model.en.md)

This document fixes the public interface that expresses Actor factory,
context, messaging, manager, and relocation adapter in Java. A regular
message targets with ActorId, and an operation that changes a specific
incarnation uses an exact `ActorRef`.

```java
public interface ZLinkActorFactory {
    CompletionStage<ZLinkActor> create(ZLinkActorContext context);
}

public interface ZLinkActorHandlerRegistry {
    void addHandler(Class<?> handlerType);
}

public interface ZLinkRelocationCancellation {
    boolean isCancellationRequested();
}

public interface ZLinkActorRelocationAdapter<TActor extends ZLinkActor> {
    CompletionStage<byte[]> capture(
        TActor actor, ZLinkRelocationCancellation cancellation);
    CompletionStage<Void> restore(
        TActor actor, byte[] state, ZLinkRelocationCancellation cancellation);
}

```

The exact builder member of
[factory](../../../../01-glossary.en.md#factory) registration is owned
by [Configuration And Host](configuration-host.en.md). Cross-node
relocation behavior is wired directly to the Actor factory configure
callback. The runtime checks the Actor the factory returned against the
specified `actorClass`, returning a type mismatch as a startup error. A
relocation registry separate from the factory isn't provided. The
`adapterClass` of `preserveStateWith(...)` must implement
`ZLinkActorRelocationAdapter<TActor>` for that Actor type. Adapter type
validation for User/Instance Spot policy is owned by the
[Spot Interface](spots.en.md). Taking `Class<?>` is a representation to
keep the policy value common due to Java type erasure, and the framework
checks whether the factory type and adapter generic target match before
socket bind. A mismatch is a startup configuration error.
`preserveStateWith(null)` is rejected as a configuration error during
callback execution.

The Actor adapter captures/restores application state as an opaque
`byte[]` of at most 64 MiB. A public state DTO, `TState`,
`stateContractId`, state class, and `ZLinkMessage` aren't put on the
relocation surface. The framework immediately copies the array once
capture completes normally. The array capture returns is still owned by
the adapter — reusing or changing it after completion doesn't change the
stored payload. Restore is passed a fresh defensive copy of the stored
payload per call, and the adapter doesn't keep that array after the
stage finishes. A zero-length array is also a valid preserved state — it
isn't interpreted as choosing `recreateOnRelocation()` or omitting
restore. The adapter doesn't receive owner claim, relocation envelope,
generation, or recovery phase.

If an Actor factory uses `preserveStateWith(...)` in cross-node
materialization, the same Actor adapter is used for maintenance Actor
relocation, remote User/[Entry Spot](../../../../01-glossary.en.md#entry-user-instance-spot)
join, and each Actor participant of a whole User Spot relocation. The
adapter isn't called on a same-node join or on a factory that selected
`disableRelocation()` or `recreateOnRelocation()`.
`recreateOnRelocation()` doesn't capture application state, so it has no
adapter.

The target commits owner after finishing restore and accepted journal
staging. After the lifecycle callback, the saved existing work is put on
the actual Actor queue first, and the relocation temporary queue's work
is moved after that. Once temporary queue registration is removed and
dispatch is switched atomically, the target opens as `READY`. Source
cleanup, the `COMPLETED` record, and the bound-session location update
response don't block the target's message processing. If the target
process terminates after `READY`, it's handled as ordinary
[owner](../../../../01-glossary.en.md#owner) loss, and the previous
relocation isn't automatically replayed. A public phase API for
manipulating this barrier isn't provided.

On a retry within the same source and target process, factory and
`restore(...)` can be called more than once. `capture(...)` can also be
repeated before the [authority](../../../../01-glossary.en.md#authority)
commit. Only the current owner and attempt fence can commit completion
and open admission. Since the callback doesn't add a relocation ID,
application restore and capture must be retry-safe, and exactly-once
external side effect isn't guaranteed. The factory creates a fresh Actor
instance per target attempt, and the framework only calls that attempt's
`restore(...)` on that instance. The source instance or a previous target
attempt's instance isn't reused for a new attempt, and restore can be
repeated within the same attempt.

If the capture stage ends with an exception, the attempt is aborted
before authority publication and source authority and admission are
kept. If the restore stage ends with an exception, target admission is
kept sealed and can be retried with the same payload on the same target
process. A different target isn't automatically selected. An exception
isn't turned into an empty payload or normal completion. A null stage
and null `byte[]` from capture, and a null stage from restore, are
adapter contract violations. A precommit adapter exception and contract
violation where a deadline hasn't been fixed yet in host relocation are
classified as `Blocked/StateIncompatible`. Once a
[deadline](../../../../01-glossary.en.md#deadline) is fixed,
`Blocked/DeadlineExceeded` is used, and cancellation of a stale target
attempt can't commit a terminal result. The adapter must be retry-safe to
allow repeated calls and stale-attempt overlap, and an external side
effect inside the callback can't be assumed exactly-once.

Relocated terminal reply accounting uses internal command ID 46
`replyRelayAck`. This command only has a stable relocation ID, operation
ID, exact request-source fence (owner ID, lease generation, node RID,
node generation), and status — it doesn't carry payload or metadata. A
physical connection close isn't terminal evidence. Only the exact
request-source lease expiry stored in an ACK or accepted record completes
terminal accounting — there's no public ACK API.

The source only commits `CAPTURED` after every admitted connection-bound
work, including connection-bound one-way, reaches terminal accounting.
A durable accepted journal is only used on a source with an exact owner
lease. If pre-`CAPTURED` drain doesn't finish within the deadline,
relocation is aborted and host relocation ends with
`BLOCKED/DEADLINE_EXCEEDED`. Source admission isn't opened before durable
abort and source normalization finish. There's no exception that
captures a connection-bound one-way in an incomplete state.

An Actor on Entry Spot and `PerActor` User Spot is an independent
relocation unit. Only a `SpotWide` User Spot's member Actor moves the
Spot and the entire current membership together as one aggregate. User
Spot membership itself isn't a relocation blocker — that Actor unit or
the `SpotWide` aggregate is only blocked when even one participant
selected `disableRelocation()` or a compatible target can't be secured.
A participant with relocation disabled gets `BLOCKED/RELOCATION_DISABLED`;
absence of target/capacity/reservation gets
`BLOCKED/TARGET_UNAVAILABLE`; a mismatch of application
version/type/state-preservation adapter capability gets
`BLOCKED/STATE_INCOMPATIBLE`. The Actor unit finishes target factory and
restore, prepares the accepted journal as a staging queue the
application handler hasn't run, and then performs the `NEW_OWNER` CAS.
This CAS atomically changes owner, authority owner generation, and the
current [Spot](../../../../01-glossary.en.md#spot) to the target
execution shell. Infrastructure relocation doesn't call an application
membership callback. Dispatch opens after finishing journal/queue/Actor
timer replay, source relay, and durable cleanup. There's no public phase
API that controls this order.

When creating a new distributed Actor, the framework selects one target
to become owner, and secures `CREATING` authority and pending capacity
together as one reservation on that target. Only the target that secured
the reservation performs factory, initial Entry membership, and
initialize. On success, the same reservation is committed with `READY`
and active capacity; on failure, it's aborted. A target that loses the
CAS race doesn't run a separate factory.

The Actor Join call only provides a synchronous `defer()`, and doesn't
provide `submit(...)`/`yield(...)`. `defer()` only registers an
immutable Join intent and an inactive barrier on the current handler, and
doesn't start a target lookup or Store I/O. If the handler finishes
normally, the Join runs; if it fails, the barrier is discarded. The
result is delivered via the `onJoinCompleted(...)` Actor callback with
the same 128-bit operation ID.

Operation ID is a completion idempotency ID, not a `RelocationId`,
reservation ID, or aggregate commit ID. Same-node and cross-node
completion retry are limited to the current source and target process
lifetime. After the process ends, a different runtime doesn't
automatically replay completion.

The overload with no request fixes an empty `ZLinkMessage`. The default
timeout is 5 seconds, and an explicit value is a finite `1..Integer.MAX_VALUE`
ms rounded up to milliseconds. `defer()` fixes a monotonic absolute
deadline.

## Exact Public Member Inventory

The declarations below fix this category's Java public types and
members.

```java
public interface systems.zlink.framework.actors.ZLinkActorFactory {
  public abstract java.util.concurrent.CompletionStage<systems.zlink.framework.actors.ZLinkActor> create(systems.zlink.framework.actors.ZLinkActorContext);
}
public interface systems.zlink.framework.actors.ZLinkRelocationCancellation {
  public abstract boolean isCancellationRequested();
}
public interface systems.zlink.framework.actors.ZLinkActorRelocationAdapter<TActor extends systems.zlink.framework.actors.ZLinkActor> {
  public abstract java.util.concurrent.CompletionStage<byte[]> capture(TActor, systems.zlink.framework.actors.ZLinkRelocationCancellation);
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> restore(TActor, byte[], systems.zlink.framework.actors.ZLinkRelocationCancellation);
}
public interface systems.zlink.framework.actors.ZLinkActorHandlerRegistry {
  public abstract void addHandler(java.lang.Class<?>);
}
public final class systems.zlink.framework.actors.ActorRef extends java.lang.Record {
  public systems.zlink.framework.actors.ActorRef(java.lang.String, long, java.lang.String, systems.zlink.contracts.core.RoutingId);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public java.lang.String actorId();
  public long objectGeneration();
  public java.lang.String meshName();
  public systems.zlink.contracts.core.RoutingId nodeRid();
}
public interface systems.zlink.framework.actors.ZLinkActor {
  public abstract systems.zlink.framework.actors.ZLinkActorContext context();
  public default void configure();
  public default java.util.concurrent.CompletionStage<java.lang.Void> onJoinCompleted(systems.zlink.framework.actors.ZLinkActorJoinCompletion);
}
public interface systems.zlink.framework.actors.ZLinkActorClient {
  public abstract systems.zlink.framework.actors.ZLinkActorSendCall sendToActor(java.lang.String, java.lang.Object);
  public abstract systems.zlink.framework.actors.ZLinkActorRequestCall requestToActor(java.lang.String, java.lang.Object);
}
public interface systems.zlink.framework.actors.ZLinkActorContext {
  public abstract java.lang.String actorId();
  public abstract long objectGeneration();
  public abstract java.lang.String meshName();
  public abstract java.util.Optional<java.lang.String> spotId();
  public abstract systems.zlink.framework.actors.ZLinkBoundSession boundSession();
  public abstract systems.zlink.framework.actors.ZLinkActorJoinCall joinSpot(java.lang.String);
  public abstract systems.zlink.framework.actors.ZLinkActorJoinCall joinSpot(java.lang.String, java.lang.Object);
  public abstract systems.zlink.framework.actors.ZLinkActorJoinCall joinEntrySpot();
  public abstract systems.zlink.framework.actors.ZLinkActorJoinCall joinEntrySpot(java.lang.Object);
}
public interface systems.zlink.framework.actors.ZLinkActorJoinCall {
  public abstract systems.zlink.framework.actors.ZLinkActorJoinCall timeout(java.time.Duration);
  public abstract void defer();
}
public final class systems.zlink.framework.actors.ZLinkActorJoinOperationId extends java.lang.Record {
  public systems.zlink.framework.actors.ZLinkActorJoinOperationId(long, long);
  public long high();
  public long low();
}
public final class systems.zlink.framework.actors.ZLinkActorJoinCompletion$Accepted extends java.lang.Record implements systems.zlink.framework.actors.ZLinkActorJoinCompletion {
  public systems.zlink.framework.actors.ZLinkActorJoinCompletion$Accepted(systems.zlink.framework.actors.ZLinkActorJoinOperationId, systems.zlink.framework.actors.ActorRef, systems.zlink.framework.messaging.ZLinkMessage);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public systems.zlink.framework.actors.ZLinkActorJoinOperationId operationId();
  public systems.zlink.framework.actors.ActorRef actor();
  public systems.zlink.framework.messaging.ZLinkMessage reply();
}
public final class systems.zlink.framework.actors.ZLinkActorJoinCompletion$Rejected extends java.lang.Record implements systems.zlink.framework.actors.ZLinkActorJoinCompletion {
  public systems.zlink.framework.actors.ZLinkActorJoinCompletion$Rejected(systems.zlink.framework.actors.ZLinkActorJoinOperationId, systems.zlink.framework.messaging.ZLinkMessage);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public systems.zlink.framework.actors.ZLinkActorJoinOperationId operationId();
  public systems.zlink.framework.messaging.ZLinkMessage reply();
}
public final class systems.zlink.framework.actors.ZLinkActorJoinCompletion$Failed extends java.lang.Record implements systems.zlink.framework.actors.ZLinkActorJoinCompletion {
  public systems.zlink.framework.actors.ZLinkActorJoinCompletion$Failed(systems.zlink.framework.actors.ZLinkActorJoinOperationId, systems.zlink.framework.errors.ZLinkFrameworkErrorKind);
  public systems.zlink.framework.actors.ZLinkActorJoinOperationId operationId();
  public systems.zlink.framework.errors.ZLinkFrameworkErrorKind kind();
}
public sealed interface systems.zlink.framework.actors.ZLinkActorJoinCompletion
    permits systems.zlink.framework.actors.ZLinkActorJoinCompletion.Accepted,
            systems.zlink.framework.actors.ZLinkActorJoinCompletion.Rejected,
            systems.zlink.framework.actors.ZLinkActorJoinCompletion.Failed {
}
public interface systems.zlink.framework.actors.ZLinkActorManager {
  public abstract systems.zlink.framework.actors.ZLinkActorCreateCall create(java.lang.String, java.lang.String);
  public abstract systems.zlink.framework.actors.ZLinkActorGetOrCreateCall getOrCreate(java.lang.String, java.lang.String);
  public abstract java.util.concurrent.CompletionStage<java.util.Optional<systems.zlink.framework.actors.ActorRef>> find(java.lang.String);
  public abstract java.util.concurrent.CompletionStage<java.util.Optional<systems.zlink.framework.spots.SpotRef>> findSpot(java.lang.String);
  public abstract java.util.concurrent.CompletionStage<java.lang.Boolean> destroy(systems.zlink.framework.actors.ActorRef);
}
public interface systems.zlink.framework.actors.ZLinkActorCreateCall {
  public abstract systems.zlink.framework.actors.ZLinkActorCreateCall inMesh(java.lang.String);
  public abstract systems.zlink.framework.actors.ZLinkActorCreateCall request(java.lang.Object);
  public abstract systems.zlink.framework.actors.ZLinkActorCreateCall request(systems.zlink.framework.messaging.ZLinkMessage);
  public abstract systems.zlink.framework.actors.ZLinkActorCreateCall timeout(java.time.Duration);
  public abstract java.util.concurrent.CompletionStage<systems.zlink.framework.actors.ZLinkActorCreateResult> submit();
  public abstract java.util.concurrent.CompletionStage<systems.zlink.framework.actors.ZLinkActorCreateResult> yield();
}
public interface systems.zlink.framework.actors.ZLinkActorGetOrCreateCall {
  public abstract systems.zlink.framework.actors.ZLinkActorGetOrCreateCall inMesh(java.lang.String);
  public abstract systems.zlink.framework.actors.ZLinkActorGetOrCreateCall request(java.lang.Object);
  public abstract systems.zlink.framework.actors.ZLinkActorGetOrCreateCall request(systems.zlink.framework.messaging.ZLinkMessage);
  public abstract systems.zlink.framework.actors.ZLinkActorGetOrCreateCall timeout(java.time.Duration);
  public abstract java.util.concurrent.CompletionStage<systems.zlink.framework.actors.ZLinkActorCreateResult> submit();
  public abstract java.util.concurrent.CompletionStage<systems.zlink.framework.actors.ZLinkActorCreateResult> yield();
}
public sealed interface systems.zlink.framework.actors.ZLinkActorCreateResult
    permits systems.zlink.framework.actors.ZLinkActorCreateResult.Existing,
            systems.zlink.framework.actors.ZLinkActorCreateResult.Created,
            systems.zlink.framework.actors.ZLinkActorCreateResult.Rejected {
}
public interface systems.zlink.framework.actors.ZLinkActorRequestCall {
  public abstract systems.zlink.framework.actors.ZLinkActorRequestCall metadata(java.lang.String, java.lang.String);
  public abstract systems.zlink.framework.actors.ZLinkActorRequestCall timeout(java.time.Duration);
  public abstract <TReply> java.util.concurrent.CompletionStage<TReply> submit(java.lang.Class<TReply>);
  public abstract <TReply> java.util.concurrent.CompletionStage<TReply> yield(java.lang.Class<TReply>);
}
public interface systems.zlink.framework.actors.ZLinkActorSendCall {
  public abstract systems.zlink.framework.actors.ZLinkActorSendCall metadata(java.lang.String, java.lang.String);
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> submit();
}
public interface systems.zlink.framework.actors.ZLinkBoundSession {
  public abstract systems.zlink.framework.actors.ZLinkBoundSessionSendCall send(java.lang.Object);
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> disconnect();
}
public interface systems.zlink.framework.actors.ZLinkBoundSessionSendCall {
  public abstract systems.zlink.framework.actors.ZLinkBoundSessionSendCall metadata(java.lang.String, java.lang.String);
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> submit();
}
```

ActorId is a global logical ID of UTF-8 1..255 bytes. `ActorRef`
preserves ActorId, a positive signed-63-bit ObjectGeneration, and the
MeshName/NodeRid at query time. A regular message only takes ActorId and
resolves current authority. Only destroy and session bind take the exact
ref.

Create and GetOrCreate calls are single-use. Setting the same option
twice, or calling submit twice, is `INVALID_OPERATION`. If `inMesh` is
omitted and there's one object-role Mesh, it's auto-selected; with 0,
`NOT_CONFIGURED`; with two or more, `INVALID_OPERATION`. If the
specified Mesh doesn't exist, `NOT_FOUND`. The caller doesn't specify a
target RID or placement callback. `find` and `findSpot` only return the
current Ready ref, and don't provide a directory or resolver.

`create` returns `ALREADY_EXISTS` if a Ready Actor exists, and a new
attempt returns `Created` or `Rejected`. `getOrCreate` returns a Ready
Actor of the same type as `Existing`, without a callback. If Creating, it
waits for the authority change, and a CAS loser doesn't start a separate
factory or callback. A different operation receives `Existing` after
Ready, competes for a new reservation after cleanup, and doesn't share an
earlier application reply. Only a resend of the same source Node
RID/lifecycle generation/`OperationId` reads the correlation-free
`creation-operation-terminal-v1` envelope and re-encodes the reply with
the current correlation/reply route. The terminal is kept for 5 minutes
after the original deadline. A callback exception isn't `Rejected` — it's
a typed creation failure.

`ActorRef.objectGeneration()` is `1..Long.MAX_VALUE`. Typed JSON uses
the required properties `actorId`, `objectGeneration`, `meshName`,
`nodeRid`, and generation is encoded as a decimal string with no leading
zero. An unknown property, duplicate property, missing required
property, a non-numeric token, and an out-of-range value are rejected.

`yield(...)` declared on an Actor request is only valid while the
current Actor handler is running on a `SpotWide` User Spot's shared
execution gate. If called by an Entry Spot Actor or a `PerActor` User
Spot's Actor, it completes with `INVALID_OPERATION`, without submitting
the operation or returning the turn. Actor Join is only registered with
`defer()` inside the current handler, and doesn't provide `submit(...)`
and `yield(...)`.
