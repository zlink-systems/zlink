---
title: "Spot/Actor Routing"
---

# Spot/Actor Routing

[Spec table of contents](README.en.md) · [Previous: Stage Wrapper On Spot](17-stage-wrapper-on-spot.en.md) · [Next: STREAM Server Session](19-stream-session.en.md)

> **What this chapter defines** — the criterion splitting paths that query the
> Location Store for a message going to a Spot/Actor from paths that don't.


## 1. Which Message Uses Which Route

Not every path sending a message to a Spot/Actor queries the Location Store.
The framework decides the route based on how the message started, as follows.

```text
+----------------------------------------------------------------------+
| Route source by message path                                         |
|                                                                      |
| Spot / Actor direct : Ready cache -> Store on cache miss             |
| Session -> Actor    : Stored binding route                           |
| Request reply       : Preserved reply route + correlation            |
|                                                                      |
| Only direct resolution reads the Location Store.                     |
+----------------------------------------------------------------------+
```

The three paths in the diagram above work as follows.

- If the application specifies a global Spot ID or Actor ID, the framework
  finds the current owner. It only queries the Location Store when a
  recently confirmed Ready route isn't usable.
- When relaying to an Actor bound to a Session, the route stored on the
  Session owner at successful bind time is used. Actor location isn't
  re-queried per message.
- A request's reply uses the return route and correlation included in the
  request. It doesn't query the requester's Spot/Actor location to send the
  reply.

This document defines, in one place, how the three paths above obtain and
verify a route and respond to location changes. It doesn't cover target
selection for Node direct, Channel select-one, or Logical Multicast. Object
create/get-or-create, and close/destroy and membership transactions using an
exact `ActorRef`/`SpotRef`, are also defined by each lifecycle document.

## 2. How To Send To A Spot/Actor By Global ID

### 2.1 The Order For Finding The Current Owner

A Spot direct call takes a global Spot ID, and an Actor direct call takes a
global Actor ID. The source runtime converts the ID into an actual owner
route before submitting the message.

```text
+----------------------------------------------------------------------+
| Direct resolution                                                    |
|                                                                      |
| Global SpotId or ActorId                                             |
|            |                                                         |
|            v                                                         |
| [Positive Ready cache] -- miss --> [Location Store]                  |
|            | hit                         | Ready authority           |
|            +-----------------------------+                           |
|                          |                                           |
|                          v                                           |
|                 [Owner route + route fences]                         |
+----------------------------------------------------------------------+
```

The source runtime proceeds in the following order.

1. Looks for a recently confirmed Ready owner route by global Spot ID or
   Actor ID.
2. If no usable recent route exists, queries the current object state from
   the Location Store.
3. If the object is Ready, records the owner's `MeshName` and `NodeRid`,
   object generation, and owner fence in a route snapshot.
4. Submits the message via the selected owner route.
5. The target checks whether it's the current owner of the same logical ID,
   whether a current Ready object exists, and whether local admission is
   possible, then puts it on the application queue. Object generation isn't
   checked as a target-match condition for the application handler
   ([§2.5](#25-where-objectgeneration-is-used-and-where-its-not)).

The current owner, incarnation, owner generation, and lease information the
Location Store records per global object is called authority. The framework
only uses the current Ready [authority](01-glossary.en.md#authority) as the
route for an application message.

A Spot ID or Actor ID string doesn't contain an owner address. The framework
doesn't parse the ID to infer a node or convert it into a Core routing ID.
The caller also doesn't specify the following values as a message target.

- `MeshName`
- Owner `NodeRid`
- `ActorRef` or `SpotRef`
- The Actor's current Spot ID

### 2.2 The Condition For Using A Recent Ready Route

The source runtime can briefly keep a Ready owner route confirmed from the
Location Store. This is called the positive route cache. This information
isn't a separate authority replacing the Store's current authority — it's a
snapshot of a recent lookup result.

| Item to check | Contract |
|---|---|
| Information kept in the cache | The [positive route cache](01-glossary.en.md#positive-route-cache) keeps the global object ID, `ObjectGeneration`, `AuthorityOwnerGeneration`, `StoreVersion`, owner lease, node lifecycle, and owner route. |
| Usable duration | Only used until the earlier of the current owner lease's local admission deadline and `RouteCacheMaxAge`. |
| Default setting | `RouteCacheMaxAge` defaults to 15 seconds. `0` means the route cache isn't used. |
| Results not stored | `Missing`, `Creating`, and Store failure aren't cached. A previous failure alone doesn't end the next call. |
| Conditions for immediate invalidation | An entry is removed on confirming a larger `StoreVersion`, a stale-route result, a Store recovery event, owner-lease invalidation, or a **relay notification**. |
| Relay notification | When a Message Follow relay hands a message to the new owner, it notifies the original sending runtime. The notified runtime removes that entry and re-queries the owner on the next call. |
| Runtime setting change | A changed `RouteCacheMaxAge` applies starting from new cache entries. It doesn't extend an existing entry's lifetime to the new value. |

A relay notification is a framework-owned infrastructure record and doesn't
call an application handler. Losing the notification doesn't change
correctness — the same result is reached once the cache lifetime ends. The
notification is meant to shorten the window that flows through the bypass
path during the
[Message Follow duration](01-glossary.en.md#message-follow-duration).

Whether the resolved owner still owns that object, and, if a new incarnation
was created under the same ID, which side processes the message, is set by
[§2.5](#25-where-objectgeneration-is-used-and-where-its-not).

The same handler, metadata, and completion contract applies to a local owner
and a remote owner.

### 2.3 When There's No Object

A Spot direct call and Actor direct call with no Instance intent only target
an already-Ready object.

- A Missing Actor message doesn't create a new Actor.
- A Missing Spot message also doesn't create a new Spot by default.
- Only when Instance intent is specified on a Spot-specific fluent call can
  [cold activation](01-glossary.en.md#cold-activation) of a Missing Instance
  Spot start.

Since `Missing`, `Creating`, and Store failure aren't cached, the next call
re-checks the current state at that time.

### 2.4 A Message Arriving At A Previous Owner Route

Even after committing an object relocation, a message can arrive on a
previous route left in the cache. The previous owner only relays the same
operation to the current owner when a committed source→target Message Follow
route exists. During relay, it doesn't read the Location Store or run an
application handler.

The Message Follow route verifies the global object ID, `ObjectGeneration`,
source/target `AuthorityOwnerGeneration`, and owner fence. Owner generation
must increase per hop, and the chain is at most 8 hops. One route's queue
can't exceed 1,024 messages and 16 MiB, and must also respect the negotiated
message bound.

`MessageFollowDuration` defaults to 30 seconds; `0` means Message Follow
isn't used. If both `RouteCacheMaxAge` and Message Follow duration are
positive, cache max age must be at least 5 seconds shorter than Message
Follow duration. A runtime-changed Message Follow duration applies starting
from new relocations.

Relay preserves the original operation ID, `ObjectGeneration`, payload, and
reply route. If there's no Message Follow route, it expired, or a loop
occurs, it's `Unavailable`; a generation mismatch is `InvalidOperation`;
exceeding the bound is `CapacityExceeded`.

This generation check confirms that a Message Follow route relocation
installed belongs to a move of the same incarnation — it isn't a check
restricting a regular message's target
([§2.5](#25-where-objectgeneration-is-used-and-where-its-not)).

During `PerActor` User Spot relocation, `ToActor` uses the per-Actor current
owner route, not Spot authority. Even after Spot authority changes to the
target, an Actor still remaining on the source keeps the source route. Once
the Actor owner CAS succeeds, the previous owner relays to the target via
the same Actor's Message Follow route.

Work accepted before sealing the Actor queue is included in the previous
queue and accepted journal. Work arriving at the source after the seal is
held in the ingress hold. The target uses the relocation temporary queue in
the following order.

1. On receiving a Restore request, registers the temporary queue before
   creating the Actor instance.
2. Relays the source ingress hold's messages to this queue, preserving
   original operation identity and reply route.
3. Once Restore finishes, runs the owner CAS. The source keeps the ingress
   hold original until the target dispatch switchover finishes, and keeps
   relaying messages on the previous route to the target temporary queue.
4. Puts the previous queue and accepted journal into the real Actor queue
   first, then moves the temporary queue's work in behind it.
5. Removes the temporary queue registration and switches to existing Actor
   dispatch.

Work arriving at the target before the switch is held in the temporary
queue. After the switch, Message Follow and target-direct work run in the
order the existing Actor queue actually accepted them.

So even if an Actor is relocated mid-transmission, the caller doesn't need
to select a new route or rebuild the operation. Request deadline and
correlation, one-way operation identity, ActorId, and ObjectGeneration are
kept before and after the relay.

The framework doesn't automatically resubmit a failed current operation to
a new owner found in the Location Store. Only the next call re-finds the
current owner from the cache or Location Store. This rule prevents an
operation whose execution status is unknown from running duplicated across
two owners.

### 2.5 Where ObjectGeneration Is Used And Where It's Not

A regular Actor/Spot message only uses the global logical ID as target. An
Actor send/request delivers to the current Ready object `ActorId` points to;
a Spot send/request, including for an Instance Spot, delivers to the one
`SpotId` points to. `ActorRef`/`SpotRef` and the
[ObjectGeneration](01-glossary.en.md#objectgeneration) within them aren't an
application message target.

`ObjectGeneration` distinguishes whether an object was removed and
re-created under the same ID. The framework uses this value as follows.

| Operation | How `ObjectGeneration` is applied |
|---|---|
| Actor/Spot direct send/request | **Excluded** from the target-match condition. If an object under the same ID was re-created by the same owner, the current Ready object at the moment the target queue accepts it processes the message. |
| `Destroy`/`Close` and membership change | Checks whether the caller-specified incarnation matches current authority. Work on a previous incarnation doesn't change a new object's state. |
| Creation recovery | Only continues the same creation attempt and incarnation. Doesn't mix in a factory or creation result from a different generation. |
| Relocation and Message Follow | Confirms the state/queue/relay route belongs to the same relocation ([§2.4](#24-a-message-arriving-at-a-previous-owner-route)). Doesn't apply a previous generation's relocation control to a new object. |
| Session bind and relay | Bind starts with the exact `ActorRef` and issues a binding token. Since removing an Actor ends the existing binding, a new incarnation needs an explicit bind. A late relay is rejected via the terminated binding token ([§3](#3-how-to-relay-to-an-actor-bound-to-a-session)). |

The result differs based on what happened to the owner after resolve.

| What happened after resolve | Result |
|---|---|
| The object was closed/destroyed by the same owner and a new incarnation was created under the same ID | Processed by the current Ready object at the moment the target queue accepts it. Applies identically to every Spot direct message, including Actor and Instance Spot. |
| The owner process terminated, or the owner changed to a different node, so the resolved route can't be used | Ends the current operation with [`Unavailable`](32-framework-error-model.en.md). |

In both cases, the framework **doesn't automatically resend** the failed
operation to the new owner. Only when the application starts a new call is
the logical ID's current Ready owner re-confirmed. This rule prevents an
operation whose execution status is unknown from running duplicated across
two owners.

Applying this distinction lets Actor and Instance Spot use the same
messaging rule. **The logical ID sets an application message's target, and
`ObjectGeneration` only restricts control that changes a specific
incarnation's state.**

## 3. How To Relay To An Actor Bound To A Session

### 3.1 The Route Is Stored At Bind Time

A session relay doesn't resolve the Actor ID per message. It verifies the
Actor route once at bind time, stores it on the Session owner, and uses that
information for subsequent relays.

```text
+----------------------------------------------------------------------+
| Session binding route                                                |
|                                                                      |
| Bind       : ActorRef -> validate -> store route                     |
| Relay      : Session -> stored route -> Actor owner                  |
| Relocation : Target -> location update request -> Session owner     |
|                                   <- location update response       |
|                                                                      |
| No per-message Location Store lookup                                 |
+----------------------------------------------------------------------+
```

The current Actor owner delivery path a Session owner keeps for a specific
Actor binding is called the binding route.

Bind uses the location of the exact `ActorRef` the caller submitted as the
initial route. An overload where the source pre-queries the current route
from the Location Store before bind, or takes a local Actor instance,
isn't provided.

The Actor owner checks whether the following values match the current
state, registers a binding generation, and returns a terminal reply.

- `ActorId` and `ObjectGeneration`
- Target `NodeGeneration`
- `AuthorityOwnerGeneration`
- Current owner lease
- Session owner and Session lifecycle identity

Once bind succeeds, the Session owner stores the following information per
Actor in one [binding route](01-glossary.en.md#binding-route).

| Stored information | Reason it's used |
|---|---|
| `ActorId`, `ObjectGeneration` | Doesn't relay to a different Actor re-created under the same ID. |
| `MeshName`, owner `NodeRid` | Used as the route to send Actor relay and disconnect notifications. |
| `NodeGeneration`, `AuthorityOwnerGeneration`, `OwnerLeaseGeneration` | Rejects a pre-restart node and a previous owner. |
| Session owner RID/lifecycle generation, binding generation/token | Rejects a late message from a previous connection or a replaced binding. |
| Session sequence | Preserves the order of messages accepted on the same Session. |

When rebinding to a different owner or a different Actor generation, the
target Actor owner registers the new identity, then submits a tombstone to
the previous exact owner. Only after receiving the previous owner's ACK does
it return the bind terminal reply. The Session owner keeps the existing
route until the terminal reply, and atomically switches to the new route
afterward. So the Session owner doesn't keep a separate durable retry
journal after switching, and doesn't record the binding route in the
Location Store or Relocation Store. On an atomic replacement under the same
owner, the previous identity's tombstone must not remove the new identity.

### 3.2 Sending A Message Using The Stored Route

Once bind finishes, the following work uses the stored route.

- `RelayAsync(...)` from Session to Actor
- Physical disconnect and application logical-disconnect notification
- A push from Actor to the bound Session

Actor location isn't queried from the Location Store each time this work
starts. The stored route is only valid within the current owner lease's
local admission deadline. Even if the Store is temporarily unavailable, the
lease or deadline isn't extended.

If the stored route is no longer valid, the original operation is either
delivered exactly once via an active Message Follow route, or ends with
`Unavailable`. It doesn't find a new `ActorRef` from the Location Store and
automatically send the same operation to a different owner.

The Location Store and Relocation Store don't store or update the binding
route. This route is owned by the Session owner runtime. Updating the direct
route cache doesn't automatically change the Session binding route.

### 3.3 Changing The Stored Route After Actor Relocation

Even if the Actor moves to a different MeshNode, the physical STREAM
connection and Session object are kept on the Session owner process. The
socket, transport handle, and Session callback state aren't moved or
duplicated to the target Actor process.

Even during relocation, the Session owner doesn't guess a new Actor route by
querying the Location Store. Only after the target Actor of the same
`ObjectGeneration` finishes the following order is a new route delivered to
the Session owner.

1. Once the source Actor's current handler ends, blocks application dispatch
   of new Actor messages. Requests and one-way packets the Actor queue
   accepted before the seal are stored including reply route and acceptance
   order.
2. Actor messages arriving at the source after the seal are held in the
   ingress hold; once the target receives the Restore request and registers
   the temporary queue, they're relayed to that queue.
3. Commits owner and membership and finishes the lifecycle callback. For a
   Join relocation, the Join completion callback is also called at this
   stage.
4. Moves saved existing work and temporary queue work into the real Actor
   queue and switches dispatch. Afterward the target Actor starts
   processing messages.
5. The target sends `sessionActorLocationUpdateReqMsg` to the Session owner.
6. The Session owner verifies Actor generation, previous/target owner
   generation, binding generation, owner lease, and high-water.
7. The Session owner atomically changes that Actor route and the
   bound-session current Actor location snapshot, and sends
   `sessionActorLocationUpdateResMsg`. The snapshot has the same
   ActorId/ObjectGeneration and the target MeshName/NodeRid.
8. Without a response, the target resends the same request 1 second after
   the first send. Subsequent resend intervals are 1, 2, 4, 5 seconds, then
   stay at 5 seconds.

A route update is only allowed for an Actor relocation matching the
`ObjectGeneration` the binding points to. If a new incarnation is created
under the same Actor ID, the existing binding isn't switched to the new
Actor — the application must start a new bind with the new `ActorRef`.

The route, location snapshot, token, and generation of a different Actor on
the same Session not included in the relocation are kept. The physical
STREAM connection is also kept as-is. The target Actor keeps processing
messages while waiting for the location update response. A message arriving
on the previous route is delivered to the target Actor by the source
Message Follow route. The application doesn't rebind to learn about the
relocation.

On a relocation failure before commit, a Session location update isn't
sent. The source owner is confirmed in the Location Store, the target
temporary queue is discarded, and the source Actor queue and admission are
restored. The Session owner's existing route and location snapshot keep
pointing at the source. After commit, it isn't rolled back to the source
route or snapshot. Only the running current target keeps resending
`sessionActorLocationUpdateReqMsg`. Until the location update response is
received, the source Message Follow route delivers a message on the
previous route to the target. If the target process terminates, a different
runtime doesn't automatically take over the route update.

## 4. How A Request's Reply Returns

### 4.1 A Reply Doesn't Start A New Address Lookup

When the source runtime submits a request, it builds together the internal
path the reply will return on and the identifying value linking an arriving
reply to the original request.

```text
+----------------------------------------------------------------------+
| Request and reply                                                    |
|                                                                      |
| Request : Source -> Target  [reply route + correlation]              |
| Reply   : Target -> Source  [preserved reply route]                  |
|                                                                      |
| No SpotId / ActorId lookup for reply                                 |
+----------------------------------------------------------------------+
```

The target handler uses the reply capability the request carries. It
doesn't resolve the global ID of the Spot/Actor that started the request
from the cache or Location Store to send the reply.

The identifying value linking a request and terminal reply is called reply
correlation. [Reply correlation](01-glossary.en.md#reply-correlation)
decides which request to complete, and the reply route decides the path
back to the original source runtime.

Reply route and correlation aren't application metadata. Application
metadata is key-value information delivered together with business payload.
Request metadata isn't auto-copied to a reply, and a regular reply doesn't
provide a metadata setter.

### 4.2 Resuming A Request Started From A Spot

If a request started from a Spot, the source runtime preserves the
following information together with request correlation.

- The Spot execution that started the request
- The `ObjectGeneration` of the Spot that started the request

Once the reply arrives, the original request completion resumes. Even if a
new incarnation is created under the same Spot ID, a previous reply isn't
delivered to the new Spot as an application message.

Even if a Spot/Actor operation goes through Message Follow or a relocation
payload, the original reply route and correlation are preserved. Operation
ID is a value distinguishing duplicate work and doesn't substitute for the
reply route.

### 4.3 When The Reply Route Isn't Usable

The framework completes a handler/decode failure, for a request whose reply
route can be restored, with a structured error reply. Not being able to
restore the reply route doesn't mean it bypasses this by finding the
requester's Spot/Actor ID or a new owner in the Location Store. That failure
follows the drop, log, metric, and observer event contract set by the
[Interaction Model](03-interaction-model.en.md#10-handler-failure).

Even after a route error, timeout, cancellation, or a failure whose
execution status is unclear, the same request isn't automatically
resubmitted to a different owner. A request completes with exactly one
terminal result — whichever of reply, error, timeout, cancellation, or
shutdown is confirmed first.

## 5. Implementation And Contract-Test Verification Requirements

- The Spot/Actor direct starter method only takes a global ID and doesn't
  require owner RID, generation, or `ActorRef`/`SpotRef` as a message
  target.
- On a cache hit, the Location Store isn't read; the current Ready
  authority is queried after a cache miss or invalidation.
- `Missing`, `Creating`, and Store failure aren't negative-cached.
- The positive cache doesn't exceed the owner admission deadline and
  `RouteCacheMaxAge`, and is immediately removed on a higher `StoreVersion`,
  stale result, Store recovery, or lease invalidation.
- Target admission verifies the resolved exact object/owner generation and
  lease fence, and doesn't retarget to a new incarnation.
- A Message Follow relay only uses a committed route, doesn't read the
  Store, and preserves operation ID, generation, payload, and reply route.
- In `PerActor` User Spot relocation, `ToSpot` uses Spot authority and
  `ToActor` uses the per-Actor current owner. The Spot's and Actor's
  relocation temporary queues are registered independently and switched to
  existing dispatch atomically.
- A failed operation isn't automatically resubmitted to a fresh owner —
  only the next call re-resolves current authority.
- Bind uses the caller's exact `ActorRef` location as the initial route,
  and only a verified route is stored in the Session owner binding.
- Session relay, disconnect, and Actor push use the stored binding route
  without querying the Location Store per message.
- Actor relocation only changes that Actor's binding route via
  `sessionActorLocationUpdateReqMsg` and `sessionActorLocationUpdateResMsg`
  for the same `ObjectGeneration`, keeping the route and physical STREAM
  connection of a different Actor not included in the relocation.
- The target Actor processes messages even without a location update
  response, and the source Message Follow route delivers a message
  arriving on the previous route until resends finish.
- A reply uses the request's reply route and correlation, and doesn't
  query the requester's logical ID from the Location Store.
- Application metadata doesn't substitute for owner route or reply route,
  and request metadata isn't auto-copied to a reply.
