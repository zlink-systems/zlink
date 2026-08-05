---
title: "Failure Handling And Failover Scope"
---

# Failure Handling And Failover Scope

[Spec table of contents](README.en.md) · [Previous: Transport Connection Liveness](29-transport-liveness.en.md) · [Next: Framework Error Model](32-framework-error-model.en.md)

> **What this chapter defines** — the scope in which the framework
> automatically continues the same work when a failure occurs during a
> connection, message operation, object creation, or relocation.


## 1. Questions This Document Answers

This document defines the scope in which the framework automatically
continues the same work when a failure occurs during a connection, message
operation, object creation, or relocation. There's no public API letting
the application choose a separate `FailoverPolicy`. The framework applies
this document's fixed rules based on the operation kind and when the
failure occurred.

The framework doesn't judge an operation's execution as confirmed merely
because it selected a processing target. Before transport or the target
queue accepts the operation, it can confirm the operation hasn't run, so it
can select a different allowed target. Once acceptance status is unclear or
already accepted, the same operation isn't automatically submitted to a
different target, to prevent duplicate execution.

In this document, failover means switching a failed processing target to a
different target and continuing application work. It's distinguished from
reconnect — re-establishing a broken physical connection to the same
logical peer — `Relocate` — moving stateful workload before planned
maintenance — and the next application call re-querying current state.

## 2. Common Judgment Criteria

The framework judges whether automatic re-selection or re-execution is
possible in the following order.

The node currently processing an Actor or Spot is called the
[owner](01-glossary.en.md#owner). An operation using an owner checks not
only the target node's physical connection but also the Location Store's
object generation and owner eligibility.

1. Checks whether the caller directly specified target identity, like a
   node RID, or whether this is an operation where the framework selects a
   Channel server.
2. Checks whether transport or the target queue accepted the operation.
3. For an object operation, checks whether the owner and generation
   confirmed in the Location Store are still valid.
4. For a stateful relocation, checks whether it's before or after the
   owner-change commit.
5. If the same operation can't continue, it ends with one terminal result.
   The application decides whether to start the next call.

| Confirmed boundary | Framework handling |
|---|---|
| The framework is selecting a target and no target has accepted the operation yet | Can select a different eligible target within the same operation's deadline. |
| The caller specified a node RID, global object ID, or Session binding | Keeps the specified logical identity. Doesn't switch to a different logical target. |
| The operation was accepted by the target queue | Doesn't re-run the same operation on a different target. |
| Whether transport accepted it can't be confirmed | Since duplication is possible, doesn't automatically resubmit to a different peer. |
| The operation reached a terminal result | Returns only whichever of reply, failure, timeout, cancellation, or shutdown was confirmed first. |

The application can start a new operation after a failure. If the new
operation requests the same change again as the earlier work, the
application protocol must prevent duplicate impact via an idempotency key
or current-state check. The framework doesn't treat an earlier operation
whose execution status is unclear as if it hadn't run.

## 3. Channel Target And Connection Failure

### 3.1 Channel Server Re-Selection

The action by which the framework picks one of the current servers for the
same ChannelName is called
[select-one](01-glossary.en.md#select-one). A RouteMesh Channel uses a
server that's in the [Ready](01-glossary.en.md#ready) state — able to
accept a new operation — with weight greater than 0, as a candidate; a
ClientServer Channel uses a `Ready` server as a candidate.

If the first target's non-blocking submit isn't accepted due to
insufficient capacity, a different eligible server can be selected until
the transport queue accepts the operation. Once accepted, the same
operation isn't re-run on a different server even if there's no reply or
the connection drops.

Node direct doesn't use this re-selection rule since the caller specifies
the node RID. If the specified node doesn't exist or the connection isn't
ready, it ends with `NotFound` or `Unavailable`. The detailed selection and
completion result are defined by
[Interaction Model §3](03-interaction-model.en.md#3-node-direct-and-channel-select-one) and
[Framework API §13](06-framework-api.en.md#13-error-kinds).

### 3.2 Connection Isolation And Reconnect

The framework reflects orderly close and transport errors immediately, and
switches a non-responding half-open connection to `not-ready` within the
liveness deadline. One peer's failure doesn't stop processing on a
different ready peer and local owner, or turn the whole host `Error`.

The framework re-establishes a connection to the same logical peer using
the current configuration or discovery descriptor. It redoes the service
handshake and identity verification at this point. A previous connection
ID, reply route, Session binding, and ready state aren't reused. If
whether transport accepted an operation before the connection loss is
unknown, that operation isn't submitted to a different peer. The detailed
timing and state transition are defined by
[Transport Liveness §6](29-transport-liveness.en.md#6-connection-loss-and-reconnect).

## 4. Object Routing And Creation Recovery

### 4.1 Logical ID Messaging And ObjectGeneration

Where `ObjectGeneration` is used and where it isn't is set by
[Spot/Actor Routing §2.5](18-object-routing.en.md#25-where-objectgeneration-is-used-and-where-its-not).
The per-operation application table and the result when an owner has
disappeared are there.

This distinction makes one difference for failure handling.
**A regular Actor/Spot message only uses the global logical ID as target.**
Since it targets the current Ready object of the logical ID, not a
generation, an object re-created under the same ID and an object that lost
its owner end with different results. `ObjectGeneration` is **excluded from
a regular message's target-match condition.** The former is handled by
the new incarnation; the latter is `Unavailable`. The sections below only
cover the latter.

### 4.2 An Existing Actor And Spot

Actor and Spot messages use the current `Ready` owner confirmed in the
Location Store. Once the cache expires or the owner lease becomes invalid,
the next new operation re-queries the current owner. The failed operation
itself isn't automatically submitted to the new owner.

Right after the owner changes via a planned relocation, the previous owner
can deliver a message it already received to the committed target. This
action is called [Message Follow](01-glossary.en.md#message-follow). The
default for
[MessageFollowDuration](01-glossary.en.md#message-follow-duration), which
sets how long this delivery path is kept, is 30 seconds; `0` means it's
unused. Message Follow isn't failover, since it only follows an
already-committed move path — it doesn't select a new owner after an owner
process failure. The detailed route and cache rules are defined by
[Spot/Actor Routing](18-object-routing.en.md).

If the current `Ready` Actor's or Spot's owner process terminates, the
framework doesn't automatically restore the same object on a different
node. It doesn't arbitrarily change the owner recorded in the Location
Store or create a new incarnation of the same global ID. This rule applies
identically to Instance Spot. Being of the Instance Spot kind alone
doesn't release authority after owner lease expiry, or convert the next
message into cold activation.

### 4.3 Actor And Spot Creation

If creation requests compete while no object exists, only the target that
first secures the Location Store's `Creating` record runs the factory. If
the process terminates during creation, the next framework operation
re-checks the creation record for the same object ID and generation. It
can continue the same creation or cancel exactly that record, and the
factory can be called again with the same input.

This behavior is recovery before creation is exposed as `Ready` — not
failover recovering an owner failure for an already-running object.
Creation competition and result are defined by
[Spot And Actor Membership §2](15-spot-actor.en.md#2-the-process-that-confirms-only-one-object-is-created).

### 4.4 Distinguishing Instance Spot Cold Activation From Owner Failure

The process of creating and preparing an Instance Spot when the first
message arrives is called
[cold activation](01-glossary.en.md#cold-activation). Since the framework
stores the first message and creation record, if the process terminates
while `Creating` or while restoring the first message, it can continue or
cancel creation under the same generation. It doesn't process new messages
before restoring the first message to the head of the queue.

An Instance Spot doesn't call a separate create API — it's created from
the `Missing` state by the first message. This trait only decides
**when** the object is created — it doesn't add a failover policy that
automatically restores the object on a different node after a `Ready`
owner failure. The following table distinguishes what the framework does
based on current authority when a message arrives.

| Current state | Handling of a new message with Instance intent |
|---|---|
| `Missing`, with no authority record | Selects one eligible node and starts cold activation of a new `ObjectGeneration`. |
| `Creating`, or `Ready` with the first message not yet restored | Uses the stored creation record and first message to continue or cancel creation of the same `ObjectGeneration`. Doesn't create a new incarnation. |
| `Ready` with a valid owner lease | Sends the message to the current owner. Doesn't start cold activation. |
| The `Ready` owner process terminated, or the owner lease is invalid | Doesn't automatically release the authority record or create a new incarnation on a different node. The operation ends with `Unavailable`. |
| The application's explicit `Close` finished, including authority release | A subsequent lookup returns `Missing`. The next Instance-intent message can start cold activation of a new `ObjectGeneration`. |
| A planned `Relocate` is in progress or finished | Moves the same object and `ObjectGeneration` to the target per the relocation contract. Not treated as cold activation or crash failover. |

So the behavior "once the process terminates and the lease expires, the
next message reactivates the Instance Spot on a different node" isn't part
of the current contract. Providing such behavior would require defining a
separate failover contract: under what condition to release a failed
owner's authority, how to recover stored state and accepted operations,
and what fence blocks the previous owner.

The first-creation recovery information is only used for an Instance
Spot's first creation. It doesn't apply to Actor, User Spot, an
already-`Ready` Instance Spot, or host relocation. The storage and resume
order are defined by
[Location Runtime §6.1](21-location-runtime.en.md#61-first-creating-an-instance-spot-on-the-node-that-received-the-message).

## 5. Host Relocation Failure

`Relocate` is a planned action where the running source and the selected
target hand off state and not-yet-executed work. It isn't an operation
that finds an owner to substitute for a failed host. The current version
only supports a graceful handoff that runs until the source runtime, the
selected target runtime, the Location Store, and the Relocation Store
finish the operation.

| Failure timing | Framework handling |
|---|---|
| Before the owner-change commit | Discards the target instance and temporary queue, keeps source owner/membership and queue. Doesn't automatically select a different target. |
| No Store change result received | Doesn't guess success or failure — re-reads the same authority record to confirm the actual owner. |
| After the owner-change commit, same target process running | Doesn't roll back to the source. Can retry the lifecycle callback or dispatch switchover on the same target within the deadline. |
| Target process terminated after the owner-change commit | The Location Store keeps the target owner, but the object becomes `Unavailable`. A different runtime doesn't take over the relocation. |
| Source or target process terminated during the operation | Doesn't select a different target, resume relocation after a process restart, or roll back to the source. |

Keeping the source before commit isn't failover — it's canceling an
operation that hasn't changed owner yet. Continuing on the same target
after commit also isn't a new target selection. Object failover after
process termination isn't part of the current contract. The detailed
stages and result are defined by
[Host Relocate And Shutdown §1.1](28-graceful-drain-handoff.en.md#11-failure-handling-scope) and
[Spot And Actor Membership §7](15-spot-actor.en.md#7-failure-handling-scope).

## 6. Session And Binding

If an Actor moves via a planned relocation, the Session's physical STREAM
connection is kept. The target runtime sends the session owner a location
update message to change that Actor's binding route and current `ActorRef`
location snapshot. This update only applies to a relocation where
[ObjectGeneration](01-glossary.en.md#objectgeneration) — identifying the
same Actor incarnation — is kept, and the application doesn't rebind to
learn about the relocation.

If an Actor is removed, or a new incarnation of the same ActorId is
created after an owner failure, the previous binding stays terminated. A
regular Actor direct message can be sent to the current ActorId, but since
Session relay needs the current binding token, the application must bind a
new `ActorRef`. A late-arriving relay/unbind/disconnect from a previous
Session isn't applied to the new binding.

If the session owner process terminates, the framework doesn't transfer
the physical connection, Session identity, and binding to a different
process. A client reconnect creates a new Session, and the application
must redo authentication and bind on the new Session. The previous
connection's reply and binding update aren't applied to the new Session.
The detailed termination boundary is defined by
[Session-Actor Dispatch §6](20-session-actor-dispatch.en.md#6-failure-handling).

## 7. Store Failure

If the framework doesn't receive a Location Store change result, it
doesn't guess success or failure. It re-reads the record with the same key
and the `StoreVersion` first used, confirms whether the change applied,
and only retries the same Store operation if needed. This confirmation is
a procedure to avoid ever creating two owners — it isn't failover
selecting a different target.

During `StoreFailureGrace`, the framework keeps the last fully-read
descriptor list and continues liveness checking of existing connections.
It doesn't create a new outbound connection. Grace doesn't extend the
owner lease or relocation deadline. Once owner eligibility ends, new
message/timer processing and state changes stop. Once the Store recovers,
owner and the whole descriptor are re-confirmed, and only needed
connection changes are applied.

The re-confirmation and payload order for a Store request are defined by
[Location Runtime §8](21-location-runtime.en.md#8-when-a-store-response-isnt-received).

## 8. The Application's Retry Decision

The framework returns a failed operation's `ErrorKind` but doesn't provide
whether to retry. This is because, on a timeout or connection loss,
whether the remote handler ran may be unknown. The application decides
whether to start a new operation after preventing duplicate impact via
operation idempotency, a business-level idempotency key, result lookup, or
state comparison.

The framework re-selecting a target before acceptance, or re-confirming a
Store result, is internal processing of the same operation. It's
distinguished from a new operation the application starts after receiving
a failure result. The detailed error and completion conditions are defined
by [Framework Error Model](32-framework-error-model.en.md).

## 9. Implementation And Contract-Test Verification Requirements

- Channel select-one only selects a different eligible server until the
  target accepts the operation.
- Node direct, Actor/Spot direct, and Session binding operations don't
  switch the specified logical identity to a different target.
- If transport acceptance is unclear, or the operation was already
  accepted, it isn't automatically resubmitted to a different peer.
- One peer's liveness failure doesn't turn a different ready peer or the
  host state `Error`.
- A reconnect redoes handshake and identity verification, and doesn't
  reuse a previous connection's reply route, Session binding, or ready
  state.
- Creation recovery only continues the same object ID and generation, and
  only one target runs the factory.
- An Actor/Spot direct message targets the logical ID's current Ready
  object, and an ObjectGeneration mismatch alone doesn't reject running
  the application handler.
- Destroy/Close, membership, relocation, and creation recovery verify the
  exact ObjectGeneration.
- After removing an Actor and re-creating it under the same ActorId, the
  previous Session binding isn't reused.
- An Instance Spot only starts cold activation when `Missing`. A `Ready`
  owner process termination or owner lease expiry isn't turned into
  `Missing` or recovered via cold activation.
- Instance Spot cold-activation recovery isn't used for Actor, User Spot,
  an already-`Ready` Instance Spot, or host relocation.
- A failure before the relocation commit keeps the source; a failure
  after commit doesn't roll back to the source.
- After a source or target process terminates, a different runtime
  doesn't take over the relocation or automatically select a different
  target.
- If a Store result is unclear, source admission and target dispatch
  aren't opened before re-reading authority.
- After a session owner process terminates, the Session and binding
  aren't restored on a different process.
