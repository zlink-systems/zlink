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

### Distinguishing State Protection From Serializing An Operation Protocol

A semaphore, socket gate, or dispose gate that runs an entire external asynchronous operation
one at a time carries a different responsibility from the state lane that guards a component's
mutable state. A gate can stay an operation-protocol primitive only when it meets all of the
following conditions.

- What the gate protects is the start/end of an external resource operation, or exact-once
  disposal — it does not separately own any part of C2 state.
- Whatever generation, identity, or ownership the operation needs is settled inside the gate
  before the operation starts.
- After releasing the gate, code acts on a Task, a reservation, a seal token, or a resource
  whose sole ownership was transferred to it — not a mutable state snapshot.
- The completion path returns that same ownership exactly once.

Waiting for a state lane's completion from inside an operation-protocol gate is allowed. The
opposite direction — waiting, from inside a state lane's turn, for that gate to be acquired or
for a long-running operation to complete — is not. Allowing both directions creates a
back-to-back deadlock where each side waits on the other's completion.

This exception is not permission to split C2 state across multiple locks. Fields and
collections that share the same invariant are still owned by a single state lane.

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

### Completion Signals And The Blocking-Compatible Boundary

A lane work item's completion signal must run the caller's continuation only after that lane's
current-ownership marker has been released. In a language where the completion API can run a
dependent continuation inline on the completing thread, do not complete the signal directly
inside the lane-current scope. Post the completion to a scheduler outside that scope, or force
the continuation to run asynchronously.

An existing synchronous surface may block-wait on a state lane's completion only as a
compatibility boundary, and only when all of the following conditions hold.

- The submitted lane item does not reacquire any external gate it currently holds.
- Every completion signal the lane item raises runs its continuation asynchronously.
- There is a contract requiring state registration/capture to complete before that
  synchronous surface returns, or a recorded reason why the public synchronous signature
  cannot change as part of this conversion.

If even one of these three conditions cannot be confirmed, do not wait for lane completion
while holding a gate. Either propagate the call path asynchronously, or separate the gate's
responsibility from the lane's again.

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
ownership.** If a turn starts an operation such as a timeout, a retry, or a background loop,
the context that operation runs in can inherit the marker "currently executing on this lane"
as-is. When the delay ends and that operation tries to enter the same lane again, it is
detected as reentrancy even though the original turn has, in fact, already ended.

At the point where such a long-running operation is started, break the flow of execution
context. This break only takes effect once the asynchronous operation has actually crossed a
thread transition, though. If the synchronous prefix of the async function that was called can
re-enter the same lane before its first `await`, breaking the context flow alone is not
enough — in that case, post the start of the operation itself to a separate scheduler so even
the synchronous prefix runs outside the original turn. Conversely, if the first action is a
genuine asynchronous delay and there is no lane reentrancy before it, breaking the context
flow alone is enough.

**Kind ③ — an external callback invoked from inside a former exclusive-access section.**
Invoking a callback directly inside a lane turn, where that callback used to work by
reentering a monitor, makes the callback re-enter the same component's public surface. Split
this into three steps.

1. In turn A, finish validation, computing the result, every state transition the original
   code used to finish before the callback, and a placeholder ownership claim.
2. Invoke the callback outside the lane turn.
3. In turn B, either replace that placeholder with the callback result's Task exactly, or
   settle the failure.

Do not defer to turn B a state transition the original code used to finish before the
callback. A racing observer inside the callback's execution window must see the same state the
original code showed "after the exclusive section ended."

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
- **Preserve the before-return completion guarantee.** If the original synchronous method
  finished registering a waiter, capturing an epoch/generation, reading a store, or claiming
  exact ownership before it returned, that same work must still be finished before the caller
  observes the return after conversion. Do not replace it with an asynchronous
  fire-and-forget post.
- If keeping that guarantee requires a synchronous compatibility boundary, confirm the
  conditions in [§5 "Completion Signals And The Blocking-Compatible
  Boundary"](#completion-signals-and-the-blocking-compatible-boundary) and record the reason.
  Later steps that wait for a completion signal may stay asynchronous, but the registration or
  capture itself is not deferred past the return.
- If the public or per-language exact interface has a synchronous contract, do not switch it to
  returning a Promise or Task solely because a state lane was introduced. Propagate an async
  signature only when the internal caller is already asynchronous and the observable contract
  does not change.

## 8. Conversion Unit And Verification

- **The conversion boundary reuses the state region the existing gate already owned.** If one
  class held several mutually independent gates, each may move to its own ownership region.
  Doing so requires no field/collection invariant spanning both regions, and the call
  direction between the regions must be recorded as one-way.
- If even one cross-region invariant or a two-way wait exists, do not split it into multiple
  lanes — merge it into a single ownership region instead. "One class" is the default unit of
  work, not permission to create several state lanes inside one class without justification.
- An operation-protocol gate such as a socket, completion, or worker gate is excluded from
  state-lane conversion only when it meets the conditions in [§4 "Distinguishing State
  Protection From Serializing An Operation
  Protocol"](#distinguishing-state-protection-from-serializing-an-operation-protocol). Record
  the ownership transfer, generation fence, completion style, and lock order that justify the
  exclusion.
- **Every conversion must pass verification before moving to the next.** What to check is
  owned by [Verification Requirements](#10-verification-requirements).
- **The success metric is not the lock count.** A drop in the number of exclusive-access
  statements is not evidence. What must shrink is the count of "snapshots used across an async
  boundary," compared before and after per component. This comparison is an **internal
  confirmation condition** measured by internal instrumentation rather than the public
  surface, and is not carried into the verification requirements section.

The counting unit for an async-boundary snapshot is a source exclusive-access location. Count
actual language tokens, not plain-text search hits. At each location, trace whether a
value/reference/decision produced inside the exclusive-access section is used past one of the
following.

- An `await`, or returning a Task/Promise/future
- Submission to a detached task, a queue, a worker thread, or a callback dispatcher
- A completion signal that runs an asynchronous continuation
- Submission of a nonblocking transport operation

Even when it crosses that boundary, if validity is pinned by an immutable completion signal, an
exact token, a reservation, or a sole-ownership transfer, count it separately as part of the
primitive/protocol exclusion group. Carrying a mutable authorization across as-is is a
remaining defect. The final report states all three counts — `total / exclusion group /
remaining defect`.

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

In .NET, the lane-ownership marker uses `AsyncLocal`, so break that inheritance at the point a
long-running operation is started with `ExecutionContext.SuppressFlow`. If the async function's
synchronous prefix can re-enter the lane before its first `await`, post the start itself to a
separate scheduler, for example with `Task.Run`.

Java does not call `CompletableFuture.complete` from inside a lane-current `ThreadLocal`
scope. `complete` can run a dependent with no async marker inline on the completing thread.
Either release the current scope before completing, or use an API that completes on a separate
scheduler, such as `completeAsync`.

A Node.js synchronous method never runs concurrently with another callback while it finishes
inside one JavaScript turn. So a synchronous surface where state access does not split across
an `await` and that needs no public-reentrancy detection is not converted to a Promise. The
targets for a state-lane conversion are only the asynchronous paths where state splits across
an `await`, and the surfaces that need reentrancy detection.

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
- A long-running operation started in a lane turn enters the same lane normally once the
  original turn has ended, and does not produce a false reentrancy exception.
- A long-running operation with a synchronous prefix is also confirmed to start outside the
  original turn.
- The completion continuation runs outside the lane-current scope.
- An external callback runs outside the turn, and the state it observes matches the state at
  the point the original exclusive-access section ended.
- For a permitted case of waiting for lane completion while holding an operation-protocol gate,
  confirm the lane item does not reacquire that gate, and that every completion continuation is
  asynchronous.
- For a method with a before-return registration/capture contract, that registration/capture
  has already completed by the time the caller observes the return.
- A re-measurement of async-boundary snapshots shows zero remaining defects, and every
  primitive/protocol exclusion location has a recorded validity-preservation justification.

**Conversion Verification**

- Before and after a conversion, that language's full unit test suite is green.
- Before and after a conversion, the sample gate holds.
- Before and after a conversion, the ordering, timeouts, and error codes a caller observes do
  not change.

---

[Execution topic table of contents](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 05. Payload Ownership And Codec](05-payload-ownership-and-codec.en.md)
