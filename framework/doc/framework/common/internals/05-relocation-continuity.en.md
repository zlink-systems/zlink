---
title: "5. Message Continuity During A Move"
---

# 5. Message Continuity During A Move

[Internal structure table of contents](README.en.md) · [Previous: 4. Operation Completion Confirmation — Only One Finalizes](04-completion.en.md) · [Next: 6. Target Selection And Route Cache](06-routing-and-cache.en.md)

> **What this chapter answers** — where a message addressed to an
> object goes while that object is moving to another node.
>
> **Contract ownership** — the move procedure's step order and store
> contract are owned by
> [Host Relocate And Shutdown](../spec/28-graceful-drain-handoff.en.md)
> and [Location Runtime](../spec/21-location-runtime.en.md). This
> chapter covers the **structure** that satisfies that contract, and
> the mismatches actually observed across the four implementations.

While a running object moves to another node, where does a message
addressed to it go? This document covers **how a message is handled at
each boundary**, rather than the move procedure itself. The step order
of the procedure and the store contract are owned by the formal spec
([Location Runtime](../spec/21-location-runtime.en.md),
[Host Relocate And Shutdown](../spec/28-graceful-drain-handoff.en.md)).

## 1. Four Boundaries

From a message's point of view, a move splits into four spans. The
fate of an arriving message differs in each span.

```mermaid
flowchart LR
    A["① Prepare<br/>confirm receiving slot"] --> B["② Block<br/>stop new work"]
    B --> C["③ Switch<br/>swap owner"]
    C --> D["④ After<br/>new owner handles it"]
```

| Span | Arriving message | Reason |
|---|---|---|
| ① Prepare | **Handled as usual** | If there's no slot to receive it, the move doesn't even start. Blocking before this check leaves only wasted stopped time on failure |
| ② Block | **Held, then handed to the new owner** | Dropping it loses it; rejecting it exposes the move to the caller |
| ③ Switch | Held or fails | This span must be as short as possible |
| ④ After | **Delivered to the new owner even if it arrives at the old address** | The sender may still know the old location |

The order in ① is a design decision — **new work on the source isn't
blocked until the receiving slot is confirmed**
([Host Relocate And Shutdown 「8.2 The Common Order Every Actor And Spot Follows」](../spec/28-graceful-drain-handoff.en.md#82-the-common-order-every-actor-and-spot-follows)).
Reversed, if the move fails for lack of a slot, that object would have
been stopped for no reason at all.

## 2. Span ② — Holding And Order

A message arriving after the block is held in a bounded slot. The
bound is **1,024 items / 16 MiB per move**, and exceeding it ends a
call waiting for a response in `Unavailable`, and one not waiting in a
drop
([Host Relocate And Shutdown 「9. Moving Pending Messages, Timers, And Sessions」](../spec/28-graceful-drain-handoff.en.md#9-moving-pending-messages-timers-and-sessions)).

There's one rule for order — **restored prior work runs before the
messages held during the move**
([Spot Model 「3.1 During Relocation, The Temporary Queue Is Checked First」](../spec/11-spot-model.en.md#31-during-relocation-the-temporary-queue-is-checked-first)).
Reversed, a request already in the queue before the move would be
processed after a newly arrived request during the move, flipping send
order against processing order.

The guarantee scope only reaches **acceptance order per target.**
There's no global order guarantee across messages arriving from
different paths.

### Where It Meets Execution Serialization

The structure from
[2. Spot · Actor Execution Serialization](02-serialization.en.md)
comes into play here again. Holding and restoring must be **splittable
per Actor.** When the unit of a move is a single Actor, only that
Actor's remaining work must be picked out. If the per-Actor queue is
merged into one just because it's `SpotWide`, it can't be split at
this point — this is where the reason the queue must be per Actor
while only the execution authority is shared comes back up.

## 3. Span ④ — A Message Arriving At The Old Address

Even after a move finishes, the sender may still know the old location
for a while. Handing that message off to the new owner is
**[Message Follow](../spec/01-glossary.en.md#message-follow)**, and
its default active period is **30 seconds**
([Location Runtime 「6.3 Delivering A Message Arriving At A Previous Owner To The New Owner」](../spec/21-location-runtime.en.md#63-delivering-a-message-arriving-at-a-previous-owner-to-the-new-owner)).

| Limit | Value and scope |
|---|---|
| Active period | 30 seconds by default. Per move |
| Forwarding hops | Up to 8 chained forwards |
| Forwarding volume | 1,024 items / 16 MiB per move |

| Situation | Result the caller observes |
|---|---|
| Forwarding loops back to where it started | `Unavailable` |
| Object generation doesn't match | `InvalidOperation` |
| Forwarding volume bound exceeded | `CapacityExceeded` |

When forwarding, the call identifier, object generation, payload, and
response path are kept exactly as-is. Not keeping them means
[4. Operation Completion Confirmation](04-completion.en.md)'s
completion slot can't be found, and the caller hangs until timeout.

### This Isn't An Optional Feature

**Session connection and relay depend on this forwarding path**
([Session Actor Dispatch 「4. How A Session Holds An Actor Route」](../spec/20-session-actor-dispatch.en.md#4-how-a-session-holds-an-actor-route)).
Without implementing it, a session connected to a moved Actor won't
work correctly. Reading it as a "nice-to-have optimization" and
deferring it shows up later as an unexplainable failure on the session
side.

## 4. Span ③ — The Asymmetry Before And After Owner Switch

The owner switch is done as **one conditional change** to the store.
If even one condition doesn't match, nothing changes and `Conflict` is
returned
([Location Store Provider SPI 「4. Conditional Atomic Batch」](../spec/22-location-store-redis.en.md#4-conditional-atomic-batch)).

Failure handling is completely different depending on this one point.

| Timing | If it fails |
|---|---|
| Before the switch | The source remains owner. There's nothing to roll back |
| After the switch | **It's not rolled back to the source.** The current step is retried against the same target within a fixed time, and if the target shuts down, that object is left unusable |

The reason it isn't rolled back is this — at the moment the switch
succeeds, the target is already the official owner, and in that
interval, another participant may have seen the target as owner and
sent it messages. Rolling back to the source would erase the
processing results of those messages.

### So Steps After The Switch Must Be Idempotent

Since it can't roll back, only forward retries remain. So each step
after the switch is built so that **receiving the same request again
produces the same result as receiving it once.** Receiving the same
restore request again doesn't start over — it uses the state already
in progress
([Host Relocate And Shutdown 「8.2 The Common Order Every Actor And Spot Follows」](../spec/28-graceful-drain-handoff.en.md#82-the-common-order-every-actor-and-spot-follows)).

Only one thing in this span must have no intermediate state —
**switching which node receives processing** must change in one shot
([Host Relocate And Shutdown 「8.2 The Common Order Every Actor And Spot Follows」](../spec/28-graceful-drain-handoff.en.md#82-the-common-order-every-actor-and-spot-follows)).
An intermediate state here means two nodes process the same object at
once.

## 5. Don't Split The Move Path Into Multiple Branches

**Decision — one state-transition rule owns the move of one object or
bundle.**

The formal spec only decides the step order and progress-stage values,
not the component decomposition. But splitting into branches means
§4's asymmetric handling gets reimplemented per branch, and when a
failure happens in the middle, **which branch owns cleanup
responsibility** can't be read off.

One implementation has the move path split into three branches using
two unrelated sets of stage values. Another implementation has it
owned by a single transition rule. The latter is taken as the
standard. This is a part the formal spec left undecided that internals
decides.

## 6. Result To Confirm

- New work on the source isn't blocked until the receiving-slot check
  finishes.
- A message arriving after the block isn't lost and is delivered to
  the new owner.
- Restored prior work runs before messages held during the move.
- A call exceeding the holding bound ends in `Unavailable`.
- A message sent to the old address right after a move is delivered to
  the new owner within 30 seconds, with the call identifier and
  response path preserved.
- Exceeding 8 forwards or the forwarding-volume bound ends in the
  defined error.
- If the owner switch ends in `Conflict`, no value in the store
  changes.
- If it fails after the owner switch, the source doesn't become owner
  again.
- Receiving the same restore request twice produces the same result as
  once.
- When the move unit is a single Actor, only that Actor's remaining
  work is split out and moved.

---

[Internal structure table of contents](README.en.md) · [Previous: 4. Operation Completion Confirmation](04-completion.en.md) · [Next: 6. Target Selection And Route Cache](06-routing-and-cache.en.md)
