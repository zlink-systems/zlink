---
title: "State Ownership And State Lanes"
---

# State Ownership And State Lanes

[Execution topic table of contents](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 05. Payload Ownership And Codec](05-payload-ownership-and-codec.en.md)

> This document defines the mechanism by which a component guards its own mutable state —
> a primitive contract guaranteeing that only one turn touches that state at a time. Every
> language runtime must follow this contract. Observable behavior such as ordering, timeouts,
> and error codes is owned by other documents, and the rules this document defines do not
> change that observable behavior.

## 1. State Ownership Overview

A component owns mutable state — fields and collections. This document defines the rules
for the mechanism that guarantees only one piece of code touches that state at a time: the
criteria for classifying which state needs which primitive, the guarantees that primitive
must provide, and the rules to follow when converting to it.

What this document does not define — when a handler executes and when it yields its place to
another handler is owned by [Handler Turn And Execution Gate](02-handler-turn-and-execution-gate.en.md);
ownership and copying of a message on its way from the socket to the handler is owned by
[Payload Ownership And Codec](05-payload-ownership-and-codec.en.md). The rules here concern the
mechanism that protects a component's internal state, not the ordering, timeouts, or error
codes an application observes.

## 2. Terminology — State Lane Versus Application/Lifecycle Lane

This document's **state lane** is the execution unit through which one component owns its
mutable state. Every piece of code that reads or writes that component's state runs only on
this lane.

This term is a different concept from the "application lane" and "lifecycle lane" used by
[Handler Turn And Execution Gate "7. Lane Separation And Priority
(Implementation)"](02-handler-turn-and-execution-gate.en.md#7-lane-separation-and-priority-implementation).
The two documents use the same word "lane" for different things, so they must be kept apart.
The application lane and lifecycle lane exist as a pair per
[owner](../00-foundation/02-glossary.en.md#owner) — the party that currently runs the
[Spot](../00-foundation/02-glossary.en.md#spot), a stateful logical instance with an address,
or the Actor inside it — while a state lane exists per component that owns state.

| | State lane (this document) | Application lane / lifecycle lane (02 §7) |
|---|---|---|
| Unit | One component (for example, one binding table, one catalog) | One owner (Spot/Actor) |
| Purpose | Lets only one turn at a time touch that component's state | Orders the work an owner processes by priority |
| What it holds | The pieces of code that read or write that component's state | Business payloads and timer callbacks (application), or join/leave/relocation/lifecycle control (lifecycle) |
| Admission/priority | None — executes strictly in FIFO arrival order, with no count/byte caps or priority between lanes | Present — count/byte caps, an owner occupancy time budget, and a lifecycle priority rule |
| Owning document | This document | [02 §7](02-handler-turn-and-execution-gate.en.md#7-lane-separation-and-priority-implementation) |

That a component has a state lane, and which of the application lane or lifecycle lane the
handler of the Spot/Actor it belongs to is admitted into, are decisions at different layers.
While a handler execution is waiting its turn on the application lane, the state that handler
touches may be serialized on a different component's state lane.

## 3. The Prohibited Shape

A state lane rules out the following shape — **taking a snapshot of state inside an exclusive
section such as a lock, releasing that section, and then deciding on the far side of an async
boundary using that snapshot.**

```csharp
Entry entry;
lock (_gate) { if (!_entries.TryGetValue(key, out entry)) return; }   // released here
await SendAsync(entry.Route);                                         // acts on a value that may already be stale
```

This shape is not a mistake — it is **a result the mechanism forces**. A lock cannot wrap an
`await`. So code that guards state with a lock while also needing to act asynchronously on
that state ends up, without exception, in the shape "take a snapshot, release the lock, then
act on that value." By the time the code reaches a point where the lock can be reacquired
(after the asynchronous work completes), some other turn may already have changed the same
state in the meantime, so the snapshot is structurally stale.

A state lane removes this shape entirely. Because there is no release point inside a lane
turn — the code that reads the state and the code that acts on it stay in the same turn — no
snapshot is ever produced.

## 4. State Classifications And How To Tell Them Apart

A component's mutable state falls into one of the following classifications. Once classified,
the mechanism that guards it follows mechanically.

| Classification | Criteria | Mechanism |
|---|---|---|
| **C1 — pure lookup registry** | A single map whose operations end at get/add/remove, with no invariant spanning any other field or collection. Splitting the exclusive-access block into two atomic operations does not break any invariant | Concurrent map (the language's thread-safe map implementation) |
| **C2 — cross-invariant state** | An invariant spans multiple collections/fields, or a decision made here continues into asynchronous action that follows from it | State-lane ownership + a plain map (unlocked) |
| **C3 — atomic counter/flag** | Only an integer increment, a flag check, or a single reference swap | Atomic operations (the language's atomic increment/compare-and-swap/volatile reference) |

When a component mixes all three, **C2 wins** — the strongest requirement decides the
mechanism for the whole component. Moving only part of a component to a lane while leaving the
rest on a concurrent map or a separate lock lets the cross-invariant violation C2 exists to
prevent reappear at that boundary.

**Do not replace C2 with a concurrent map.** A concurrent map makes only the individual
operation on one map atomic. An invariant spanning multiple collections/fields is not
preserved by per-map atomicity — atomicity fractures down to that one map, and the invariant
breaks with it.

**Do not replace C2 with a different exclusive-access primitive (a semaphore, for example).**
Swapping the exclusive-access primitive from a lock to a semaphore leaves the shape
[§3](#3-the-prohibited-shape) rules out fully intact — all that changes is a number (the lock
count went down), while the structure of "release the exclusive section, then act on a
snapshot" is unchanged.

## 5. What A State Lane Guarantees And Requires

A state lane guarantees the following.

- **Only one turn executes at a time.** Two turns on the same lane never execute concurrently.
- **It is FIFO.** Work executes in the order it entered the lane. There is no front-of-queue
  insertion.
- **It is non-reentrant.** If code already executing on that lane's turn tries to enter the
  same lane again, that attempt is refused.
- **It does not lock the collections it owns.** A collection holding state the lane owns
  stays an ordinary structure — it is the lane's single execution, not a lock, that provides
  exclusivity, so the collection itself has no need to be thread-safe.

**A reentrancy violation must be detected as an exception, not a deadlock.** Why reentrancy
has no way to proceed shows up once the sequence is spelled out concretely.

1. Public method A of some component is already running on the lane's turn.
2. A's body calls public method B of the same component.
3. B also needs to enter the same lane to touch the state, so it waits for "the turn currently
   running on this lane" to finish.
4. But the turn currently running on this lane is A itself — the one that called B.

B is waiting for A to finish, and A cannot finish its own turn until B finishes — which comes
down to A waiting for itself. No other turn can break this wait, so it never ends.

Left as a hang, this stops the server silently. Nothing in a log or stack trace names the call
that caused it, so finding the cause means combing through an execution dump taken at that
moment. The lane must instead end this at the exact call site where the reentrant call
happened, as an exception. Raising an error right there that names which lane was re-entered
means the stack trace points straight at the offending code.

## 6. Removing Reentrancy

When converting a component classified as C2 to a lane, reentrancy is removed first. In
practice a conversion encounters the following kinds of reentrancy.

**Kind ① — a spot where code inside the lane calls back into the same component's public
surface.** If one public entry point, already inside a turn admitted to the lane, calls
another public method of the same component, that method also tries to enter the same lane,
which is reentrancy. Split such a spot into a private method that does not enter the lane. The
public entry point enters the lane exactly once, and code inside the lane calls the private
method's body directly.

**Kind ② — a long-running asynchronous operation started inside a lane turn inherits lane
ownership.** If a turn starts an asynchronous operation that completes only after a delay,
such as a timeout, the context that operation runs in can inherit the marker "currently
executing on this lane" as-is. When the delay ends and that operation tries to enter the same
lane again, it is detected as reentrancy even though the original turn has, in fact, already
ended. At the point where such a long-running operation is started, break the flow of
execution context so the operation enters the lane as a new caller — in a language where the
lane-ownership marker is inherited by an asynchronous context, explicitly break that
inheritance at the point where the long-running operation starts.

## 7. Signature Conversion Rules

The signature of a state-accessing method converts under the following rules.

- **Changing a synchronous return to an asynchronous return is allowed.** If the caller was
  already consuming that value on an asynchronous path, the value was a snapshot to begin
  with — matching the return style to asynchronous does not change existing observable
  behavior.
- **An out parameter is folded into the return value.** One return value carries both success
  and result together.
- **An out parameter that also returned a value on failure is not mechanically replaced with a
  nullable scalar.** Collapsing success and value into one scalar can't express a case where a
  failure also needs to carry a value alongside it — for example, when a caller must still
  receive the current high-water value at the time of rejection even though the call was
  rejected, a single nullable scalar cannot hold both the success value and the failure-side
  extra value at once. Preserve such a case with a result type that carries success, the value,
  and any failure-side extra value together. A mechanical nullable replacement changes
  observable behavior and is not allowed.

## 8. Conversion Unit And Verification

- **The conversion boundary reuses the existing gate unit.** The scope one exclusive-access
  section already guards is already the ownership unit. Converting it does not widen or split
  that boundary — boundary redesign is not the purpose of this conversion.
- **The conversion unit is one class.** One component — the one exclusive-access section its
  class holds — moves at a time.
- **Every conversion must pass verification before moving to the next.** What to check is
  owned by [Verification Requirements](#10-verification-requirements).
- **The success metric is not the lock count.** A drop in the number of exclusive-access
  statements is not evidence. What must shrink is the count of "snapshots used across an async
  boundary," compared before and after per component. This comparison is an **internal
  confirmation condition** measured by internal instrumentation rather than the public
  surface, and is not carried into the verification requirements section.

## 9. Per-Language Mapping

In .NET, `Zlink.Framework.Runtime.Execution.ZLinkStateLane` is the reference implementation of
the state lane this document defines. State access runs as work submitted to this lane, and
the collections the lane owns stay plain `Dictionary` instances. Reentrancy is detected, not as
a hang, but as an `InvalidOperationException` raised immediately at the call site.

`ZLinkStateLane` is separate from `ZLinkSerialExecutionQueue`, which is used for Spot/Actor
**execution**. `ZLinkSerialExecutionQueue` carries relocation sealing and lifecycle admission
together, responsibilities a state owner does not need — a component that only wants to guard
its state, borrowing this execution queue instead, would take on the relocation/lifecycle
responsibilities that queue carries along with it. For that reason, state ownership does not
use this execution queue.

When porting to another language, rather than carrying over the concrete type or API shape as
is, use that language's primitive that satisfies the same guarantees this document defines —
one turn executing at a time, FIFO, immediate exception detection on reentrancy, and no locking
on owned collections.

## 10. Verification Requirements

The following is confirmed using only the public surface — the return value and exception of
work submitted to a state lane, the error a reentrant call receives, and the converted
language's unit test and sample gate results. Each item leads to one test.

**Reentrancy And Execution Order**

- If code already running on a lane's turn attempts to re-enter the same lane, this ends in an
  exception raised immediately at the call site, not a hang.
- If different callers submit work to the same lane concurrently, updates to the lane-owned,
  unlocked plain collection are not lost.
- Work submitted to the same lane executes in the order it was submitted.
- Submitting a call that waits for a result to a closed lane ends immediately in an exception;
  submitting work that does not wait for a result returns failure.

**Conversion Verification**

- Before and after a conversion, that language's full unit test suite is green.
- Before and after a conversion, the sample gate holds.
- Before and after a conversion, the ordering, timeouts, and error codes a caller observes do
  not change.

---

[Execution topic table of contents](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 05. Payload Ownership And Codec](05-payload-ownership-and-codec.en.md)
