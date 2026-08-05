---
title: "Spot And Actor Membership"
---

# Spot And Actor Membership

[Spec table of contents](README.en.md) · [Previous: Actor Model](14-actor-model.ko.md) · [Next: Spot Address Messaging](16-spot-address-messaging.ko.md)

> **What this chapter defines** — Actor creation, Spot membership, relocation, and
> aggregate relocation. Automatic failover is out of scope.


This document defines Actor creation, Spot membership, relocation, and aggregate
relocation — moving several objects together — in ZLink Framework. Automatic failover,
where a different runtime takes over relocation after a process terminates, isn't part
of the contract.

Core only provides raw sockets and transport. An object's
[membership](01-glossary.en.md#membership), relocation state, and lifecycle are
managed by each language's framework runtime.

## 1. Identity And Authority

ActorId and Entry/User/Instance Spot ID are logical keys global across the whole
Location Store namespace. `MeshName` is an attribute used to decide where to initially
place an object, and isn't part of the authority key.

The [Location Store](01-glossary.en.md#location-store) records, for each logical key,
which node currently processes the object and which
[Spot](01-glossary.en.md#spot) an Actor belongs to. This current processing right and
location record is called authority. When an object moves to another node, the logical
key stays the same while only the current owner information changes to a new value.

`ActorRef`'s and `SpotRef`'s `ObjectGeneration` is a non-zero unsigned 63-bit
conceptual value. `ObjectGeneration` is kept even when membership or the
[owner](01-glossary.en.md#owner) MeshNode changes within the same incarnation.
Instead, the provider issues a larger `AuthorityOwnerGeneration` to distinguish the
new owner.

Authority recorded in the Location Store includes the following information.

| Item | Meaning |
|---|---|
| Current owner | Points to the owner currently processing the Actor/Spot. |
| Spot membership | Points to the Entry Spot or User Spot the Actor currently belongs to. |
| `ObjectGeneration` | Distinguishes an incarnation re-created under the same logical identity. |
| `AuthorityOwnerGeneration` | Distinguishes a previous owner's work when the owner changes within the same incarnation. |
| `StoreVersion` | Verified so CAS applies only when it matches the read authority's state. |
| Exact owner lease | Verifies whether the owner lifecycle recorded in authority is still valid. |

The runtime route cache is only a snapshot of the [authority](01-glossary.en.md#authority)
row — the cache alone doesn't determine current authority. Join, leave, relocation,
destroy, and close all only use a transaction that verifies the expected
`StoreVersion`, generation, and [owner lease](01-glossary.en.md#owner-lease).

The Object Client or Server role requires a Location Store. Without a Store, it's
rejected at startup, and a hidden local Store or a runtime-local object manager with a
reduced meaning under the same name isn't created. Manual topology with object role
`None` can only use Node direct and Channel operations.

## 2. The Process That Confirms Only One Object Is Created

Even if several nodes try to create the same Actor or Spot at the same time, the
factory must only start on the one place that obtained creation authority. The
framework confirms this authority as one by reserving, in the Location Store, both the
object to create and the target node's capacity together. This record is called a
placement reservation.

When creating an Actor and User Spot via manager, versus creating an
[Instance Spot](01-glossary.en.md#entry-user-instance-spot) from its first message, the
location requesting the reservation differs.

| Creation method | Who requests the reservation, and when |
|---|---|
| Actor/User Spot manager create | The coordinator requests the reservation before sending the request over target transport. |
| Instance Spot direct [cold activation](01-glossary.en.md#cold-activation) | The source first sends the first message and creation info to the target. If the target has no currently usable Spot, the target requests the Location Store for permission to create this Spot itself. |

The common result of both methods is the same. The Location Store grants creation
authority to only one target, and other targets don't start a separate
[factory](01-glossary.en.md#factory).

The unchanging name used when registering an object factory and comparing whether it's
the same type is called stable type.

Remote User Spot manager create sends a separate terminal service operation to the
exact target after reservation. This operation fixes the source and target node
lifecycle, global Spot key/[stable type](01-glossary.en.md#stable-type), the
provider-issued reservation, `StoreVersion`, and deadline. The target does an exact
read of the `Reserved` allocation's `pendingCreation` from the Location Store, then
runs factory/initialize/Commit. Location row polling or an application control packet
isn't the terminal result.

Remote User Spot close is also a separate terminal service operation carrying the
exact `SpotRef`, owner generation/`StoreVersion`, and target lifecycle. The target
checks active Actor membership and relocation state before admission.

1. The runtime fixes the global key, stable type, optional Mesh/placement, and
   durable creation input, then selects a positive-node-wide-weight candidate
   satisfying role, type capability, and typed population capacity.
2. For Actor/User Spot manager create, the coordinator calls `Reserve`. For Instance
   Spot, the source first submits a first-message activation envelope to a candidate
   target, and the target activation registry calls `Reserve`.
3. Store `Reserve` changes the object state from `Missing` to `Creating`, and fixes
   the allocation and typed capacity bundle needed to create that object on the
   target as `Reserved`, in the same transaction. This authority state change is
   called the `Missing → Creating` transition.
4. Only the target that succeeds in this reservation runs factory and initialize. A
   target that requested concurrently but failed the reservation doesn't separately
   create the same object.
5. If the creation callback approves, the Store's terminal `Commit` changes the same
   fence to `Ready`, transitions allocation and typed capacity bundle from
   `Reserved → Active`, and publishes a `Created` result.
   Instance Spot has no separate application creation approval — it submits the
   first message included in the envelope to the local queue once, after the
   activation barrier.
6. If the creation callback declines, the same terminal `Commit` doesn't create
   Ready or active capacity — it cleans up the Creating authority and reserved
   allocation/typed capacity bundle and publishes a `Rejected` result.
7. On node termination, timeout, or a callback exception, `Abort` cleans up the
   exact Creating authority and `Reserved` allocation/typed capacity bundle and
   publishes an `Aborted` failure.

The reservation carries which object to create on which target, and the information
needed to verify capacity and current owner. Precisely, it fixes object kind, global
key, stable type, target descriptor, typed capacity bundle, exact owner lease, and
`StoreVersion`. Creation authority isn't judged by a fixed expiration TTL. Recovery,
handoff to a different target, and cancellation are decided by checking the recorded
`Creating` state and target owner lease together. Actor and Spot share this common
reservation operation.

An encoded creation request is at most 1 MiB. The framework records an unchangeable
content reference and hash into the creation intent before reservation, and keeps it
until the object becomes Ready or a failed creation is cleaned up. Only the target
that obtained creation authority passes this request to the factory. Since factory and
initialize can run more than once per `(logical key, ObjectGeneration, attempt)`, they
must safely handle re-execution with the same input.

The staging instance an Actor factory builds is passed to the Entry Spot's
`OnCreateActor`. The callback returns approve/decline and an optional reply. If
approved, initial Entry membership/Ready authority/active capacity and a `Created`
terminal record are published together. If declined, Ready and message admission
aren't opened — the Creating authority/pending capacity are cleaned up and a
`Rejected` terminal record is published. A callback exception is a typed creation
failure that already exists — it isn't an application rejection.

A caller that requested concurrently but didn't obtain creation authority doesn't
start a separate factory. A different operation waits for the authority change. Once
authority becomes Ready, it receives `Existing`; if it becomes Missing via callback
rejection or failure cleanup, it competes for a new reservation to process its own
creation request. It doesn't share an earlier operation's `Rejected` state or
application reply.
One terminal call deadline applies across the whole of resolve, waiting, reservation,
factory, and [Ready](01-glossary.en.md#ready) preparation. Once the
[deadline](01-glossary.en.md#deadline) ends, it's `DeadlineExceeded`. The next call
re-checks the Store's current authority to clean up or continue an interrupted
attempt. `Missing`, `Creating`, and Store failure aren't stored in a negative cache.

If multiple processes concurrently call `GetOrCreate` for the same ActorId, only the
Location Store reservation CAS winner runs factory and `OnCreateActor`. If the same
Actor is Creating, other callers don't create a new reservation — they wait for the
authority change.

```text
Missing
  → Reserved(R1)
      ├─ Created(R1, ActorRef, ReplyRef?)
      ├─ Rejected(R1, ReplyRef?)
      └─ Aborted(R1, Failure)
```

`Created` and `Rejected` are normal
[terminal results](01-glossary.en.md#creation-terminal-result) of the reservation
winner operation. A callback exception is `Failed`; recovery cleanup is `Abort`, which
creates no terminal record. `Existing` is the lookup result of a different operation
that found a Ready Actor, and doesn't create a new reservation or callback.

A `Created` terminal publish also performs the Ready authority and active capacity
transition together. A `Rejected` terminal publish doesn't create Ready authority or
active capacity — it cleans up Creating authority and reserved capacity. A terminal
record is identified by the exact source Node RID/lifecycle generation/`OperationId`
and is only used for resends of the same operation. The
`creation-operation-terminal-v1` semantic envelope — with no request correlation or
reply route — and its SHA-256 are preserved up to 5 minutes after the original
deadline. On resend, the framework encodes a new command reply using the current
request's correlation and reply route.

An Entry Spot isn't published to descriptor and resolver before finishing startup
initialization. Actor creation completes initial Entry Spot membership and the Ready
barrier in the same lifecycle, without calling `OnActorJoin` or `OnJoinedActor`.

## 3. Actor Membership For Entry Spot And User Spot

The three Spot kinds' creation methods and functional differences, and the Entry
Spot's overall role, are defined by
[19 Spot Model](11-spot-model.ko.md). This section only defines the order in which
Entry/User Spot handles Actor membership.

An Entry Spot's Actor uses a per-Actor execution gate. In a User Spot's default
`SpotWide` mode, the Spot handler, member Actor handlers, timer, and lifecycle
callbacks share the User Spot's common execution gate. Choosing `PerActor` at factory
registration separates a per-Actor gate, Spot lane, and per-timer gate — different
gates can run concurrently.

When an Actor is created, the Entry Spot of the selected owner
[MeshNode](01-glossary.en.md#meshnode) handles initial membership. Actor business
messages are delivered directly to the Actor queue, without going through an Entry
Spot or User Spot callback.

The location where Actor payload is put on the Actor queue and the gate deciding
handler execution authority are different contracts. `Yield` is only allowed in a
`SpotWide` User Spot, which uses a shared gate. In an Entry Spot Actor and `PerActor`
User Spot, only `Async`, which keeps the current turn, is used.

The Actor Join call only provides synchronous `Defer()`, regardless of execution
mode. A handler calls `Defer()` to schedule the Join and register a barrier. The
framework runs the actual Join once the handler's last continuation ends normally.
Join doesn't provide `Async`/`await`/`submit` or `Yield`. Unlike a request or worker's
`Yield`, `Defer()` doesn't return the Spot gate or Actor queue claim.

One handler can register at most 64 Joins. One Join request is encoded to at most
1 MiB, and the sum of every Join request registered by the same handler is at most
8 MiB. Exceeding the limit fails the current registration synchronously as a startup
configuration error, without leaving any partial record.

If timeout is omitted, 5 seconds is used. An explicit value must be a finite value in
`1..INT_MAX`, rounded up to milliseconds. The framework computes an absolute deadline
once, using the monotonic clock, at the moment `Defer()` is called. So time the
handler keeps running after `Defer()` is also included in the Join timeout.

A User Spot serializes Join request processing, joined, leave, and disconnected
lifecycle control on that Spot's control queue. The order of packet/timer turns and
callbacks on the same Spot is set by the Spot turn. Instance Spot isn't an Actor
membership target.

The Actor disconnected callback runs from either the current binding snapshot of a
physical Session disconnect, or an explicit logical notification from public
`NotifyDisconnected`. The framework runs it at most once per exact binding identity,
and doesn't interpret it as Actor destroy, leave, or a membership change. One Actor
callback's failure doesn't block other binding notifications or Session cleanup.

A regular User Spot Close returns public `false` and keeps admission and authority if
even one current Actor membership remains. It can only close once the caller finishes
an explicit leave or destroy. The framework doesn't secretly move or destroy an Actor.

## 4. Actor Join And Commit Order

`JoinSpot` takes the global Spot ID of the User Spot to move to. `JoinEntrySpot`
doesn't take a target node RID. The framework finds the target Spot and owner node to
use. If the Actor's and target Spot's owner node differ, Actor relocation is also
performed within the same Join operation.

The application doesn't directly specify relocation stage, target node, state
adapter, or owner token. The framework decides these values based on current
configuration and authority.

The following C# excerpt is a .NET expression to help understand the common join
behavior. It doesn't require the same signature in other languages; the exact full
contract is defined by the
[.NET Actor interface](server/languages/dotnet/interfaces/06-actors.ko.md).

```csharp
public interface IZLinkActorContext
{
    IZLinkActorJoinSpotCall JoinSpot(
        string spotId,
        ZLinkMessage request);

    IZLinkActorJoinEntrySpotCall JoinEntrySpot(
        ZLinkMessage request);
}

public interface IZLinkActorJoinSpotCall : IZLinkActorJoinCall
{
    IZLinkActorJoinSpotCall Timeout(TimeSpan timeout);
}

public interface IZLinkActorJoinCall
{
    void Defer();
}
```

A minimal example of an Actor handler joining a specific User Spot is as follows. The
application only specifies the [Spot ID](01-glossary.en.md#spot-id) and the request
needed for the admission decision. The framework finds the current owner, and if it's
on a different node, performs relocation within the same operation.

```csharp
Context
    .JoinSpot(targetSpotId, ZLinkMessage.From(joinRequest))
    .Timeout(TimeSpan.FromSeconds(5)) // applies to the whole of Join plus any needed relocation.
    .Defer(); // registers the Join to run once the current handler ends normally.
```

The Join request is optional. If omitted, an empty request is passed to the target
User Spot's `OnActorJoin` callback. Once `Defer()` is called, the framework stores an
unchangeable copy of the request and an absolute deadline. This request is only used
to judge Join admission — it isn't reused as a relocation state payload.

`Defer()` can only be called while the current handler's registration scope is open.
An awaited continuation the framework tracks also uses the same scope. Calling it
after the handler ends and the scope is closed is `InvalidOperation`. Calling it from
a background detached task, without waiting for work the handler started, is an
application contract violation. The framework doesn't guarantee catching such a call
before the scope closes, in every language.

Once the handler ends normally, the framework activates every registered barrier. If
the handler ends with an exception, cancellation, or request reply encoding failure,
every barrier is discarded. Once reply encoding finishes, Join isn't canceled even if
the caller closed the connection or transport couldn't accept the reply.

The framework notifies the application of the Join result via an Actor Join
completion callback, together with a non-zero 128-bit `OperationId`. This callback
delivers the final result of a Join that proceeded asynchronously after the handler
ended. `Accepted` is received by the target Actor that committed the location change.
`Rejected` and `Failed` before commit are received by the existing source Actor. If
the target process terminates after commit, the completion callback isn't re-run on a
different runtime.

Before the target's `OnJoinedActor` callback finishes, the completion callback and
application payload waiting behind it aren't run. The source's `OnLeaveActor`
notification is sent one-way. This notification's completion or failure doesn't
block completion. A separate cleanup stage for resources left on the source isn't
added to the Join procedure.

Regardless of whether a bound Session exists, once the target's `OnJoinedActor`
callback ends, the target Actor receives the `Accepted` result via the Join
completion callback. Once this callback ends, the target Actor processes pending
messages. If there's a bound Session, after the Join completion callback the target
runtime sends the Session owner `sessionActorLocationUpdateReqMsg` to request an
update to the binding route and current `ActorRef` location snapshot. This update
doesn't block Join completion or Actor message processing. Once the session owner
finishes the update, it returns `sessionActorLocationUpdateResMsg` as a separate send
message. If there's no response, the target runtime resends the same request, and
keeps trying the update until it gets a response or the Session or binding ends. A
message arriving on the pre-update route is delivered to the target Actor by the
source's Message Follow route.

The completion, `OperationId`, optional reply, and retry cursor of a same-node and
cross-node Join are preserved only while the current source and target processes are
running. The Relocation Store payload is used to deliver state and queue to the
target during a normal handoff, and isn't used as the basis to automatically replay
completion after process termination.

`OperationId` is the idempotency ID the application uses to distinguish completion
callback retries as the same work. It's a different value from `RelocationId`, which
identifies the whole relocation, the placement reservation ID, or an aggregate commit
ID confirming several Store entries together. It's also stored as a separate field in
a cross-node `Accepted`'s relocation manifest.

| Completion outcome | Actor that runs the callback | Information the application receives |
|---|---|---|
| `Accepted` | The target Actor that committed the location change receives it. For a same-target no-op, the current Actor receives it. | Receives the current `ActorRef` and the optional reply the target User Spot's `OnActorJoin` callback returned. |
| `Rejected` | The existing source Actor receives it. | Receives the optional reply the target User Spot's `OnActorJoin` callback returned. |
| `Failed` | Before commit, the source Actor receives it. If it fails on the same target runtime after commit, the target Actor may receive it. If the process terminates, the callback isn't re-run on a different runtime. | Receives a typed framework error kind. |

The error kind `Failed` delivers distinguishes the point of failure as follows. A
result where the target's `OnActorJoin` callback normally declines isn't an error, so
it's `Rejected`, not `Failed`.

| Point of failure | `Failed.Kind` |
|---|---|
| The requested User Spot can't be found. | `NotFound` |
| There's no Entry Spot to move to, or no compatible target node. | `Unavailable` |
| The target node's remaining capacity is insufficient. | `CapacityExceeded` |
| The Actor's relocation policy forbids cross-node moves. | `Rejected` |
| The location change wasn't committed by the deadline. | `DeadlineExceeded` |
| Capture/factory/restore/staging fails with an internal error. | `InternalFailure` |
| The durable relocation payload is missing or fails verification. | `DataLost` |
| The Actor generation differs from the current value. | `InvalidOperation` |
| The owner or membership fence differs, or the Actor is moving. | `Unavailable` |
| Runtime shutdown started first and interrupted before commit. | `ShuttingDown` |

`Accepted` means location and membership change was committed. It doesn't mean the
completion callback execution has finished too. The framework runs the completion
callback after processing the lifecycle callback and source membership cleanup. If
completion keeps failing, the Actor is kept sealed, and regular messages behind the
barrier aren't run.

A same-node join isn't relocation, so it's allowed even if the relocation policy is
`DisableRelocation`.

If the Actor already belongs to the requested User Spot, or an Entry Spot Actor calls
`JoinEntrySpot` again, the framework submits an `Accepted` completion with no actual
move. In this case the Location Store, membership, and capacity aren't changed. It
also doesn't call `OnActorJoin`, `OnJoinedActor`, or `OnLeaveActor`.

If Join and host maintenance start at the same time, whichever sealed or claimed the
work first takes priority. If the Join claim comes before `Relocate`, maintenance
waits until Join reaches a terminal state. If the `Relocate` seal comes first, Join
ends with `Unavailable`. If the Shutdown admission seal comes first, Join ends with
`ShuttingDown`.

If the same handler that registered a barrier sends a request to that Actor and
waits for the reply, a cycle can form where the request and handler each wait for the
other to finish. The framework rejects it with `InvalidOperation` before submitting
the request to the queue.

### 4.1 Comparing Entry Spot And User Spot Callbacks

Entry Spot and User Spot are different Spot instances. A User Spot decides whether to
accept an Actor in `OnActorJoin`. Entry Spot has no such callback. In a regular
same-node membership change, once committed, it runs the target Spot's
`OnJoinedActor` and the source Spot's `OnLeaveActor`. In a cross-node Join, it first
processes the restore request and source relay. After the target restore and
membership commit finish, it calls the target Spot's `OnJoinedActor`, and sends the
source Spot `OnLeaveActor` one-way.

When first placing a new Actor into an Entry Spot, the Entry Spot's `OnCreateActor`
is used. When moving from Entry Spot to User Spot, the target User Spot's
`OnActorJoin` decides admission. When returning from User Spot to Entry Spot,
membership is committed with no admission decision. Both regular moves use the
target's `OnJoinedActor` and the source's `OnLeaveActor` after commit.

```mermaid
%%{init: {"theme": "base", "themeVariables": {"primaryTextColor": "#111827", "secondaryTextColor": "#111827", "tertiaryTextColor": "#111827", "textColor": "#111827", "lineColor": "#374151", "actorBkg": "#ffffff", "actorBorder": "#111827", "actorTextColor": "#111827", "signalColor": "#111827", "signalTextColor": "#111827", "labelBoxBkgColor": "#ffffff", "labelBoxBorderColor": "#111827", "labelTextColor": "#111827", "noteBkgColor": "#ffffff", "noteBorderColor": "#374151", "noteTextColor": "#111827"}}}%%
sequenceDiagram
    participant E as Entry Spot
    participant F as Framework
    participant U as User Spot

    rect rgb(235, 245, 255)
        Note over E,F: Initial membership of a new Actor
        F->>E: call OnCreateActor
        alt approved
            E-->>F: return Accepted with an optional reply
            F->>F: confirm the Actor and Entry membership as Ready
        else declined
            E-->>F: return Rejected with an optional reply
            F->>F: clean up the staging Actor and reservation
        end
    end

    rect rgb(240, 255, 240)
        Note over E,U: Moving from Entry Spot to User Spot
        F->>U: call OnActorJoin (passing Actor ID and join request)
        alt approved
            U-->>F: return Accepted with an optional reply
            F->>F: confirm User Spot membership
            F->>U: call OnJoinedActor
            F->>E: call OnLeaveActor
        else declined
            U-->>F: return Rejected with an optional reply
            F->>F: keep Entry Spot membership
        end
    end

    rect rgb(255, 245, 235)
        Note over E,U: Returning from User Spot to Entry Spot
        F->>F: confirm Entry Spot membership
        F->>E: call OnJoinedActor
        F->>U: call OnLeaveActor
    end
```

So an Actor returning from a User Spot to an Entry Spot isn't a new Actor. The target
Entry Spot doesn't call `OnCreateActor` or `OnActorJoin` — it only runs
`OnJoinedActor`, while the source User Spot runs `OnLeaveActor`.

### 4.2 The Order For Joining An Actor To A Spot On A Different Node

Once an Actor handler calls `JoinSpot(...)` or `JoinEntrySpot(...)` and calls
`Defer()` on the returned call object, the framework runs the Join in the following
order after the handler ends normally.

1. The handler calls `JoinSpot(...)` or `JoinEntrySpot(...)` and executes `Defer()`.
   `Defer()` only registers the Join. Before the handler ends normally, no Actor is
   created and no message is sent.
2. Once the handler ends normally, the framework confirms the target. If the target
   is a User Spot, it passes `ActorId` and the join request to the target's
   `OnActorJoin`. If `Accepted`, it continues; if `Rejected`, it ends while keeping
   source membership. If the target is an Entry Spot, `OnActorJoin` isn't called.
3. The framework checks the relocation policy and target capacity. If the move can
   proceed, it briefly blocks new message processing on the source Actor and stores
   application state and the current Actor queue in `RelocationStore`. If it's
   `DisableRelocation`, it's rejected at this stage.
4. The source runtime **sends the Actor Restore request to the target runtime
   first**. Before dispatching the next packet, the target dispatcher registers a
   [relocation temporary queue](01-glossary.en.md#relocation-temporary-queue) for
   the ActorId and `ObjectGeneration`. Afterward, a message arriving for the same
   Actor goes into the temporary queue without looking up the application instance.
   The target creates the Actor and reads application state and the saved existing
   queue, but doesn't run application work yet.
5. A message arriving after the source seal is held in the source runtime's
   size-bounded `ingress hold`. After sending the Restore request, the source
   runtime relays the hold's messages and later messages arriving on the previous
   route to the target. The target dispatcher also puts these messages in the same
   temporary queue.
6. Once the target finishes Actor Restore, it updates target membership, owner,
   capacity, and generation in `LocationStore`, all at once. Once this commit
   succeeds, the target becomes the new owner. The source keeps the hold's original
   until it receives the target's dispatch-switchover completion.
7. After commit, it calls the target Spot's `OnJoinedActor` and sends the source
   Spot `OnLeaveActor` one-way. It then calls the Actor's Join completion callback.
   Once the completion callback ends, saved existing Actor work is put into the real
   Actor queue first, then the temporary queue's work moves in behind it. Then the
   temporary queue registration is removed and it switches to the existing dispatch
   path. After this switchover, the target Actor processes messages. A Session
   location update response doesn't block Actor message processing.
8. If the Actor is bound to a Session, the target runtime sends
   `sessionActorLocationUpdateReqMsg` to the session owner after calling the Join
   completion callback. The session owner atomically changes that Actor's binding
   route and current `ActorRef` location snapshot to the target location and sends
   `sessionActorLocationUpdateResMsg`. If there's no response 1 second after the
   first send, the target runtime resends the same request for the first time.
   Subsequent resend intervals are 1, 2, 4, 5 seconds, then stay at 5 seconds. The
   session owner must keep the same result even after receiving the same request
   multiple times. The route and physical STREAM connection of other Actors on the
   same Session don't change.

Since `Accepted` and `Rejected` are mutually exclusive results that don't happen at
the same time, they're split with `alt` in the diagram. Whether a bound Session
exists is optional, so only that part is marked `opt`.

```mermaid
sequenceDiagram
    participant Handler
    participant SourceRuntime as Source runtime
    participant SourceActor as Source Actor
    participant SourceSpot as Source Spot
    participant RelocationStore as Relocation Store
    participant TargetRuntime as Target runtime
    participant TargetTemp as Actor temporary queue
    participant TargetQueue as Target Actor queue
    participant TargetSpot as Target Spot
    participant LocationStore as Location Store
    participant TargetActor as Target Actor
    participant SessionOwner as Session owner

    Handler->>SourceRuntime: call JoinSpot(...) then Defer() on the returned object
    Handler-->>SourceRuntime: handler ends normally
    Note over SourceRuntime,TargetSpot: the flow below is for a User Spot target
    SourceRuntime->>TargetSpot: call OnActorJoin
    alt Accepted
        TargetSpot-->>SourceRuntime: return Accepted with an optional reply
        SourceRuntime->>SourceActor: block new messages on the source Actor
        SourceRuntime->>RelocationStore: store state and the current queue
        SourceRuntime->>TargetRuntime: send the Actor restore request first
        TargetRuntime->>TargetTemp: register the Actor relocation temporary queue
        TargetRuntime->>TargetActor: create the Actor and Restore application state
        SourceRuntime->>TargetRuntime: relay ingress hold messages
        TargetRuntime->>TargetTemp: add message to the temporary queue
        TargetRuntime->>LocationStore: update membership/owner/capacity/generation
        LocationStore-->>TargetRuntime: target owner confirmed
        TargetRuntime->>TargetSpot: call OnJoinedActor
        SourceRuntime-)SourceSpot: call OnLeaveActor (one-way)
        TargetRuntime->>TargetActor: deliver Accepted via the Join completion callback
        TargetRuntime->>TargetQueue: move temporary queue work behind existing work
        TargetRuntime->>TargetTemp: remove the temporary queue, switch to existing dispatch
        TargetRuntime-->>SourceRuntime: signal dispatch switchover complete
        TargetQueue->>TargetActor: process messages in queue order
        opt if a bound session exists
            TargetRuntime-)SessionOwner: send sessionActorLocationUpdateReqMsg
            SessionOwner->>SessionOwner: swap the binding route and current ActorRef snapshot
            SessionOwner-)TargetRuntime: send sessionActorLocationUpdateResMsg
            Note over TargetRuntime,SessionOwner: without a ResMsg, resend the same ReqMsg at 1s, 1s, 2s, 4s, then 5s intervals
        end
    else Rejected
        TargetSpot-->>SourceRuntime: return Rejected with an optional reply
        SourceRuntime->>SourceActor: keep existing source membership
    end
```

This diagram shows only the path that ends normally. If `OnActorJoin` returns
`Rejected` or fails before commit, source membership is kept. `OnLeaveActor` is only
sent after commit, so it isn't called on a pre-commit failure. A target that received
the Restore request first registers a relocation temporary queue. Messages and
requests arriving in that time wait in the temporary queue and aren't run before
moving to the real Actor queue. If it fails after target commit, it isn't rolled back
to the source. Only while the same target process is running is it retried within
the deadline; if the process terminates, relocation isn't automatically taken over.

If a reject, timeout, `Capture`/`Restore` failure, or aggregate commit conflict
happens before commit, the target application instance isn't exposed. The relay
record the target received is a staging copy, so it's discarded from the temporary
queue without running or creating a terminal result. The source restores the ingress
hold's requests and one-way messages to the original Actor queue in arrival order.
Once the queue is empty, that temporary queue registration is removed. Source owner,
state, and membership are kept unchanged throughout. If a failure happens after
commit, it isn't rolled back to the source. If the same target process is running, it
can retry within the deadline using the confirmed location information and stored
payload. If the target process terminates, a different runtime doesn't automatically
recover it.

[ObjectGeneration](01-glossary.en.md#objectgeneration) is kept unchanged. Since the
owner changes via a cross-node move, only `AuthorityOwnerGeneration` increases. The
target Context uses the existing `ObjectGeneration` and the new owner generation.
Once the bounded aggregate commit succeeds, it blocks the source Context from
executing any further operations.

A message arriving after `Defer()` but before the source seal is stored in
`RelocationStore` together with the current Actor queue. A message arriving after the
source seal is temporarily held in a size-bounded ingress hold. The source runtime
keeps relaying the hold's records and later records arriving on the previous route to
the target temporary queue. If interrupted before commit, the hold's records are
restored to the source queue in arrival order, and the target temporary queue is
discarded. If commit succeeds, the temporary queue's records move in behind the saved
existing work. The source removes the hold original after receiving the target's
dispatch-switchover completion.

In an application-requested User Spot join, the target User Spot's `OnActorJoin`
first decides admission. In a cross-node Join, after the restore request and source
relay, the target restore and membership commit finish. Then the target's
`OnJoinedActor` is called and the source's `OnLeaveActor` is sent one-way.
When returning from User Spot to Entry Spot, `OnActorJoin` isn't called — membership
is committed directly. Afterward, the target Entry Spot's `OnJoinedActor` and source
User Spot's `OnLeaveActor` are called. These callbacks are only used for an
application-requested logical membership change.

The Entry Spot itself isn't a relocation participant. When Host `Relocate` moves a
source Entry Spot's Actor to the target node's Entry Spot, the framework restores
state via the Actor adapter. Owner, membership, queue, timer, and session route also
move to the target. This infrastructure relocation doesn't call the target's
`OnJoinedActor` or the source's `OnLeaveActor`. A dedicated relocation application
callback also isn't provided. During target Actor dispatch, messages arriving during
Restore are held in the relocation temporary queue. After commit, journal, saved
queue, and timers are put into the real Actor queue first, then the temporary
queue's messages move in behind them. After the switchover, Message Follow and
target direct messages use the existing Actor queue path.

A Spot's terminal lifecycle callback is `OnClosing(ClosingContext)`. Since an Actor
always belongs to an Entry or User Spot, a separate per-Actor closing callback isn't
provided. `ClosingContext` provides the following closed reasons and the operation's
absolute deadline.

| Value | Reason | Call condition |
|---:|---|---|
| 0 | `ExplicitClose` | The application starts a User/Instance Spot close, normally cleaning up that local instance. |
| 1 | `HostShutdown` | Host `Shutdown`, without relocation, cleans up a local Entry/User/Instance Spot. |
| 2 | `RelocationOut` | The source local instance is cleaned up after a User/Instance Spot owner commit. |

A standalone Actor move doesn't close the Entry Spot itself, so it doesn't call the
Entry Spot's `OnClosing`. Infrastructure relocation also doesn't call Actor
membership callbacks. If Actor membership remains on a User Spot and explicit close
is rejected, `OnClosing` isn't called. On host `Shutdown`, after bringing accepted
handler and timer turns to a terminal state, Spot `OnClosing` is called while Actor
membership and the local instance are still valid. After callback completion, the
Actor/Spot scope is disposed and Location authority and resources are cleaned up.

If the language runtime has a standard cooperative cancellation expression, the
remaining cleanup budget can be passed to the callback along with it. A separate
framework cancellation type just for Spot closing isn't created. In a language with
no standard expression, only `ClosingContext`'s deadline is passed, and the framework
ends the wait for callback completion at the deadline. The application doesn't keep
the context or cancellation signal after the callback. `HostShutdown` doesn't start
relocation or rollback due to callback failure. A callback exception ends as
`ForceStopped/TeardownFailed`; deadline expiry ends as
`ForceStopped/DeadlineExceeded`. Callback execution isn't guaranteed on process crash
or `SIGKILL`. The exact enum, context, and standard cancellation expression are set
by each language's interface document.

## 5. Relocation Policy Shared By Every Move Path

An Actor's/User Spot's/Instance Spot's
[Object Server](01-glossary.en.md#object-role) factory must register one of the
following policies.

| Policy | Meaning |
|---|---|
| `DisableRelocation` | Rejects cross-node relocation before capture and keeps the source owner and admission. |
| `RecreateOnRelocation` | Runs the target factory and keeps framework queue/timer information, but doesn't deliver an application state payload. `ObjectGeneration` is kept even though a new application object is built, since it's the same logical incarnation. |
| `PreserveStateWith` | Captures application state, at the boundary where the handler ended normally, into an opaque byte sequence via a relocation adapter matching the object kind, and restores it on the target. Framework queue/timer information is also kept. |

Actor uses `ActorRelocationAdapter`. `SpotWide` User Spot and Instance Spot use
`SpotRelocationAdapter`. Since a `PerActor` User Spot's Spot shell doesn't move
application state, only the `RecreateOnRelocation` policy is allowed, and
registering a Spot adapter is a startup configuration error.

Both adapters' operation names are `Capture` and `Restore`. `Capture` takes the
source instance and returns a byte sequence; `Restore` takes the instance the target
factory built and the byte sequence, and applies the state. It doesn't return an
instance.

The application manages byte format, version, compatibility, and migration. The
framework doesn't add a state contract ID, generic state type, serialization
profile, or message codec to the relocation adapter contract. The Relocation Store
stores application bytes as-is as an opaque payload, and the framework only verifies
its own root manifest/chunk/checksum.

The byte sequence `Capture` returns for one participant is at most 64 MiB. An empty
byte sequence is valid application state, and a null result is an adapter contract
violation. Once the callback succeeds, the framework immediately copies the result or
takes ownership of it, so the application doesn't change the result afterward. Bytes
passed to `Restore` are only valid until the callback completes — the callback must
copy them itself to keep them.

Join and host maintenance use the same factory relocation configuration and adapter
registration. The Actor adapter is called when an Actor that chose
`PreserveStateWith` joins a User Spot/Entry Spot on a different node, or moves via
maintenance. In User Spot aggregate relocation, the adapters of a Spot registered
with `PreserveStateWith` and each member Actor are called separately. The adapter
isn't called on a same-node join, a `DisableRelocation` rejection, or
`RecreateOnRelocation`. A per-operation policy, an omission overload, and a separate
adapter registry aren't provided.
Policy and adapter registration don't change after startup.

The exact stages and sequence diagrams for moving an Entry Spot Actor,
`PerActor`/`SpotWide` User Spot, and Instance Spot in host relocation are defined by
[Graceful Drain And Handoff §8](28-graceful-drain-handoff.en.md#8-the-order-for-relocating-one-unit).

## 6. Maintenance Aggregate Moving A User Spot And Member Actors Together

When host `Relocate` moves a User Spot, it treats that Spot and every current member
Actor at seal time as one aggregate. The application doesn't choose which
participant or relocation phase to include in the aggregate.

Once the host transitions to `Relocating`, the framework schedules an infrastructure
intent notification on the aggregate's Spot control queue. This notification isn't
an application callback. If a permit isn't obtained at the turn boundary where the
notification is processed, it doesn't seal — it schedules the next notification
instead, so the Spot and member Actors keep processing application messages and
timers.

Aggregate ID is a non-zero 128-bit value. There's no fixed cap on the total number
of Actors that can be included in an aggregate. The actual total is limited by the
membership existing on the source and the population capacity the target advertised.

The framework doesn't put every participant into one record. It sorts object kind,
global key, ObjectGeneration, owner fence, and policy, then stores it in the Location
Store as several immutable inventory chunks. One leaf chunk stores at most 1,024
entries, and its encoded size doesn't exceed 1 MiB. If the list doesn't fit in one
leaf, an index chunk is added to build a tree. Aggregate authority holds only the
following values.

| Value | Purpose |
|---|---|
| `AggregateId` and generation | Identifies the same User Spot move and its commit generation. |
| Participant count | The total number of Spot and Actors in the tree. |
| Inventory root and digest | Points to the whole list the Location Store uses as authority. |
| Owner and relocation root | Points to the current owner and the payload to restore. |

```mermaid
flowchart LR
    Members["The User Spot and every member Actor"] --> Split["split into groups of at most 1,024"]
    Split --> C1["Inventory leaf 1"]
    Split --> C2["Inventory leaf 2"]
    Split --> CN["Inventory leaf N"]
    C1 --> Root["Inventory root<br/>count and digest"]
    C2 --> Root
    CN --> Root
    Root --> CAS["Aggregate authority CAS"]
    CAS --> Visible["The Spot and every Actor<br/>use the new owner"]
```

The current owner of an Actor belonging to a `SpotWide` User Spot follows the User
Spot aggregate authority. A per-Actor membership record points at that aggregate and
doesn't publish owner one at a time during relocation.

1. At a Spot queue turn boundary, once the aggregate's active unit, callback, and
   expected payload byte permit are all obtained, the source User Spot's join/leave
   and every participant's admission are reversibly sealed.
2. The exact participant inventory is stored as an immutable tree, and root/count/
   digest are verified.
3. Every relocation configuration, target type/state-preservation adapter
   capability, and active/pending capacity are preflighted.
4. Every `PreserveStateWith` participant's state, not-yet-executed message queue,
   accepted journal, and timer logical registration/pending tick are captured, and
   target reservation/factory/restore are prepared with admission closed.
5. A single CAS in the Location Store transitions aggregate owner, generation,
   inventory root, and capacity. Once this CAS succeeds, the Spot and every member
   Actor use the new owner together.
6. After the authority commit, target lifecycle callback, accepted message/journal
   replay, and automatic framework timer restore finish, and the target User Spot
   and member Actors start processing messages. For each bound Actor in the
   aggregate, the target runtime sends the session owner
   `sessionActorLocationUpdateReqMsg` requesting that route be changed to the
   target. Along with the route switch, each bound-session's current Actor
   location snapshot is also updated to the target MeshName/NodeRid, keeping the
   same ActorId/ObjectGeneration. The route and physical STREAM connection of
   Actors outside the aggregate on the same Session are kept. Once the session
   owner finishes the update, it sends `sessionActorLocationUpdateResMsg`. Without
   a response, the target runtime resends the same request starting 1 second after
   the first send, at intervals of 1, 2, 4, 5 seconds, then stays at 5-second
   intervals. The target User Spot and member Actors keep processing messages
   while waiting for the response.

Step 4's restore must finish before step 5's aggregate commit. Since a `SpotWide`
User Spot aggregate moves logical membership as-is, it doesn't call application
membership callbacks for member Actors. Only the Spot/Actor adapters' restore and
Spot lifecycle callback finish before target admission.

Before commit, the new inventory tree and target staging aren't visible to the
resolver. If even one participant fails before commit, target staging is discarded
and the whole aggregate's source state is kept. After commit, it doesn't roll back
just some participants to the source — it keeps the same aggregate identity,
inventory root, and relocation root. Only while the same target process is running
is the whole aggregate continued; if the process terminates, a different runtime
doesn't take over.

A `PerActor` User Spot doesn't use an aggregate owner change. The framework prepares
a runtime-private Spot shell on the target, finishes the Spot queue's current turn
and in-progress Create/Join, then CASes the Location Store's Spot authority to the
target. The public SpotId and ObjectGeneration don't change — a temporary public
SpotId isn't created and the SpotId isn't reassigned after target activation.

After the Spot authority commit, new `ToSpot`, Actor Create, and Join go to the
target. The source shell only runs handler and relocation control for Actors still
left on the source. Each Actor is an independent relocation unit — after sealing the
Actor queue, it moves state, the not-yet-executed queue, accepted journal, timer,
session binding route, and bound-session current Actor location snapshot to the
target. The snapshot keeps the same ActorId/ObjectGeneration and provides the target
MeshName/NodeRid. Once a per-Actor owner CAS succeeds, a message arriving at the
previous owner is relayed to the target with the same operation identity,
ObjectGeneration, deadline, request correlation, and reply route.

Once the last Actor becomes a target owner and the source has finished all already-
accepted Spot work and relay, the source shell closes with `RelocationOut`. During
relocation, some Actors may be on the source and some on the target. This
distributed state is only allowed within the same relocation operation — in steady
state, Spot authority and every member Actor owner must be the same.

If a `SpotWide` User Spot uses the application-signaled relocation boundary,
`RelocationReady().Defer()` registers a framework-owned barrier after the current
turn. The framework calls the Spot's default no-op `OnRelocationReadyCompleted`
callback on the current owner that confirmed whether to move. This callback isn't an
Actor membership change callback and isn't delivered to member Actors. An
application that overrides the callback can start the next round or match here.

## 7. Failure-Handling Scope

A failure before commit finishes an `Aborted` CAS, confirms route and source
location snapshot cancellation, cleans up relocation root/reservation, and restores
source state before reopening source admission. If a Location Store change's result
isn't received, success or failure isn't guessed. Source admission isn't reopened
before re-reading the same authority to confirm the source is owner.

If `Capture` fails, the relocation payload isn't linked to the current authority. If
`Restore` fails, the target staging instance and temporary queue are discarded. If
the same source and target processes are still running and the deadline remains, a
new instance can be created to retry Restore with the same payload. A different
target isn't automatically selected — if it doesn't succeed by the deadline, the
source is kept and it ends with `StateIncompatible` or a `Failed` result matching
the cause.

A failure after the owner and membership commit isn't a source-rollback condition.
While the same target process is running, lifecycle callback or dispatch switchover
can be retried within the deadline, with target admission closed. If the source or
target process terminates, a different runtime doesn't take over the relocation. If
the target terminates after commit, authority keeps the target, but the object
becomes unavailable. Automatic target replacement and relocation resumption after a
process restart are defined separately in a future version.

Because of retries within the same process, factory, `Restore`, and lifecycle
callback can be called more than once. A callback must converge even when it
receives the same object generation and input again, and must not assume
exactly-once external side effects. A previous owner that resumes after a process
pause can't perform message, timer, phase update, or cleanup work, due to a stale
[AuthorityOwnerGeneration](01-glossary.en.md#authority-owner-generation), owner
lease, and local admission deadline.

## 8. Message Follow

After commit, within `MessageFollowDuration`, the source only relays a stale route
using the committed source→target Message Follow route. The relay doesn't read the
Store or run an application handler — it preserves the original operation ID,
generation, payload, and reply route. If a bound Session's
`sessionRelocationRouteUpdate` is in progress, that Actor's Message Follow route is
also removed once it receives `sessionActorLocationUpdateResMsg` or
`MessageFollowDuration` ends. Since the target runtime separately keeps resending
the session location update, the Message Follow route isn't kept indefinitely, and
the source host's Shutdown doesn't wait for this response either. Once the route
expires, a request arriving on the previous route ends with `Unavailable`.

The Message Follow route exactly verifies the global key, ObjectGeneration,
source/target AuthorityOwnerGeneration, and [owner fence](01-glossary.en.md#owner-fence).
Owner generation increases per hop, up to 8 hops max. One route's queue is at most
1024 messages and 16 MiB, and also respects the negotiated message bound. Message
Follow duration expiry, no route, and a loop are `Unavailable`; generation mismatch
is `InvalidOperation`; exceeding the bound is `CapacityExceeded`. The framework
doesn't hidden-retry a failed operation against a fresh owner.
This `ObjectGeneration` check confirms the relocation route belongs to the same
incarnation. A regular Actor/Spot message's target is the logical ID, and a
generation mismatch doesn't restrict the handler target.

A User Spot aggregate's Spot and member Actor Message Follow routes are registered
under the same commit generation.

## 9. Bound Session

Even when an Actor moves, the physical STREAM connection, session identity, and
ObjectGeneration are kept. Once owner/membership commit and Actor restore finish,
the target Actor starts message processing. If moved via Join, the target runtime
first calls the Join completion callback. The target runtime then sends the session
owner `sessionActorLocationUpdateReqMsg` requesting that Actor's
[binding route](01-glossary.en.md#binding-route) and current `ActorRef` location
snapshot be changed to the target. The session owner verifies the
[binding token](01-glossary.en.md#binding-token), AuthorityOwnerGeneration, and
sequence barrier, then atomically updates both values and sends
`sessionActorLocationUpdateResMsg`. Even if multiple Actors are bound on one
Session, the route of an Actor that didn't move isn't changed.

Once the route switch succeeds, the current `ActorRef` location snapshot the
bound-session API returns is also updated to match the same transition. The returned
snapshot keeps `ActorId` and `ObjectGeneration` and provides the target `MeshName`
and `NodeRid`. Each `ActorRef` value is an immutable snapshot, but a bound-session
accessor like `IZLinkSessionActor.Ref` or `ZLinkSessionActor.ref()` must return the
current snapshot after a route switch. The application doesn't rebind to learn about
an Actor relocation.

The two messages aren't a synchronous transport request/reply — they're
independent send packets. The target runtime proceeds with Actor message processing
and Join completion without waiting for the response. If
`sessionActorLocationUpdateResMsg` isn't received, the same
`sessionActorLocationUpdateReqMsg` is resent for the first time 1 second later. If
there's still no response, it resends at intervals of 1, 2, 4, 5 seconds, then keeps
a 5-second interval afterward. The session owner processes a request with the same
relocation ID and binding generation idempotently. Even before the response is
received, the Message Follow route only delivers a message arriving on the previous
route to the target Actor up to `MessageFollowDuration`. Afterward the request ends
with `Unavailable`, but the target runtime's location-update resend continues on the
running target runtime. If the target runtime terminates, a different runtime
doesn't automatically take over resending the same request.
A packet, reply, push, or close of a previous owner generation, binding token, and
sequence isn't applied to the current binding. A route update is only allowed for a
relocation matching the bound ObjectGeneration — a new incarnation under the same
ActorId needs an explicit bind.

The two messages' fields, duplicate handling, and resend-stop conditions are defined
by
[Session-Actor Dispatch §5.1](20-session-actor-dispatch.en.md#51-session-actor-location-update-message).

## 10. Implementation And Contract-Test Verification Requirements

- An object role doesn't start up without a Store and doesn't create a hidden local
  manager.
- A creation reservation atomically fixes global key authority and pending
  capacity.
- Even when the same Actor creation is requested concurrently, only the reservation
  CAS winner runs factory and the creation callback, while the loser waits for the
  authority change.
- A different operation receives `Existing` once Ready, and competes for a new
  reservation after cleanup — only a resend with the same source lifecycle/
  `OperationId` replays the terminal.
- `Rejected` and `Aborted` don't create Ready authority or active capacity — they
  return pending capacity.
- A terminal record allows replay of the same operation for 5 minutes after the
  original deadline, and if there's no Ready authority after TTL, it can be
  re-created via a new reservation.
- The target User Spot's `OnActorJoin` runs before `Capture`, and a pre-commit
  failure keeps the whole source.
- Actor join doesn't provide `Yield`, regardless of execution mode.
- `Defer()` leaves only Join registration and an inactive barrier on the current
  handler, without a target lookup or Store I/O, and runs once the handler's last
  continuation ends normally.
- If the handler fails, every barrier that handler registered is discarded.
- Applies limits of 64 Joins per handler, 1 MiB per request, and 8 MiB total
  requests, and an exceeded registration fails synchronously with no partial
  record.
- If timeout is omitted, uses 5 seconds and fixes a monotonic absolute deadline at
  `Defer()` time.
- Rejects `Defer()` after the registration scope closes, and treats a call from a
  detached task as an application contract violation.
- A `SpotWide` member Actor's request/worker `Yield` keeps the Actor queue claim,
  so the continuation completes before the same Actor's next job.
- An awaited request, from the same handler, to an Actor with a barrier is rejected
  with `InvalidOperation`.
- In a race between Join and Relocate/Shutdown, whichever claim/seal is confirmed
  first decides wait, `Unavailable`, or `ShuttingDown`.
- A same-target User Spot Join and an Entry Spot Actor's `JoinEntrySpot` complete
  with `Accepted` and no Store mutation or lifecycle callback.
- A reply encoding failure discards the barrier, but a caller disconnect or
  transport admission failure after encoding doesn't cancel the Join.
- A cross-node join uses the shared factory policy, and a same-node join isn't
  blocked by `DisableRelocation`.
- Actor `ObjectGeneration` is kept in same-node Join, cross-node Join, and
  `RecreateOnRelocation`, and `AuthorityOwnerGeneration` only increases on a
  cross-node owner change.
- Actor authority, source/target membership, capacity, and aggregate generation
  are confirmed in one bounded aggregate commit, and the same aggregate isn't
  committed again for post-processing.
- Same-node and cross-node Join completion is only delivered while the source and
  target processes are running. Completion replay after a process restart isn't
  guaranteed.
- The public [Actor Join `OperationId`](01-glossary.en.md#actor-join-operation-id)
  is only used for completion idempotency, and `RelocationId`, reservation ID, and
  aggregate commit ID aren't reused.
- A message after `Defer()` but before the source seal goes in the Actor queue
  behind the barrier; only a post-seal message is held in the
  [bounded ingress hold](01-glossary.en.md#relocation-ingress-hold).
- A cross-node Join's target dispatcher registers the relocation temporary queue
  before the Actor instance. A message in this queue during Restore doesn't run an
  application handler.
- Saved existing Actor work is put into the real Actor queue first, then the
  temporary queue's work moves in behind it, then it atomically switches to the
  existing dispatch path.
- On an abort before commit, the target temporary queue is discarded without
  running, and only the source original is reprocessed.
- A duplicate Restore with the same `RelocationId`, target attempt, and owner
  generation doesn't restart the work — it uses the existing temporary queue and
  progress state.
- After the membership commit, the order of `OnJoinedActor`, one-way
  `OnLeaveActor`, and completion callback is kept, and regular messages run after
  the completion callback.
- `PreserveStateWith` restores application state at the handler-end boundary along
  with framework queue/timer; `RecreateOnRelocation` restores only framework
  queue/timer, without application state.
- A User Spot and member Actors transition together under one generation of the
  [bounded aggregate commit](01-glossary.en.md#bounded-aggregate-commit).
- A post-commit failure doesn't roll back just some participants to the source.
- Message Follow only uses a bounded committed route and preserves
  [operation identity](01-glossary.en.md#operation-identity).
- A bound STREAM connection doesn't move — only that Actor's binding route and
  bound-session current location snapshot change to the target, via authority
  generation and sequence barrier. ActorId/ObjectGeneration are kept.
