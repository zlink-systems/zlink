---
title: "State Ownership And State Lanes"
---

# State Ownership And State Lanes

[Execution topic table of contents](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 05. Payload Ownership And Codec](05-payload-ownership-and-codec.en.md) · [Next: 07. Serial Executor Layers](07-serial-executor-layers.en.md)

> This document defines the mechanism by which a component guards its own mutable state —
> a primitive contract guaranteeing that only one turn touches that state at a time. Every
> language runtime must follow this contract. Observable behavior such as ordering, timeouts,
> and error codes is owned by other documents, and the rules this document defines do not
> change that observable behavior.

## 1. State Ownership Overview

A component owns mutable state — fields and collections. This document defines the rules
for the mechanism that guarantees only one piece of code touches that state at a time: the
criteria for classifying which state needs which primitive, the guarantees that primitive
must provide, and how to structure code so reentrancy cannot arise.

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

```csharp
// contract pseudocode, not the real API — the real signatures are owned by each language interface.
await lane.Run(async () =>
{
    if (!entries.TryGetValue(key, out var entry)) return;   // a plain map. not locked
    await SendAsync(entry.Route);                           // same turn — the value cannot go stale
});
```

**The point is that the `await` sits inside the turn.** A lock cannot wrap an `await`; a lane
turn can. So "read → release → act" becomes "read → act".

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

```csharp
// contract pseudocode, not the real API — the real signatures are owned by each language interface.
class RouteRegistry            // C1 — lookup/add/remove only, no invariant spanning anything
{
    ConcurrentMap<RouteKey, Route> routes;
}

class BindingTable             // C2 — the two maps reference each other. change one and it breaks
{
    ZLinkStateLane lane;
    Map<ActorId, SessionId> actorToSession;   // plain map
    Map<SessionId, ActorId> sessionToActor;   // the lane provides exclusivity

    Task Bind(actorId, sessionId) => lane.Run(() =>
    {
        actorToSession[actorId]   = sessionId;   // both change in one turn
        sessionToActor[sessionId] = actorId;
    });
}

class SendCounter              // C3 — increments only
{
    Atomic<long> sent;
}
```

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

```csharp
// contract pseudocode, not the real API — the real signatures are owned by each language interface.
interface ZLinkStateLane
{
    Lane Current { get; }        // the lane currently running, or null
    bool IsOnLane { get; }       // is the caller on this lane's turn
    Task<T> Run<T>(work);        // run in one turn of this lane and return the result
    bool TryPost(work);          // enqueue without awaiting; false if closed
    void ThrowIfReentrant();     // already on this lane → throw right here
    Task Close();                // stop accepting, finish what was accepted
}
```

These six carry the same name and the same meaning in all four languages (§7).

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
  cannot change.

If even one of these three conditions cannot be confirmed, do not wait for lane completion
while holding a gate. Either propagate the call path asynchronously, or separate the gate's
responsibility from the lane's again.

### Completion Before Return

If the original synchronous method finished registering a waiter, capturing a generation,
reading a store, or claiming ownership **before returning**, that work must still be finished
before a caller can observe the return once it moves onto a lane. Do not turn it into an
asynchronous fire-and-forget post.

```csharp
// contract pseudocode, not the real API — the real signatures are owned by each language interface.
Task<Reservation> Reserve(key)
{
    // a caller observing the return believes the reservation is already taken.
    return lane.Run(() => reservations.Add(key));
}

Task<Reservation> Reserve_WRONG(key)
{
    lane.TryPost(() => reservations.Add(key));   // returns before it is taken
    return pendingReservation;
}
```

Later stages that wait on a completion signal may stay asynchronous. The registration or
capture itself is not deferred past the return.

**A synchronous public contract does not become asynchronous merely because a state lane was
introduced.** Propagate an async signature only when the internal callers are already
asynchronous and the observable contract does not change.

## 6. Structuring So Reentrancy Cannot Arise

Reentrancy does not happen by accident. It arises structurally in three places, and each place
has a settled shape.

**Kind ① — a spot where code inside the lane calls back into the same component's public
surface.** If one public entry point, already inside a turn admitted to the lane, calls
another public method of the same component, that method also tries to enter the same lane,
which is reentrancy. Split such a spot into a private method that does not enter the lane. The
public entry point enters the lane exactly once, and code inside the lane calls the private
method's body directly.

```csharp
// contract pseudocode, not the real API — the real signatures are owned by each language interface.
Task Bind(actorId, sessionId) => lane.Run(() => BindOnLane(actorId, sessionId));
Task Rebind(actorId, sessionId) => lane.Run(() =>
{
    UnbindOnLane(actorId);              // calling Unbind() would enter the lane again
    BindOnLane(actorId, sessionId);     // inside the lane, call the body directly
});

void BindOnLane(actorId, sessionId) { ... }    // does not enter the lane
void UnbindOnLane(actorId) { ... }
```

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

```csharp
// contract pseudocode, not the real API — the real signatures are owned by each language interface.
await lane.Run(() =>
{
    state.retrying = true;
    StartRetryLoop(key);        // the context flow is broken inside here
});

void StartRetryLoop(key)
{
    using (SuppressLaneContext())           // do not hand down "currently on this lane"
        RunDetached(() => RetryLoop(key));  // if the synchronous prefix can reach the lane,
                                            //   post the start itself to a separate scheduler
}
```

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

## 7. Per-Language Mapping

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

When porting to another language, rather than carrying over the concrete type or language idiom
as is, use that language's primitive that satisfies the same guarantees this document defines —
one turn executing at a time, FIFO, immediate exception detection on reentrancy, and no locking
on owned collections.

**The public surface names and contracts are the exception.** These six on the state lane carry
the same name (spelling conversion only) and the same meaning across all four languages.
Measurement confirms the four already agree.

| Contract | Meaning | .NET | java | cpp | node |
|---|---|---|---|---|---|
| `current` | the lane currently executing | `Current` | `current()` | `current()` | `current` |
| `isOnLane` | am I on this lane | `IsOnLane` | `isOnLane()` | `is_on_lane()` | `isOnLane` |
| `run` | execute in a lane turn | `RunAsync()` | `runAsync()` | `run()` | `run()` |
| `tryPost` | post without waiting | `TryPost()` | `tryPost()` | `try_post()` | `tryPost()` |
| `throwIfReentrant` | throw on reentrancy | `ThrowIfReentrant()` | `throwIfReentrant()` | `throw_if_reentrant()` | `throwIfReentrant()` |
| `close` | shut down | `DisposeAsync()` | — | `close()` | `closed` |

`throwIfReentrant` is a **required contract, not an option.** Even where an upper execution unit
guarantees serial ownership, do not trust that premise unchecked — once reentrancy slips through
it becomes a hang, and with nobody checking it surfaces as a silent deadlock.

Names and contracts for the executor layers (Spot/Actor/Session coordinators and the serial queue
primitive) are set by [07. Serial executor layers](07-serial-executor-layers.en.md).

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
an `await` and that needs no public-reentrancy detection is not converted to a Promise. A state
lane is needed only on the asynchronous paths where state splits across
an `await`, and the surfaces that need reentrancy detection.

## 8. Verification Requirements

The following are checked using the public surface only (the return value and exceptions of
work submitted to a state lane, the error a reentrant call receives, and the state observable
at the moment a component method returns). Each item maps to one test.

**Single execution and order**

- When different callers submit work to the same lane concurrently, no update to the plain,
  unlocked collection the lane owns is lost.
- Work submitted to the same lane executes in submission order.
- Submitting a result-awaiting call to a closed lane ends immediately with an exception, and a
  submission that does not await a result returns a failure.

**Reentrancy**

- Code already executing on that lane's turn that tries to enter the same lane again does not
  hang; it ends immediately with an exception at that call site.
- A long-running operation started from a lane turn enters the same lane normally once the
  original turn has ended, and produces no false reentrancy exception. The same holds for a
  long-running operation with a synchronous prefix.
- External callbacks run outside the turn, and the state a callback observes reflects every
  state transition the original code used to finish before the callback.

**Completion boundary**

- Completion continuations run outside the lane-current scope — signalling completion inside a
  turn does not make that continuation re-enter the same lane.
- A method with a register-or-capture-before-return contract has that registration or capture
  already finished at the moment a caller observes the return.
- Where a call waits for lane completion while holding an operation-protocol gate, that lane
  item does not re-acquire the same gate and every completion continuation is asynchronous.

**Cross-collection invariants**

- When calls that change several collections participating in one invariant arrive
  concurrently, reading two of those collections from outside never observes them disagreeing.


---

[Execution topic table of contents](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 05. Payload Ownership And Codec](05-payload-ownership-and-codec.en.md) · [Next: 07. Serial Executor Layers](07-serial-executor-layers.en.md)
