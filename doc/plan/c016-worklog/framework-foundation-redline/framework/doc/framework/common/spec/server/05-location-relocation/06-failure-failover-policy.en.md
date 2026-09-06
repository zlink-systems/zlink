---
title: "Failure Handling and Failover Scope"
---

# Failure Handling and Failover Scope

[Location·Relocation Topic Table of Contents](README.en.md) · [Spec Table of Contents](../README.en.md) · [Previous: 05. Complete Host Relocation Flow](05-host-relocation-flow.en.md)

> **What this document defines** — the extent to which the Framework
> automatically continues the same work when a failure occurs during a
> connection, message operation, object creation, or relocation.

## 1. Questions This Document Answers

This document defines the extent to which the Framework automatically continues the same work
when a failure occurs during a connection, message operation, object creation, or relocation.
There's no public API letting the application choose a separate `FailoverPolicy`. The Framework
applies this document's fixed rules based on the operation kind and when the failure occurred.

The Framework doesn't consider an operation's execution confirmed merely because it selected a
processing target. Before transport or the target queue accepts the operation, it can confirm
the operation hasn't run, so it can select a different allowed target. Once acceptance is unclear
or has already occurred, the same operation isn't automatically submitted to a different
target, to prevent duplicate execution.

In this document, failover means switching from a failed processing target to a different target and
continuing application work. It's distinguished from reconnect — re-establishing a broken
physical connection to the same logical peer — `Relocate` — moving stateful workload before
planned maintenance — and the next application call re-querying current state.

## 2. Common Judgment Criteria

The Framework judges whether automatic re-selection or re-execution is possible in the following
order.

An Actor or [Spot](../00-foundation/02-glossary.en.md#spot) is a logical target that stays reachable
at the same ID even when the node actually executing it changes. The node currently processing an
Actor or Spot is called the [owner](../00-foundation/02-glossary.en.md#owner). An operation using
an owner checks not only the target node's physical connection but also the object generation and
owner eligibility recorded in the [Location Store](../00-foundation/02-glossary.en.md#location-store)
— the storage that lets multiple nodes check each object's current owner and generation together.

1. Checks whether the caller directly specified target identity, like a node RID, or whether this
   is an operation where the Framework selects a Channel server.
2. Checks whether transport or the target queue accepted the operation.
3. For an object operation, checks whether the owner and generation confirmed in the Location
   Store are still valid.
4. For a stateful relocation, checks whether the failure occurs before relay-ready reply becomes
   accepted,
   after that boundary but before the owner-change commit, or after commit. This accepted
   boundary's definition is owned by
   [Complete Actor and Spot Relocation Flow "4.4"](04-relocation-flow.en.md#44-ordered-relay-and-one-way-cutover).
5. If the same operation can't continue, it ends with one terminal result. The application
   decides whether to start the next call.

| Confirmed boundary | Framework handling |
|---|---|
| The Framework is selecting a target and no target has accepted the operation yet | Can select a different eligible target within the same operation's deadline. |
| The caller specified a node RID, global object ID, or Session binding | Keeps the specified logical identity. Doesn't switch to a different logical target. |
| The operation was accepted by the target queue | Doesn't re-run the same operation on a different target. |
| Whether transport accepted the operation can't be confirmed | Since duplication is possible, doesn't automatically resubmit to a different peer. |
| The operation reached a terminal result | Returns only whichever of reply, failure, timeout, cancellation, or shutdown was confirmed first. |

The application can start a new operation after a failure. If the new operation requests the same
change as the earlier work, the application protocol must prevent duplicate impact via an
idempotency key or current-state check. The Framework doesn't treat an earlier operation whose
execution status is unclear as if it hadn't run.

## 3. Channel Target and Connection Failure

### 3.1 Channel Server Re-Selection

The action by which the Framework picks one of the current servers for the same ChannelName is
called [select-one](../00-foundation/02-glossary.en.md#select-one). A Channel in a
[RouteMesh](../00-foundation/02-glossary.en.md#routemesh) — the scope in which multiple MeshNodes
participate in exchanging node and Channel messages — uses a server that's in
the [Ready](../00-foundation/02-glossary.en.md#ready) state — able to accept a new operation — with weight
greater than 0, as a candidate; a ClientServer Channel uses a `Ready` server as a candidate.

If the first target's non-blocking submit isn't accepted due to insufficient capacity, a
different eligible server can be selected until the transport queue accepts the operation. Once
accepted, the same operation isn't re-run on a different server even if there's no reply or the
connection drops.

[Node direct](../00-foundation/02-glossary.en.md#node-direct) — where the caller directly
specifies both MeshName and target RID to message one node — doesn't use this re-selection rule
since the caller specifies the node RID. If the
specified node doesn't exist or the connection isn't ready, it ends with `NotFound` or
`Unavailable`. The detailed selection and completion result are defined by
[Interaction Model §3](../00-foundation/04-interaction-model.en.md#3-node-direct-and-channel-select-one) and
[Framework API §13](../00-foundation/06-framework-api.en.md#19-error-kinds).

### 3.2 Connection Isolation and Reconnect

The Framework reflects orderly close and transport errors immediately, and switches a
non-responding half-open connection to `not-ready` within the liveness deadline. One peer's
failure doesn't stop processing by another ready peer or local owner, or turn the whole host
`Error`.

The Framework re-establishes a connection to the same logical peer using the current
configuration or discovery descriptor. It redoes the service handshake and identity verification
at this point. A previous connection ID, reply route, Session binding, and ready state aren't
reused. If it is unknown whether transport accepted an operation before the connection loss, that
operation isn't submitted to a different peer. The detailed timing and state transition are
defined by
[Transport Liveness §6](../02-channel-transport/05-transport-liveness.en.md).

## 4. Object Routing and Creation Recovery

### 4.1 Logical ID Messaging and ObjectGeneration

A number distinguishing different logical incarnations of the same ActorId or a Spot's global ID
is called [`ObjectGeneration`](../00-foundation/02-glossary.en.md#objectgeneration). Where it is
and isn't used is defined by
[Spot/Actor Routing §2.5](../03-spot-actor/08-routing.en.md#26-where-objectgeneration-is-used-and-where-its-not).
The table showing its application to each operation and the result when an owner has disappeared are there.

This distinction has one consequence for failure handling. **A regular Actor/Spot message only
uses the global logical ID as its target.** Since it targets the current Ready object of the logical
ID, not a generation, an object re-created under the same ID and one that lost its owner produce
different results. `ObjectGeneration` is **excluded from a regular message's
target-match condition.** The former is handled by the new incarnation; the latter is
`Unavailable`. The sections below only cover the latter.

### 4.2 An Existing Actor and Spot

Actor and Spot messages use the current `Ready` owner confirmed in the Location Store. Once the
cache expires or the owner lease becomes invalid, the next new operation re-queries the current
owner. The failed operation itself isn't automatically submitted to the new owner.

Right after the owner changes via a planned relocation, the previous owner can deliver a message
it already received to the committed target. This action is called
[Message Follow](../00-foundation/02-glossary.en.md#message-follow). The default for
[MessageFollowDuration](../00-foundation/02-glossary.en.md#message-follow-duration), which sets how long this
delivery path is kept, is 30 seconds; `0` means it's unused. Message Follow isn't failover, since
it only follows an already-committed move path — it doesn't select a new owner after an owner
process failure. The detailed route and cache rules are defined by
[Spot/Actor Routing](../03-spot-actor/08-routing.en.md).

If the owner process of the current `Ready` Actor or Spot terminates, the Framework doesn't
automatically restore the same object on a different node. It doesn't arbitrarily change the
owner recorded in the Location Store or create a new incarnation of the same global ID. This rule
applies equally to Instance Spots. After the owner lease expires, merely being an Instance Spot
doesn't release the [authority](../00-foundation/02-glossary.en.md#authority) — the reference
information that determines which node an Actor or Spot is on and which node is currently the
owner — or convert the next message into cold activation.

### 4.3 Actor and Spot Creation

If creation requests compete while no object exists, only the target that first secures the
Location Store's `Creating` record runs the factory. If the process terminates during creation,
the next Framework operation re-checks the creation record for the same object ID and generation.
It can continue the same creation or cancel exactly that record, and the factory can be called
again with the same input.

This is creation recovery before the object is exposed as `Ready` — not failover recovering an
owner failure for an already-running object. Creation competition and result are defined by
[Spot and Actor Membership §2](../03-spot-actor/05-spot-actor-membership.en.md#2-the-process-that-confirms-only-one-object-is-created).

### 4.4 Distinguishing Instance Spot Cold Activation from Owner Failure

The process of creating and preparing an Instance Spot when the first message arrives is called
[cold activation](../00-foundation/02-glossary.en.md#cold-activation). Since the Framework stores the first
message and creation record, if the process terminates while `Creating` or while restoring the
first message, it can continue or cancel creation under the same generation. It doesn't process
new messages before restoring the first message to the head of the queue.

An Instance Spot is created from the `Missing` state by the first message, without a separate
create API call. This trait only decides **when** the object is created — it doesn't add a
failover policy that automatically restores the object on a different node after a `Ready` owner
failure. The caller's explicit choice to allow a new Instance Spot to be created when the target
Spot doesn't exist is called
[Instance intent](../00-foundation/02-glossary.en.md#instance-intent). The following table
distinguishes what the Framework does based on current authority when a message arrives.

| Current state | Handling of a new message with Instance intent |
|---|---|
| `Missing`, with no authority record | Selects one eligible node and starts cold activation of a new `ObjectGeneration`. |
| `Creating`, or `Ready` with the first message not yet restored | Uses the stored creation record and first message to continue or cancel creation of the same `ObjectGeneration`. Doesn't create a new incarnation. |
| `Ready` with a valid owner lease | Sends the message to the current owner. Doesn't start cold activation. |
| The `Ready` owner process terminated, or the owner lease is invalid | Doesn't automatically release the authority record or create a new incarnation on a different node. The operation ends with `Unavailable`. |
| The application's explicit `Close` finished, including authority release | A subsequent lookup returns `Missing`. The next Instance-intent message can start cold activation of a new `ObjectGeneration`. |
| A planned `Relocate` is in progress or finished | Moves the same object and `ObjectGeneration` to the target per the relocation contract. Not treated as cold activation or crash failover. |

So the behavior "once the process terminates and the lease expires, the next message reactivates
the Instance Spot on a different node" isn't part of the current contract. Providing such behavior
would require defining a separate failover contract: under what conditions to release a failed
owner's authority, how to recover stored state and accepted operations, and what fence blocks the
previous owner.

The first-creation recovery information is only used for an Instance Spot's first creation. It
doesn't apply to Actor, User Spot, an already-`Ready` Instance Spot, or host relocation. The
storage and resume order are defined in the first-message storage-and-resume section of
[Location Runtime](01-location-runtime.en.md).

## 5. Host Relocation Failure

`Relocate` is a planned action in which the running source and the selected target hand off state
and not-yet-executed work. It isn't an operation that finds an owner to substitute for a failed
host. The current version only supports a graceful handoff in which the source runtime, the
selected target runtime, the Location Store, and the Relocation Store remain running until the
operation finishes.

| Failure timing | Framework handling |
|---|---|
| Explicit failure before relay-ready reply becomes accepted | Discards the target instance and temporary queue, keeps source owner/membership and queue. Doesn't automatically select a different target. |
| After relay-ready reply becomes accepted but before the owner-change commit | Doesn't restore source regardless of cutover-submit result. Target continues the owner change through cutover receipt or the 1,000ms fallback. |
| No Store change result received | Doesn't guess success or failure — re-reads the same authority record to confirm the actual owner. |
| After the owner-change commit, same target process running | Doesn't roll back to the source. Can retry the lifecycle callback or dispatch switchover on the same target within the deadline. |
| Target process terminated after the owner-change commit | The Location Store keeps the target owner, but the object becomes `Unavailable`. A different runtime doesn't take over the relocation. |
| Source or target process terminated during the operation | Doesn't select a different target, resume relocation after a process restart, or roll back to the source. |

Keeping the source before relay-ready reply becomes accepted isn't failover — it's canceling an
operation before its irreversible boundary. After that boundary, the source isn't restored even before owner
commit. Continuing on the same target after commit also isn't a new target selection. Object
failover after process termination isn't part of the current contract. The detailed stages and
result are defined by
[Complete Host Relocation Flow §1.1](05-host-relocation-flow.en.md#11-failure-handling-scope) and
[Spot and Actor Membership §7](../03-spot-actor/05-spot-actor-membership.en.md#8-failure-handling-scope).

## 6. Session and Binding

If an Actor moves via a planned relocation, the Session's physical STREAM connection is kept. The
target runtime sends the Session owner a location update message to change that Actor's binding
route and current `ActorRef` location snapshot. This update only applies to a relocation where the same ObjectGeneration is kept, and the
application doesn't rebind to learn about the relocation.

If an Actor is removed, or a new incarnation of the same ActorId is created after an owner
failure, the previous binding stays terminated. A regular Actor direct message can be sent to the
current ActorId, but since Session relay needs the current binding token, the application must
bind a new `ActorRef`. A late-arriving relay/unbind/disconnect from a previous Session isn't
applied to the new binding.

If the Session owner process terminates, the Framework doesn't transfer the physical connection,
Session identity, and binding to a different process. A client reconnect creates a new Session,
and the application must authenticate and bind again in the new Session. The previous
connection's reply and binding update aren't applied to the new Session. The detailed termination
boundary is defined by
[Session and Actor Binding "9. Distinguishing Reconnection from Relocation"](../04-session/02-session-actor-binding.en.md#9-distinguishing-reconnection-from-relocation).

## 7. Store Failure

If the Framework doesn't receive a Location Store change result, it doesn't guess success or
failure. It re-reads the record with the same key and the `StoreVersion` first used, confirms
whether the change applied, and only retries the same Store operation if needed. This
confirmation is a procedure to avoid ever creating two owners at once — it isn't failover
selecting a different target.

During `StoreFailureGrace`, the Framework keeps the last fully-read descriptor list and the
connection intents for its targets, and continues liveness checks for existing connections. It
doesn't connect to a new target outside that list (owned by
[Location Runtime §10](01-location-runtime.en.md)). Grace
doesn't extend the owner lease or relocation deadline. Once owner eligibility ends, new
message/timer processing and state changes stop. Once the Store recovers, the owner and the full
descriptor list are re-confirmed, and only the necessary connection changes are applied.

The re-confirmation and payload order for a Store request are defined in the Store-response-loss
section of [Location Runtime](01-location-runtime.en.md).

## 8. The Application's Retry Decision

The Framework returns a failed operation's `ErrorKind` but doesn't indicate whether to retry. This
is because, on a timeout or connection loss, whether the remote handler ran may be unknown. The
application decides whether to start a new operation after preventing duplicate effects through
operation idempotency, a business-level idempotency key, result lookup, or state comparison.

Re-selecting a target before acceptance or re-confirming a Store result is internal processing
within the same operation. It's distinguished from a new operation the
application starts after receiving a failure result. The detailed error and completion conditions
are defined by [Framework Error Model](../00-foundation/07-framework-error-model.en.md).

## 9. Related Internal Structure

**This document is authoritative for public failure behavior.** The following documents describe
the corresponding implementation structure and don't redefine this chapter's
error meaning or failover scope.

- [Spot/Actor Routing](../03-spot-actor/08-routing.en.md) explains how resolver result types
  preserve `Missing` and `Unavailable`.
- [09. Object Kind and Activation](../03-spot-actor/09-object-lifecycle.en.md) explains how
  resolver results become distinct activation-state-machine inputs.
- [Transport Liveness](../02-channel-transport/05-transport-liveness.en.md) separates
  availability evidence from ownership of authority release.
- [Service Wire Protocol](../02-channel-transport/06-wire-protocol.en.md#8-instance-spot-cold-activation-recovery)
  explains the durable root and scan used only for first-activation recovery on the same target.

## 10. Verification Requirements

The following is verified using only the public surface (target selection result, returned
`ErrorKind`/outcome, Location Store record lookup, Session binding state). Each item corresponds
to one test.

**Channel and connection**

Delivering a send or request to one Spot by specifying its global ID is called
[Spot direct](../00-foundation/02-glossary.en.md#spot-direct).

- Channel select-one only selects a different eligible server until the target accepts the
  operation.
- Node direct, Actor/Spot direct, and Session binding operations don't switch the specified
  logical identity to a different target.
- If transport acceptance is unclear, or the operation was already accepted, it isn't
  automatically resubmitted to a different peer.
- One peer's liveness failure doesn't put a different ready peer or the host state into `Error`.
- A reconnect redoes the handshake and identity verification, and doesn't reuse a previous
  connection's reply route, Session binding, or ready state.

**Object routing and creation recovery**

- Creation recovery only continues the same object ID and generation, and only one target runs
  the factory.
- An Actor/Spot direct message targets the logical ID's current Ready object, and an
  ObjectGeneration mismatch alone doesn't reject running the application handler.
- Destroy/Close, membership, relocation, and creation recovery check that the ObjectGeneration matches.
- After removing an Actor and re-creating it under the same ActorId, the previous Session binding
  isn't reused.
- An Instance Spot only starts cold activation when `Missing`. A `Ready` owner process termination
  or owner lease expiry isn't turned into `Missing` or recovered via cold activation.
- Even if a `Ready` authority still has an [activation recovery pointer](../00-foundation/02-glossary.en.md#activation-recovery-pointer) identifying the cold activation recovery root and replay cursor, it is used only to resume
  the incomplete first cold-activation operation on the same target node and lifecycle designated
  by the authority. It is never a basis for selecting another target after a steady `Ready` owner
  failure.
- Instance Spot cold-activation recovery isn't used for Actor, User Spot, an already-`Ready`
  Instance Spot, or host relocation.

**Host relocation and Session failure**

- Only an explicit failure before relay-ready reply becomes accepted keeps the source; a later
  failure doesn't roll back to source regardless of cutover-submit result.
- After a source or target process terminates, a different runtime doesn't take over the
  relocation or automatically select a different target.
- After a Session owner process terminates, the Session and binding aren't restored on a
  different process.

**Store failure**

- If a Store result is unclear, source admission and target dispatch aren't opened before
  re-reading authority.

---

[Location·Relocation Topic Table of Contents](README.en.md) · [Spec Table of Contents](../README.en.md) · [Previous: 05. Complete Host Relocation Flow](05-host-relocation-flow.en.md)
