---
title: "8. Object Kind And Activation"
---

# 8. Object Kind And Activation

[Internal structure table of contents](README.en.md) · [Previous: 7. Receive And Dispatch Loop](07-dispatch-loop.en.md) · [Next: 9. Session And Actor Binding](09-session-binding.en.md)

> **What this chapter answers** — how the three Spot kinds are
> distinguished, when a missing object is built, and how a message
> arriving at a stale owner is filtered out.
>
> **Contract ownership** — the Spot kinds and shutdown reasons are
> owned by [the Spot Model](../spec/11-spot-model.en.md), and where
> generation is used by
> [Spot · Actor Routing](../spec/18-object-routing.en.md). This
> chapter covers the **structure** that satisfies that contract, and
> the mismatches actually observed across the four implementations.

Covers how, in code, the three kinds of
[Spot](../spec/01-glossary.en.md#spot) — the execution unit holding
Actors and handlers — are distinguished, when a missing object is
built, and how a message sent to a stale owner is filtered out.

## 1. Don't Distinguish Kind By A True/False Marker

Spot kind is a closed value set — `Invalid = 0`, `Entry = 1`,
`User = 2`, `Instance = 3`
([Glossary](../spec/01-glossary.en.md#spot-kind)). The three kinds
behave differently.

| Kind | When it's created | Move | Return-wait |
|---|---|---|---|
| Entry Spot | One per node when the Object Server starts | **Never** | Can't be used |
| User Spot | When creation is explicitly requested | Yes | Only in `SpotWide` |
| Instance Spot | When a call explicitly declaring intent to create first arrives | Yes | Can be used |

**Decision — represent the three kinds as separate types.** Attaching
a marker like `entry_spot` or `instance_spot` to one type means the
type can't stop a combination where two are true at once, and rules
that differ per kind scatter into conditionals. One implementation
actually uses this marker approach, with return-wait permission
folded into a conditional checking that marker.

Putting three sibling types over a common base collects per-kind
differences at the type boundary.

**Per-language discretion.** Whether to express this with inheritance,
composition, or a tagged union is free. The observation standard is
"can an impossible combination be constructed."

## 2. What "Entry Spot Doesn't Move" Means

The Entry Spot **instance** belongs to that Object Server's lifecycle,
so it doesn't go into the move-candidate list
([Spot Model 「4.2 Entry Spot's Actor Lifecycle」](../spec/11-spot-model.en.md#42-entry-spots-actor-lifecycle)).

This is frequently mismatched here — **an Actor that was on the Entry
Spot does move.** What doesn't move is the Entry Spot itself. If
"Actors belonging to an Entry Spot" are excluded wholesale when
picking move candidates, those Actors disappear when the node goes
down.

## 3. When To Build A Missing Object

**An ordinary message never builds a missing object.** Only a
Spot-dedicated call that explicitly declares intent to create can
build a new one; an ordinary message and a lookup call only target an
already-ready object
([Spot · Actor Routing 「2.2 The Condition For Using A Recent Ready Route」](../spec/18-object-routing.en.md#22-the-condition-for-using-a-recent-ready-route)).

Without this distinction, one typo builds an object. A message sent
with a wrong ID would create a new object for that ID, and no one
would ever clean it up.

### When Multiple Attempt To Create At Once

If several callers try to build the same object at once, **only
whichever secures the creation authority first** records itself as
owner and creates it. The rest target the already-created object. The
factory runs exactly once.

**Decision — don't cache the in-progress-creation state.** Since
"being created" is a state about to change, it isn't put into
[6. Target Selection And Route Cache](06-routing-and-cache.en.md)'s
cache. Caching it would keep showing "being created" for the cache
lifetime even after creation finished.

```mermaid
sequenceDiagram
    participant A as caller A
    participant B as caller B
    participant S as Location Store
    participant F as factory

    A->>S: claims creation authority
    B->>S: claims creation authority
    S-->>A: secured
    S-->>B: already claimed
    A->>F: runs
    Note over B: waits. Not put into cache
    F-->>A: object
    A->>S: records itself as owner · Ready
    S-->>B: the Ready object
    Note over A,B: the factory runs only once<br/>the losing side targets the created object
```

### The Publication Order Of The Ready Record And The Target Route

For an Instance Spot, `Ready` being recorded in the Location Store
alone doesn't finish the target node's receive readiness. The target
runtime must also reflect the same route into its internal
`instance intent` projection immediately. This projection isn't an
authoritative record replacing the Store — it's a local view that uses
the already-validated `Ready` route for application message admission.

So the order is as follows.

1. The target node commits `Ready` authority to the Location Store.
2. In the same synchronous continuation where the commit succeeds, the
   target runtime registers the `instance intent`.
3. After that, the activation continuation enqueues the first
   application message.

If step 2 is pushed back, `Ready` exists in the Store but the target
runtime has no route, and the first application message can end in
`NotFound` or a stale-route error. Re-registering the same route in a
following continuation is a safety net that recovers the omission, and
it must run before the first admission. Re-registering the same route
is handled idempotently so it doesn't create duplicate execution.

If the losing side **caches "being created,"** the last two lines of
this diagram get delayed by the cache lifetime.

### If Creation Fails Midway

If creation fails midway, the leftover record must be cleaned up. One
implementation leaves the creation-progress state in the store and
sweeps incomplete records at startup. Without deciding who's
responsible for cleanup, a failed creation permanently occupies that
ID.

## 4. Filtering Out A Message Sent To A Stale Owner

Since owner info is cached, the owner the sender knows may have
already changed. The receiving side must filter this out.

**Decision — the filtering criterion is owner identity and validity
period. Not object generation.**

[ObjectGeneration](../spec/01-glossary.en.md#objectgeneration) is
**not** a targeting condition for an ordinary message
([Spot · Actor Routing 「2.5 Where ObjectGeneration Is Used And Where It's Not」](../spec/18-object-routing.en.md#25-where-objectgeneration-is-used-and-where-its-not)).
Checking object generation as a condition for ordinary messages too
would reject every normal message right after an object is recreated.
Object generation is used to filter lifecycle changes and move relay.

| Checked | What it filters |
|---|---|
| Owner identity | This node is no longer owner |
| Owner validity period | It's owner, but past the deadline |
| Object generation | Applies only to lifecycle change and move relay |

```mermaid
flowchart TB
    M["message arriving via a stale route"] --> O{"is this node<br/>still owner"}
    O -- "no" --> X["stale-route error<br/>no automatic retry"]
    O -- "yes" --> L{"is owner validity<br/>period remaining"}
    L -- "no" --> X
    L -- "yes" --> G["object generation isn't checked"]
    G --> Q["enqueued"]
    G -. "if generation were checked too" .-> W["every normal message<br/>right after recreation would be rejected"]
    K["lifecycle change<br/>move relay"] --> GEN["object generation is checked only here"]
```

The left axis is the path an ordinary message goes through. **The spot
where you'd want to add a generation check at `G` is the pitfall**, and
the right-side `K` is the only path where generation is checked.

**Decision — a filtered message ends in a stale-route error. The
runtime doesn't automatically retry.** Retrying would let the sender
see success while it may actually have executed twice. The application
can start a new call, and at that point the risk of duplicate
execution is judged by the application.

## 5. When To Clean Up An Active Object, And What Bounds It

### Idle Cleanup Targets Only Instance Spots

This chapter's idle-cleanup standard is owned by
`ZLinkSpotNodeCatalog` on the .NET runtime. When the configured
`InstanceSpotIdleTimeout` is positive, the catalog periodically checks
candidates. Each check examines at most 64, and the last check
position carries over to the next cycle, so maintenance work doesn't
monopolize application dispatch even with a large number of Spots.

Candidates are limited to Instance Spots. Only an activation with no
Actor membership, not participating in relocation or Message Follow,
and with no application work pending, becomes a candidate. It's only
accepted as a candidate once the timeout has passed since the last
application work finished.

Once a cleanup transaction starts, the catalog merges it with other
close requests for the same Spot ID. After reconfirming serial
quiescence, it calls the closing callback with reason `IdleEvicted`,
disposes the activation, and releases the Spot location in the
Location Store. So no new work is accepted while the callback is
running, and the location isn't cleared before the callback finishes.

If, during this process, the location row is still being released
while an Instance intent request uses the previous route, the .NET
runtime invalidates the route and re-reads until the location becomes
Missing or newly Ready. This behavior isn't a retry resending an
already-accepted application request — it's an owner-route refresh to
decide cold activation. This behavior isn't applied to a normal Spot's
stale-route error.

The idle-cleanup state of other Framework languages, and the common
process-verification result, aren't treated as complete just from this
.NET structural explanation. Each language must separately confirm the
same shutdown condition and process evidence.

### The Ceiling Exists, But Is Used Only At The Placement Stage — Not Enough

The active-object-count ceiling must be applied at **both placement
selection and local activation** — excluding a node near the ceiling
from candidates, or rejecting new object creation. If **reducing**
already-created objects and **new activation** are treated as separate
judgments, a request already pointing at that node can bypass the
ceiling.

**Decision — the ceiling is used at two points.**

| Point | What it does |
|---|---|
| Placement selection | Excludes a node near the ceiling from candidates for a new object |
| **Local activation** | If over the ceiling, **rejects activation at that node** |

Blocking only at the placement stage lets a request already pointing
at that node, or an object incoming via a move, simply pass through
the ceiling.

### What The Cleanup Criterion Is

**Decision — the cleanup target is Instance Spot only.** The formal
spec added the `IdleEvicted` shutdown reason limited to Instance Spot
([Spot Model](../spec/11-spot-model.en.md)). The reason User Spot
isn't cleaned up is that **a cleaned-up User Spot doesn't come back to
life from an ordinary message** — only a call explicitly declaring
Instance intent can build a missing object (§3). Entry Spot belongs to
that Object Server's lifecycle, so it's never a target to begin with
(§2).

**Decision — the cleanup criterion must satisfy both "elapsed time
since last activity" and "no work currently in progress."** Looking at
time alone would delete an object with a long-waiting operation still
on it.

**Decision — Framework doesn't preserve application state on
cleanup.** State that needs to be kept is saved by the application
itself directly in the shutdown callback
([Spot Model 「6.2 Cleaning Up An Idle Instance Spot」](../spec/11-spot-model.en.md#62-cleaning-up-an-idle-instance-spot)).
For Framework to save state on the application's behalf, it would need
to know what to save, and that's the application's job.

## 6. Which Unit Memory Accounting Uses

Process-unit byte accounting and Spot-unit byte accounting are
different accounting units. Process-unit byte accounting bounds
pending-receive payload in bytes, and Spot-unit byte accounting bounds
per-lane work count and bytes together. One side's number isn't
reused as the other side's ceiling.

The current .NET `ZLinkSerialExecutionQueue` keeps application and
lifecycle as separate FIFO lanes, each with a count/byte reservation.
The application default is 4,096 items/64 MiB, the lifecycle default
is 256 items/4 MiB, and accepted application work reserves the payload
plus a fixed per-work cost together. The reservation is returned at
handler terminal completion. The relocation hold uses a separate
1,024 items/16 MiB ceiling.

So the Spot queue can saturate first even when the process HWM has
room left, and conversely, process inbound admission can stop first
even when the Spot queue has room. The two results aren't lumped into
the same `CapacityExceeded` situation — they're distinguished by the
owning queue's admission result.

### The Queue Bound Is Set By Accumulated Payload Size

**Decision — the execution queue's bound enforces both the count and
byte axes, and applies whichever is hit first.** The formal spec
mandates both axes
([Framework API](../spec/06-framework-api.en.md)).

A single axis alone can be routed around via the other axis. Count
alone lets a few large payloads fill memory; bytes alone lets empty
payloads pile up indefinitely without hitting the bound.

**Decision — byte accounting doesn't count only payload size.** It
includes the envelope, metadata, and queue node one pending work item
occupies. In a language where this can't be calculated exactly, use a
value with a fixed per-work cost added. Even an empty payload, one
work item isn't 0 bytes.

The queue bound exists for two reasons — deciding how much memory
stays tied up, and judging how much work is backed up. **Count alone
tells you neither.**

The same 4,096 items is 400 KB at 100 bytes each, and 4 GiB at 1 MiB
each. Memory differs by 10,000x while the bound triggers the same. The
time to drain is the same story — throughput moves closer to bytes per
second than items per second, so measuring the backlog requires
measuring in bytes.

A count bound is wrong in both directions.

| Situation | Result of a count bound |
|---|---|
| Small messages pile up | Rejects on hitting the bound despite memory headroom |
| Large messages pile up | Doesn't hit the bound while memory runs out |

Process-unit accounting is already in bytes (§6 first paragraph).
Bringing the same standard down to the Spot unit works, and since both
layers use the same unit, which layer triggered is also distinguishable.

**Decision — don't have an unbounded execution queue.** Each lane must
have both count and byte reservations, and the relocation hold must
also have a separate ceiling
([Framework API](../spec/06-framework-api.en.md)).

The result on exceeding it **isn't one thing.** It splits by
submission family and queue location, so an implementation must not
lump it into one. The table is in
[2. Spot · Actor Execution Serialization 「2. The Pitfall When Building Execution Authority」](02-serialization.en.md#2-the-pitfall-when-building-execution-authority).

Two non-queue spots aren't in that table and are each
`CapacityExceeded` — the **worker scheduler queue** and **batch
capacity.** The latter is an admission judgment, not queue saturation.

Only the pending-during-a-move bound uses count and bytes together
(1,024 items / 16 MiB). This is a value the formal spec fixed, so it's
followed as-is
([Host Relocate And Shutdown 「9. Moving Pending Messages, Timers, And Sessions」](../spec/28-graceful-drain-handoff.en.md#9-moving-pending-messages-timers-and-sessions)).

## 7. Result To Confirm

- The three Spot kinds are distinct types, and a value where two kinds
  are true at once can't be constructed.
- Entry Spot isn't in the move-candidate list, and an Actor that was
  on an Entry Spot does move.
- Sending an ordinary message with a missing ID doesn't build the
  object.
- A call that explicitly declares intent to create does build a
  missing object.
- Even if several callers request creating the same object at the same
  time, the factory runs only once.
- The "being created" state doesn't stay in the location cache.
- A record from a failed creation is cleaned up at startup.
- Even right after an object is recreated, ordinary messages aren't
  rejected for generation mismatch.
- A call sent to a stale owner ends in an error without automatic
  retry.
- Once active-object count hits the ceiling, activation is rejected at
  that node.
- Idle-cleanup targets are limited to Instance Spot. Entry Spot and
  User Spot aren't cleaned up.
- An Instance Spot with work in progress isn't cleaned up even after
  the idle time passes.
- On cleanup, the closing callback is called with shutdown reason
  `IdleEvicted`.
- The execution queue is bounded on both count and byte axes.
- When large messages pile up, the byte bound hits before the count
  bound.
- When empty payloads pile up, the count bound hits before the byte
  bound.
- Byte accounting includes a fixed per-work cost, so even an empty
  payload consumes the bound.
- There's no unbounded execution queue.

---

[Internal structure table of contents](README.en.md) · [Previous: 7. Receive And Dispatch Loop](07-dispatch-loop.en.md) · [Next: 9. Session And Actor Binding](09-session-binding.en.md)
