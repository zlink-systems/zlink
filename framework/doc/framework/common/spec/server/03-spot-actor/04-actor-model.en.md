---
title: "Actor Model"
---

# Actor Model

[Spot And Actor topic index](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 03. MeshNode](03-mesh-node.en.md) · [Next: 05. Spot And Actor Membership](05-spot-actor-membership.en.md)

> **What this chapter defines** — an Actor's identity, location, message queue,
> lifecycle, and session binding.

## 1. Actor Model Overview

Regardless of whether an Actor is in a
[Spot](../00-foundation/02-glossary.en.md#spot) — a logical instance with an address and
state — such as an
[Entry Spot](../00-foundation/02-glossary.en.md#entry-spot-user-spot-and-instance-spot) or a User
Spot, or on a remote [MeshNode](../00-foundation/02-glossary.en.md#meshnode) — a runtime node
that sends or receives messages on a connection topology in which multiple nodes
participate — application payload is submitted to the Actor's own queue.
The gate that runs a handler once it's on the queue is determined by
[Actor membership](../00-foundation/02-glossary.en.md#actor-membership) — the relationship
representing which Entry Spot or User Spot the Actor currently belongs to —
and User Spot execution mode.

The related contracts are owned by the following documents.

| Related contract | Document that defines it |
|---|---|
| MeshNode route and peer admission | [MeshNode](03-mesh-node.en.md) |
| [Spot](../00-foundation/02-glossary.en.md#spot) membership transactions and relocation | [Spot And Actor Membership](05-spot-actor-membership.en.md) |
| [STREAM session](../00-foundation/02-glossary.en.md#stream-session) (a server-side execution unit kept alive from accepting one client connection until it closes) integration | [Session–Actor binding](../04-session/02-session-actor-binding.en.md) |
| Payload and metadata | [Message Model](../00-foundation/05-message-model.en.md) |
| Callback execution and completion | [Async Execution Policy](../01-execution/01-submit-and-completion.en.md) |

## 2. Actor Identity and Mutually Independent State

### 2.1 ActorId and Stable Type

An Actor is a stateful object identified by a logical `ActorId`, unique across
the whole namespace of the
[Location Store](../00-foundation/02-glossary.en.md#location-store) — the store that keeps
each Spot's current owner, ObjectGeneration, and lifecycle state where
multiple nodes can check it together.

`ActorId` is UTF-8 `1..255` bytes, compared byte-for-byte, case-sensitive. The
Framework doesn't apply Unicode normalization or case folding.

[`MeshName`](../00-foundation/02-glossary.en.md#meshname) is an attribute used when choosing
where to initially place an Actor and isn't part of Actor identity. So the same `ActorId` can't be
duplicated across different Meshes.

Actor type is a stable name, UTF-8 `1..255` bytes. This name selects the
factory when creating an Actor. A language's class name or generic type name
isn't used as Store or wire identity. Registering the same stable type twice
on the same server is a startup error.

### 2.2 ActorRef

`ActorRef` is an immutable snapshot representing an Actor's location at a
specific point in time.

| `ActorRef` field | Meaning |
|---|---|
| `ActorId` | Logical Actor identity. |
| [`ObjectGeneration`](../00-foundation/02-glossary.en.md#objectgeneration) | A non-zero unsigned 63-bit value distinguishing different logical incarnations of the same ActorId. `RecreateOnRelocation`, which re-creates the Actor object on the target during relocation, doesn't change this value, since it's the same incarnation continuing. |
| Current `MeshName` | The Mesh the current owner belongs to. |
| Current `NodeRid` | The RID of the current [owner](../00-foundation/02-glossary.en.md#owner) node. |

`ActorRef` isn't a value used as an Actor message target. If the Actor moves
or is re-created, a previous `ActorRef` can become stale.

[`ObjectGeneration`](../00-foundation/02-glossary.en.md#objectgeneration) is represented as
a decimal string in JSON. A separate `ActorRefSnapshot` public type isn't
provided.

### 2.3 Spot Membership and STREAM Binding

An Actor manages the following two states independently of each other.

| State axis | Possible states | What this state represents |
|---|---|---|
| Spot membership | [Entry Spot](../00-foundation/02-glossary.en.md#entry-spot-user-spot-and-instance-spot), user Spot, moving | Represents the Actor's logical location and Spot membership. |
| STREAM binding | unbound, bound | Represents whether it can currently push to a client session or receive session payload. |

An Actor doesn't need a bound session to exist in a user Spot. Session bind or
unbind also doesn't change the Actor's current Spot.

One Actor can bind to only one session at a time. Conversely, multiple Actors
can be bound to one session.

## 3. Actor Queue

Every Actor application payload is submitted directly to the target Actor's
application queue. The same rule applies whether the Actor is in an Entry
Spot, a user Spot, or on a remote MeshNode.

- Payload accepted by the same Actor queue is processed in order on the Actor
  turn.
- An Entry Spot Actor and a `PerActor` User Spot's Actor use a per-Actor gate.
  Different Actors can run independently.
- A `SpotWide` User Spot's member Actors use the User Spot's common execution
  gate. Only one of that User Spot's Actor/Spot handler/timer/lifecycle
  callback runs at a time, across the whole Spot.
- `Yield` can be used only while running on a `SpotWide` User Spot's common
  gate. It isn't provided for an Entry Spot Actor or a `PerActor` User Spot's
  Actor.
- Even if a `SpotWide` member Actor yields, the claim on the current Actor
  queue head is kept. Other Actors/Spot handlers/timers can use the returned
  User Spot gate, but the same Actor's next payload doesn't start until the
  current continuation ends.
- Actor send/request, [STREAM session](../00-foundation/02-glossary.en.md#stream-session)
  relay, and calls between Actors all go into the same Actor queue.
- Actor payload isn't put on the Spot application queue or converted into a
  Spot callback.

The execution rules for User Spot execution mode and `Yield` continuation are
defined by
[Async Execution Policy §1.1](../01-execution/01-submit-and-completion.en.md).
The sequence in which Entry Spot and User Spot process Actor membership itself
is defined by
[Spot And Actor Membership §3](05-spot-actor-membership.en.md#3-actor-membership-for-entry-spot-and-user-spot),
and this section's gate/`Yield` rules apply as-is regardless of membership
state.

### 3.1 Deferred Join Barrier

An Actor Join is registered via `Defer()` on a `JoinSpot(...)` or
`JoinEntrySpot(...)` call. `Defer()` is a synchronous terminal that schedules
the Join to run after the current handler ends. At the call site itself, it
doesn't look up a target or access the Store. It records an immutable Join
request on the current handler and only registers a
[Deferred Join barrier](../00-foundation/02-glossary.en.md#deferred-join-barrier) (an
inactive queue barrier), so this Actor's next message doesn't run before the
Join.

The Join call doesn't provide `Async`, `await`, `submit`, a coroutine
terminal, or `Yield`. `Defer()` itself also doesn't return the Spot gate or
Actor queue claim. The current handler keeps running, and only once it ends
normally through its last awaited continuation does the framework activate
the barrier and start the Join. If the handler ends with an exception or
cancellation, every inactive barrier that handler registered is discarded.

Registration fixes the Actor's generation, current membership, an
immutable request snapshot, an absolute deadline, and a non-zero 128-bit
operation ID.

One handler can register at most 64 Joins. One Join request's encoded size is
at most 1 MiB, and the sum of every Join request the same handler registered
is at most 8 MiB. If the request is omitted, an empty `ZLinkMessage` is fixed.
Each `Defer()` turns the request into an immutable snapshot and computes an
absolute deadline based on the monotonic clock. The default timeout is 5
seconds; an explicit value must be a finite value in `1..INT_MAX`, rounded up
to milliseconds. Exceeding the limit fails the current registration
synchronously as a startup configuration error, without leaving any partial
record.

A cross-node Join's application reply is also at most 1 MiB. The request's and
reply's size limits are independent of each other. Even when storing both for
crash recovery, they aren't merged into one 1 MiB limit.

An Actor send/request handler, and a User/Entry Spot's
packet/request/subscription/timer handler, can register a local member
Actor's Join. It's `InvalidOperation` in the
[factory](../00-foundation/02-glossary.en.md#factory) (application-provided code that
creates an Actor instance matching a registered stable type), `Configure`,
lifecycle callback, relocation adapter, a detached task, an Instance Spot
handler, and a thread the framework doesn't manage. A second `Defer()` on the same call is
`InvalidOperation`; a different pending membership transition on the same
Actor is `Unavailable`.

The time window during which the framework allows `Defer()` to be called is
called the handler registration scope. This scope is open while the handler
is running and in an awaited continuation the framework tracks. Calling it
after the scope closes is `InvalidOperation`. It's a contract violation for
the application to call `Defer()` from a detached task the handler started
without awaiting. The framework doesn't guarantee catching this misuse before
handler completion in every language.

Handler turn, inactive barrier, and scope are kept only in the current
process's memory. If the process terminates before the Join runs or the
Location Store commits, this registration and completion aren't replayed —
source authority and membership are kept unchanged.

Payload that arrives after registration but before the source is sealed is
accepted by the Actor queue behind the barrier. In cross-node relocation, this
payload also moves to the target together with the accepted journal and
not-yet-run queue work.

After the source is sealed, handling differs before and after the
owner-change commit. Payload that arrives before the commit is kept in the
relocation ingress hold. After the commit finishes, payload that reaches the
previous owner is delivered to the new owner via
[Message Follow](../00-foundation/02-glossary.en.md#message-follow).

If the same handler that registered a barrier sends a request to that Actor
and waits for its reply, the request waits behind the barrier while the
handler also can't finish — creating a circular wait. The framework rejects
this request with `InvalidOperation` before submitting it.

If Join and maintenance race, whichever control state is confirmed first is
followed. If the Join claim comes before `Relocate`, maintenance waits until
Join reaches a terminal state. If the `Relocate` seal comes first, Join fails
with `Unavailable`; if the shutdown admission seal comes first, it fails with
`ShuttingDown`.

If the Actor already belongs to the requested User Spot, or an Entry Spot
Actor calls `JoinEntrySpot` again, an `Accepted` completion runs without
actually changing location. The Location Store and membership aren't
changed, and the join/joined/leave lifecycle callbacks aren't run either.

If a request handler fails to encode the application reply, it's treated as a
handler failure and the inactive barrier is discarded. If, after encoding
finishes, the caller closed the connection or the transport couldn't accept
the reply, the already-registered Join isn't canceled.

An Actor handler owns the Actor's own mutable state. To read or change state
a Spot owns — like a room, stage, or zone — the Actor handler must submit an
explicit Spot send/request. This work runs on the target Spot turn.

An Actor handler receives the containing Spot object. In `SpotWide`, Spot
state can be used within the shared gate. In `PerActor` and Entry, the
containing Spot's mutable state isn't directly shared — the explicit Spot
send/request above is used instead.

The framework processes notification that an Actor is Ready, request
completion, relocation stage transition, and session binding progress in a
dedicated queue. Since this is separate from the queue where the Actor's
business handler runs, it must be able to keep processing even while an
application handler is waiting for an async task.

## 4. Actor Control Handled by a Spot

A Spot doesn't process Actor application payload. The only Actor-related work
processed in a [Spot turn](../00-foundation/02-glossary.en.md#spot-turn) is membership and
lifecycle control.

| Control work | What the Spot processes |
|---|---|
| Join | Determines whether to allow Actor membership and updates the membership the Spot owns. |
| Leave | Releases membership and cleans up the state the Spot owns. |
| Relocation prepare/commit/abort | Changes the state the Spot owns consistently with the move transaction. |
| Actor lifecycle notification | Runs follow-up work the Spot owns after Actor creation/termination. |

The framework puts this lifecycle work on the target Spot's dedicated queue.
Since it runs one at a time with the same Spot's other callbacks, two
callbacks never change Spot state concurrently.

Lifecycle work that changes state an Actor owns also runs one at a time on
the Actor's dedicated queue. The order for changing both Actor and Spot state
together, and the rule rejecting a stale owner's change, are defined by
[Spot And Actor Membership](05-spot-actor-membership.en.md).

When the lifecycle queue and application payload queue can both run, **the
lifecycle queue runs first.** This is to prevent running payload addressed to
that Actor before its Join finishes, or after its leave is confirmed. This
priority applies only between the two queues — it doesn't change the
acceptance order within each queue.

This priority **isn't an absolute priority.** Two different caps are involved
here, and they must not be mixed up.

| Cap | Fairness between what | Where it's defined |
|---|---|---|
| Owner occupancy cap | Between **different owners** | [Framework API](../00-foundation/06-framework-api.en.md) |
| Lifecycle continuous-run cap | Between **two lanes within the same owner** | This section |

Once the owner occupancy cap is reached, that whole owner gives up its turn
and a different ready owner runs. This alone doesn't prevent starvation
between lanes — when the turn comes back to this owner, if both lanes are
still ready, the same priority rule picks lifecycle again.

So the lifecycle lane gets a separate **continuous-run cap** and **yield
debt**.

The continuous-run cap counts **the number of turns the lifecycle lane was
picked consecutively.** It's turn count, not time, because the execution-time
cap is already handled by the owner occupancy cap, and the problem between
lanes is "how many times in a row is it picked."

1. Every time the lifecycle lane is picked, the consecutive count goes up by
   one.
2. Once the consecutive count reaches the cap, **yield debt** is marked on
   that owner, and the count resets to 0.
3. As long as the application lane is ready, an owner with yield debt **runs
   the application lane first** when it gets a turn.
4. Once one application turn runs, the debt is cleared.

The boundary conditions are as follows.

| Situation | Handling |
|---|---|
| The lifecycle lane was empty, so the application lane was picked | Resets the consecutive count to 0 |
| There's debt but the application lane isn't ready | Keeps the debt and keeps running the lifecycle lane. No application work means no starvation either |
| It yielded to another owner and came back | Keeps the debt and consecutive count as-is. Independent of the owner occupancy cap |
| The owner terminates or moves | Discards debt and consecutive count together |

**Where the debt attaches depends on execution mode**
([Spot Messaging §5.4](02-spot-messaging.en.md)).

| Mode | Where the debt attaches | What clears the debt |
|---|---|---|
| `SpotWide` User Spot, Entry Spot, Instance Spot | One shared execution gate | Any application work running on that gate |
| `PerActor` User Spot | Per gate — Actor gate, Spot lane gate, timer gate | That gate's application work. An Actor lifecycle debt is cleared only by **that Actor's** application work; a Spot lifecycle debt is cleared only by **the Spot lane's** application work |

If `PerActor` didn't scope debt per gate, one Actor's lifecycle burst would
count as resolved by a different Actor's turn, and that Actor's application
work would keep getting pushed back.

This guarantee is still **qualitative** — since the owner occupancy cap has
no fixed value or allowed range, this provision can't be used to judge "runs
within N ms." Until values are fixed, what can be verified is only
"application turns do eventually run, even while lifecycle work keeps
arriving."

> The top-level spec README cites the owner occupancy cap and the lifecycle
> continuous-run cap against this section. Once this document moves to its
> new path, its section number changes to `## 4`, so the top-level README's
> citation must also be updated as part of the move step (§5).

## 5. Actor Messaging

An Actor send/request's target is a global `ActorId`. The framework finds the
current, [Ready](../00-foundation/02-glossary.en.md#ready) incarnation and the owner route
[authority](../00-foundation/02-glossary.en.md#authority) points to, either in the positive
route cache or the [Location Store](../00-foundation/02-glossary.en.md#location-store). It
then checks owner fence and submits the message to the target queue. The
`ObjectGeneration` confirmed while resolving is information distinguishing a
route snapshot from a stale cache — it isn't a target-match condition for the
Actor handler.

A local Actor and a remote Actor use the same meaning for handler execution
and completion.

The caller doesn't specify the following values as an Actor message target.

- `MeshName`
- `ActorRef`
- Owner RID
- The current Spot's global address

### 5.1 Route Cache and Generation

- `Missing`, `Creating`, and Store failure results aren't stored in a negative
  cache.
- The positive cache holding the current Ready location is also used only
  within the current owner lease's local admission deadline and the public
  `RouteCacheMaxAge`.
- The positive cache is invalidated immediately on confirming a larger
  StoreVersion, a stale result, or a Store recovery event.
- `ObjectGeneration` isn't a target-match condition for an Actor direct
  message.
- If, after resolve, an Actor is destroyed by the same owner and re-created
  under the same `ActorId`, the current Ready Actor at the moment the target
  queue accepts it processes the message.
- If the resolved owner no longer owns that ActorId, the current operation
  ends with a stale-route error. The framework doesn't automatically resend
  the same operation after finding a new owner in the Location Store.
- Even on a request timeout or a failure where execution status is unknown,
  the framework doesn't automatically resend.
- Actor direct messaging doesn't create or change a session binding.

### 5.2 Handler Selection

The framework selects a handler by Actor type, message kind, and packet name.
Registering the same key twice in the same Actor handler namespace is a
startup error.

Handler type and signature are defined by each language's public interface
document. The handler instance and scoped dependency are owned by that
Actor's activation, not the hosting Spot. Different Actors don't share the
same handler instance — it's re-created in the target Actor activation after
relocation and cross-node Join. The detailed lifetime contract follows
[Framework API's Handler Lifetime](../00-foundation/06-framework-api.en.md#11-handler-execution-object-and-dependency-lifetime).

Actor and Actor Context have a composition relationship. Before calling the
factory, the framework builds a Context holding `ActorId`,
`ObjectGeneration`, current `MeshName`, a nullable current `SpotId`, and
bound-session capability. The factory receives only this Context — it doesn't
take the ID as a separate argument. The returned Actor must expose the
received Context as-is via a read-only `Context` member, and `Configure()`
doesn't take a Context argument. Returning a different Context means the
staging Actor isn't exposed as Ready.

A same-node Join keeps the Actor instance and Context, only changing `SpotId`
in the membership commit. A cross-node Join keeps the Actor ID and
ObjectGeneration but passes a new Context, bound to the target owner and
membership, to the target factory. After commit, the source Context's
identity can still be read through the source leave callback, but a new
send/request/session mutation/Join ends with `Unavailable` and isn't
automatically forwarded to the current target.

## 6. Actor Lifecycle

### 6.1 Registering Factory and Relocation Policy

The Object Server registers the following values together.

- Actor [stable type](../00-foundation/02-glossary.en.md#stable-type)
- [Factory](../00-foundation/02-glossary.en.md#factory)
- The relocation policy chosen in the factory configure callback

An overload that omits relocation policy, or a compatibility default, isn't
provided.

If `PreserveStateWith` is chosen, an `ActorRelocationAdapter` matching that
Actor type must be provided in the same registration. The adapter stores and
restores Actor state as a byte sequence only the application interprets. The
Framework doesn't interpret this byte sequence's content and doesn't manage a
separate state contract ID either.

`PreserveStateWith` captures application state at the moment the source
handler ended normally, and restores it into the target Actor.
`RecreateOnRelocation` re-creates the Actor object on the target but doesn't
restore application state. Instead, framework-owned not-yet-executed queue
and timer information are kept after the move. Neither policy changes
`ObjectGeneration`, since it's the same logical Actor moving. On a cross-node
move where the owner changes, only
[`AuthorityOwnerGeneration`](../00-foundation/02-glossary.en.md#authorityownergeneration) —
a provider-issued value marking the order in which the authority owner
changed for the same object incarnation — increases.

If the moving Actor is bound to a Session, once the target restores the Actor
and finishes owner/membership commit, queue merge, switching to the regular
route, and lifecycle callbacks, the target runtime sends command 44
`sessionRelocationRoute` commit one-way to the Session owner to update the
binding route to the target owner. The full sequence of this protocol, and
its sequence diagram, are owned by
[Spot And Actor Membership §4.2](05-spot-actor-membership.en.md#42-the-order-for-joining-an-actor-to-a-spot-on-a-different-node),
and the seal/held-message/route-switch responsibilities the Session owner
carries are owned by
[Session–Actor binding §8](../04-session/02-session-actor-binding.en.md#8-the-sessions-responsibility-during-actor-relocation).

The following .NET excerpt is an example to help understand the common rule
of registering factory and relocation policy together. It doesn't require the
same signature in other languages; the .NET contract itself is defined by the
[.NET Actor interface](../languages/dotnet/interfaces/06-actors.en.md).

```csharp
public interface IZLinkActorRelocationAdapter<TActor>
    where TActor : class, IZLinkActor
{
    ValueTask<byte[]> CaptureAsync(
        TActor actor,
        CancellationToken cancellationToken);
    ValueTask RestoreAsync(
        TActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken);
}

public interface IZLinkActorFactoryBuilder<TActor>
    where TActor : class, IZLinkActor
{
    IZLinkActorFactoryBuilder<TActor> DisableRelocation();
    IZLinkActorFactoryBuilder<TActor> RecreateOnRelocation();
    IZLinkActorFactoryBuilder<TActor> PreserveStateWith<TAdapter>()
        where TAdapter : class, IZLinkActorRelocationAdapter<TActor>;
}
```

### 6.2 Create and GetOrCreate Input

The Actor manager's `Create` and `GetOrCreate` are fluent calls that can be
submitted only once. Both calls take the following required values.

- `ActorId`
- Stable Actor type

The following values are optional.

- `InMesh`
- Encoded creation request
- Timeout

The caller can't specify target RID, predicate, factory class, or placement
callback.

Setting the same option twice is `InvalidOperation`. Running the terminal submit
twice is `InvalidOperation`.

One end-to-end [deadline](../00-foundation/02-glossary.en.md#deadline) is fixed when
terminal submit starts. This deadline applies across resolve, reservation,
factory execution, and the whole Ready barrier.

The following .NET excerpt shows how the fluent call splits required and
optional values.

```csharp
public abstract record ZLinkActorCreateResult
{
    public sealed record Existing(ActorRef Actor)
        : ZLinkActorCreateResult;

    public sealed record Created(
        ActorRef Actor,
        ZLinkMessage? Reply)
        : ZLinkActorCreateResult;

    public sealed record Rejected(ZLinkMessage? Reply)
        : ZLinkActorCreateResult;
}

public interface IZLinkActorManager
{
    IZLinkActorCreateCall Create(string actorId, string actorType);
    IZLinkActorGetOrCreateCall GetOrCreate(string actorId, string actorType);
    ValueTask<ActorRef?> FindAsync(
        string actorId,
        CancellationToken cancellationToken = default);
    ValueTask<bool> DestroyAsync(
        ActorRef actor,
        CancellationToken cancellationToken = default);
}

public interface IZLinkActorCreateCall
{
    // Specifies the Mesh to first create the Actor on.
    // Can be omitted if there is one object-role Mesh; InvalidOperation if omitted with two or more.
    IZLinkActorCreateCall InMesh(string meshName);
    IZLinkActorCreateCall Request(ZLinkMessage request);
    IZLinkActorCreateCall Request<TRequest>(TRequest request);
    IZLinkActorCreateCall Timeout(TimeSpan timeout); // Sets the whole creation deadline.
    ValueTask<ZLinkActorCreateResult> Async(
        CancellationToken cancellationToken = default);
}

public interface IZLinkActorGetOrCreateCall
{
    IZLinkActorGetOrCreateCall InMesh(string meshName);
    IZLinkActorGetOrCreateCall Request(ZLinkMessage request);
    IZLinkActorGetOrCreateCall Request<TRequest>(TRequest request);
    IZLinkActorGetOrCreateCall Timeout(TimeSpan timeout);
    ValueTask<ZLinkActorCreateResult> Async(
        CancellationToken cancellationToken = default);
}
```

```csharp
ZLinkActorCreateResult result = await actorManager
    .Create("player-42", "player") // ActorId and stable type are required values.
    .InMesh("world")
    .Timeout(TimeSpan.FromSeconds(5))
    .Async(cancellationToken);     // Returns Created or Rejected.
```

### 6.3 Selecting Mesh and Placement Target

If `InMesh` was specified, that Mesh is used. If omitted, the following rules
decide the Mesh.

| Condition | Result |
|---|---|
| There is exactly one Mesh with Object `Client` or `Server` role. | That Mesh is auto-selected. |
| There is no candidate. | Ends with `NotConfigured`. |
| There are two or more candidates. | Ends with `InvalidOperation`. |
| The Mesh specified via `InMesh` doesn't exist. | Ends with `NotFound`. |

The framework checks target candidates in the following order.

1. Checks object role.
2. Checks whether the requested stable type is registered.
3. Checks whether active/pending capacity remains.
4. Applies node-wide placement weight to the remaining candidates.

The caller doesn't select the target node or endpoint.

### 6.4 Creation Request and Factory Execution

An encoded creation request is at most 1 MiB. The whole procedure that
confirms the authority to create only one object — the reservation, why
factory execution must be safe, the Entry Spot `OnCreateActor`'s
approve/reject/exception handling, and the authority exposed as its result —
is owned by
[Spot And Actor Membership §2](05-spot-actor-membership.en.md#2-the-process-that-confirms-only-one-object-is-created).
This section only defines the inputs the Actor manager uses to start that
procedure.

The Actor the factory builds is a staging instance not yet exposed
externally. Whether it's approved is decided by the Entry Spot's
`OnCreateActor`; a rejected Actor can't be looked up via `Find`, can't
receive messages, and doesn't consume active capacity. Rejection isn't
implemented by exposing the Actor as Ready and then destroying it.

### 6.5 The Difference Between Create and GetOrCreate

Running exclusive `Create` when a Ready Actor of the same type already exists
ends with `AlreadyExists`. If an Actor of a different type exists, it ends
with `TypeMismatch`.

`GetOrCreate` returns the current incarnation as `Existing`, with no new
reservation or callback execution, if a Ready Actor of the same type exists.
If an Actor of the same type is Creating, it re-checks authority with bounded
backoff until that state ends. Even if multiple processes concurrently call
`GetOrCreate` for the same ActorId, only the caller whose Location Store
[reservation](../00-foundation/02-glossary.en.md#reservation-id) CAS succeeds owns
the creation execution.

The meaning of each `Created`/`Rejected`/`Failed`/`Existing` state, how a
later operation joins a concurrent creation race, and how long `OperationId`
resends preserve the terminal record, are defined by
[Spot And Actor Membership §2](05-spot-actor-membership.en.md#2-the-process-that-confirms-only-one-object-is-created).

### 6.6 Find

The manager's `Find(ActorId)` returns the `ActorRef` of the current, Ready
authority. It doesn't start Actor creation and doesn't provide a separate
Actor directory either.

### 6.7 Spot Move

Moving an Actor to a user Spot through join/leave/relocation follows the fencing and
barriers of [Spot And Actor Membership](05-spot-actor-membership.en.md).

Payload accepted during a move isn't sent to the previous Spot callback.
Payload keeps its order in the Actor queue.

### 6.8 Termination and Destroy

Actor termination closes new payload admission and cleans up session binding
and location ownership. An Actor isn't automatically terminated, or
automatically made to leave its current Spot, merely because a bound
session's connection ended.

The actual state and transaction allowing lifecycle termination are defined by
[Spot And Actor Membership](05-spot-actor-membership.en.md).

Actor destroy takes an `ActorRef`. If the Actor is in a user Spot,
leave or Entry Spot join must finish first.

Destroy isn't a membership move. So `OnLeaveActor` isn't called again during
a successful destroy.

The framework proceeds with destroy in the following order.

1. Closes new payload admission.
2. Cleans up in-progress lifecycle work.
3. Removes the session binding.
4. Removes location ownership and the registry entry.

| State | Destroy result |
|---|---|
| That incarnation no longer exists. | Returns idempotent `false`. |
| A different generation of the same ActorId exists. | Ends with `InvalidOperation`. |
| The Actor is sealed for a move. | Ends with `Unavailable`. |

The framework doesn't re-find the current `ActorRef` and terminate a new
incarnation.

## 7. Session Binding

Session binding is the runtime relationship between an Actor and the current
STREAM session. The binding token distinguishes reconnection and
late-arriving work from a previous session.

An Actor handler can use the current bound session to do the following.

- Send a one-way push to the client
- Request that the session connection be closed

Payload arriving from a session toward an Actor is also submitted directly to
the Actor queue. Spot membership can be used for route and lifecycle
verification, but isn't used as a basis to send payload to a Spot callback.

Bind, rebind, disconnect, and request correlation are defined by
[Session–Actor binding](../04-session/02-session-actor-binding.en.md).

## 8. Failure and Observability

### 8.1 Failure

| Condition | Result |
|---|---|
| A logical ActorId has no Ready authority. | Ends with an Actor target error. |
| No mapping in a call addressed by ActorRef. | Ends with `Unavailable`. |
| A call addressed by ActorRef has a generation differing from the current generation. | Ends with `InvalidOperation`. |
| The Actor is sealed before commit. | Ends with `Unavailable`. |
| An operation needing a bound session has no valid binding. | Ends with `InvalidOperation`. It's an ordering issue — a binding must be made first. |

If there's no handler, decoding fails, or the application handler returns an
exception, a request returns an error via a recoverable reply route. A
one-way message records the error in the runtime observability path.

During drain, new Actor creation and membership assignment are blocked.
Already-accepted Actor turns and control transactions proceed to the
deadline.

### 8.2 Observability Information

The runtime must be able to observe each of the following separately.

- Current `MeshName` and Actor type
- Application queue and control backlog
- ObjectGeneration
- membership state
- Session-binding state
- Dispatch result

ActorId isn't used as a metric label.

## 9. Implementation and Contract-Test Verification Requirements

- Actor payload from both Entry Spot and user Spot is delivered directly to
  the Actor queue.
- Actor payload doesn't go through a Spot callback or the
  [Spot application queue](../00-foundation/02-glossary.en.md#spot-application-queue).
- A Spot's dedicated lifecycle queue holds only join/leave/relocation and
  lifecycle control — not Actor business payload.
- Inbound dispatch checks whether a current relocation temporary queue is
  registered before finding the Actor application instance. If so, it goes
  into that queue; if not, existing Actor dispatch is used.
- A message arriving during Restore isn't run from the temporary queue.
  After commit, saved work, pre-boundary relay, and remaining temporary work
  enter the real Actor queue in order, then the regular route is installed.
  Required lifecycle work finishes before dispatch opens.
- Temporary queue removal and regular-route switchover are handled
  atomically, so messages aren't duplicated or dropped.
- If Relocation Restore explicitly fails before relay-ready is accepted, the
  target temporary queue is discarded without running and the source-owned
  original is restored. Afterward, source isn't restored regardless of
  cutover-submit result.
- Even after receiving multiple Restores with the same `RelocationId`,
  target attempt, and owner generation, the temporary queue and application
  instance are created only once. A previous attempt's temporary queue isn't
  used.
- The same Actor's payload runs in Actor-queue acceptance order, regardless
  of ingress kind.
- If a `SpotWide` member Actor yields, only the User Spot gate is returned
  while the Actor queue claim is kept. During this, other Actors/Spots/timers
  proceed, but the same Actor's next job doesn't proceed.
- Even after a `Yield`, a request the same Actor sent to itself doesn't run
  ahead of the current job or re-enter inline.
- Actor Join doesn't provide `Async` or `Yield` — it's registered with
  synchronous `Defer()` inside the handler. The result is delivered via the
  Actor completion callback.
- `Defer()` registers only Join intent and an
  [inactive barrier](../00-foundation/02-glossary.en.md#deferred-join-barrier), with no
  target lookup or Store I/O, and runs the Join only once the handler ends
  normally.
- The limits are at most 64 per handler, at most 1 MiB per request, at most
  8 MiB in total request size, and a default timeout of 5 seconds.
- Same-node Join, cross-node Join, and the
  [`RecreateOnRelocation` relocation policy](../00-foundation/02-glossary.en.md#relocation-policy)
  keep the same logical incarnation's `ObjectGeneration`.
- An Actor handler doesn't directly access mutable Spot state — it uses an
  explicit Spot call.
- Session bind and Spot membership change independently and don't implicitly
  change each other.
- The same ActorId isn't duplicated across different MeshNames.
- Actor messaging takes only an ActorId and doesn't require the application
  to provide an [owner route](../00-foundation/02-glossary.en.md#owner-route) or
  generation.
- Even when the same Actor creation is requested concurrently, a target that
  didn't obtain creation authority doesn't run an additional factory — it
  waits for the same attempt's completion.
- A different operation observing Creating receives `Existing` after Ready,
  and competes for a new reservation after rejection/failure cleanup,
  without sharing an earlier application reply.
- Only a resend with the same source Node RID/lifecycle
  generation/`OperationId` reads the correlation-free semantic terminal
  envelope and re-encodes the reply with the current correlation/reply
  route.
- `Rejected` and `Failed` don't create Ready authority or active capacity —
  they return reserved capacity.
- A terminal record allows replay of the same operation for 5 minutes after
  the original deadline, and the Actor can be re-created via a new reservation
  if there's no Ready authority after the TTL.
- The resender (replayer) is the framework runtime that started the operation.
  Only framework durable lifecycle operations that carry an `OperationId`
  (Actor create/join, session bind, and so on) are resent with the same
  `OperationId`; application requests are never resent automatically, as
  [§5.1](#51-route-cache-and-generation) states. The "caller resends after the
  handover" in the Core socket contract means the application for application
  requests and the framework for durable lifecycle operations.
- The resend condition is any case in which no terminal envelope (`Created`,
  `Existing`, `Rejected`, `Failed`, and so on) was received: a missing route,
  the single timeout caused by a handover, or a lost reply. An operation whose
  terminal envelope was received is not resent.
- Each attempt uses the operation's entire remaining deadline; the deadline is
  not split across attempts. There is no attempt limit; when the whole deadline
  is exhausted the operation ends, and the cause decides the kind: exhausted
  without any attempt ever being admitted (route absent) ends with
  `Unavailable` per the [error model](../00-foundation/07-framework-error-model.en.md);
  exhausted while an admitted request's reply never arrived ends with
  `DeadlineExceeded`. The target's terminal record guarantees that execution is
  not duplicated, so the sender does not guess whether execution happened.
- Destroy accepts only the same generation and doesn't retarget to a new
  incarnation.

---

[Spot And Actor topic index](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 03. MeshNode](03-mesh-node.en.md) · [Next: 05. Spot And Actor Membership](05-spot-actor-membership.en.md)
