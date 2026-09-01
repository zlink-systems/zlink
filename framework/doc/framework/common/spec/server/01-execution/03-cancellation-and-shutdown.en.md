---
title: "Cancellation And Shutdown"
---

# Cancellation And Shutdown

[Execution topic table of contents](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 02. Handler Turn And Execution Gate](02-handler-turn-and-execution-gate.en.md) · [Next: 04. Application Job Queue And Backpressure](04-application-job-queue-and-backpressure.en.md)

> This document defines what cancellation can and cannot do to work already
> accepted, what wins when cancellation, timeout, shutdown, and acceptance
> race at the same time, and how a MeshNode cleans up in-flight operations
> when it transitions to `Relocating`/`Draining`. Where a call completes and
> the structure that fixes that completion belong to
> [Submit And Completion](01-submit-and-completion.en.md); handler execution
> order and gate return belong to
> [Handler Turn And Execution Gate](02-handler-turn-and-execution-gate.en.md).

## 1. Cooperative Cancellation

- Cancellation is a cooperative request.
- An already-completed result is not turned into a cancellation, and delivery
  of an already-accepted one-way message is not cancelled.
- While waiting in a Framework-owned queue, cancellation can remove the record
  and prevent Core submit from starting. Once Core owns the payload through a
  successful submit, it can only stop the caller's wait; it does not cancel
  Core admission or the request.
- The socket owner drains a completion that arrives after the caller stops
  waiting and cleans up its native payload. That completion does not complete
  the caller again.
- The per-language surface uses `.NET` `CancellationToken`, Java
  `CompletionStage.toCompletableFuture().cancel(false)`, Kotlin coroutine
  cancellation, and Node.js `AbortSignal`.
- The `toCompletableFuture()` of a stage the Java Framework returns is tied to
  the cancellation and cleanup of the original pending admission.
- The C++ one-way `async()` terminal provides no separate public cancellation input.
- Not using a C++ task, or simply not holding onto a Java stage, does not by
  itself guarantee the operation was cancelled.

## 2. Pre-Cancelled Call

The rules for a call that arrives already pre-cancelled are:

- The call validates arguments, handles, and one-shot state first.
- `.NET`'s pre-cancelled `CancellationToken` and Node.js's already-aborted
  `AbortSignal` do not start runtime admission for an otherwise valid call —
  they complete with that language's cancelled awaitable.
- Java's and Kotlin's submit have no cancellation input.
- A valid, ordinary JVM call returns the stage to the caller only after its
  first non-blocking admission attempt, so a Java `cancel(false)` the caller
  runs after receiving the stage, or a Kotlin coroutine cancellation that
  awaits that stage, cannot cancel that first attempt.
- If the operation is pending, this cancellation races binding completion and
  clears queue and payload reservations.
- Therefore, the JVM path does not guarantee transport attempt 0 as a result
  of pre-cancellation.

| Language | Cancellation input | Can the first admission attempt be cancelled? |
|---|---|---|
| .NET | `CancellationToken` | A pre-cancelled token does not start runtime admission |
| Node.js | `AbortSignal` | An already-aborted signal completes immediately as a cancelled awaitable |
| Java | `CompletionStage.toCompletableFuture().cancel(false)` | No — the stage is returned only after the first non-blocking attempt, so that attempt cannot be cancelled |
| Kotlin | Coroutine cancellation of the linked stage | No — same reason as Java |
| C++ | No separate public cancellation input | Not applicable — not using the task does not guarantee cancellation |

## 3. Handling The Cancellation Race

- Cancellation is an exceptional completion.
- While a record waits in the Framework queue before Core submit, cancellation, timeout,
  shutdown, and admission race; only the one decided first determines the caller result and
  record handling. The structure itself — one completion slot being raced for — is owned by
  [Submit And Completion "10. Operation Identity And Where Completion Happens"](01-submit-and-completion.en.md#10-operation-identity-and-where-completion-happens-implementation).
- A cancellation decided after Core owns the payload through a successful
  submit completes only the caller as cancelled. Core can admit later once
  local capacity becomes available; the runtime drains that completion without
  delivering a second terminal result to the caller.
- [Logical Multicast](../00-foundation/02-glossary.en.md#logical-multicast) — delivering one
  message to multiple [Spot](../00-foundation/02-glossary.en.md#spot)s in the same Channel by
  ChannelName and topic — cancellation follows the bounded I/O executor submission and commit
  boundary in §4 below.

```mermaid
flowchart LR
    A["Admission starts"] --> O{"Has Core taken ownership<br/>of payload?"}
    O -->|no| X{"What is decided first?"}
    X -->|cancellation| R1["Completes with cancellation<br/>without Core submit"]
    X -->|timeout| R2["Completes with DeadlineExceeded"]
    X -->|shutdown| R3["Completes with ShuttingDown"]
    X -->|admission| R4["Core successful submit"]
    O -->|yes| R4
    R4 --> C{"caller cancellation?"}
    C -->|no| R5["Caller observes Core completion"]
    C -->|yes| R6["Only caller completes with cancellation<br/>Core may admit late"]
    R6 --> R7["Drain late completion and clean up payload"]
```

## 4. Logical Multicast Cancellation

The rules for Logical Multicast cancellation's bounded I/O executor
submission and commit boundary are as follows. The Framework service runtime
submits the publish operation to a [bounded I/O
executor](../00-foundation/04-interaction-model.en.md#5-spot-logical-multicast),
and that executor starts the publish transaction once it has secured a
worker slot.

- Cancellation can only block the operation from starting before the
  executor secures a worker slot and starts the publish transaction. Once
  the worker slot is secured and the transaction has started, cancellation
  can no longer block it.
- Cancellation after the publish transaction has started does not interrupt
  the committed snapshot operation, and does not return per-target
  observation data or turn it into publish-only monitoring values.
- `.NET` `ValueTask` and Node.js `Promise` do not change their completion
  because of a cancellation signal after commit.
- `cancel(false)` on a Java stage, and the linked stage cancellation in
  Kotlin, both return `false` and do not cancel the underlying operation.
- In Kotlin, an already-cancelled caller coroutine keeps its cancelled state,
  but the shared `CompletionStage` and the runtime operation evidence still
  record a final normal completion and monitoring event. This is not
  operation cancellation.
- Drain/shutdown also wait for started transactions to complete, and follow
  the whole runtime's bounded force-stop rule only once the host drain
  deadline is exceeded.

```mermaid
flowchart LR
    S["Logical Multicast starts"] --> C{"Are executor handoff and<br/>transaction start atomically fixed?"}
    C -->|before fixed| Cancel["Cancellation can block the start"]
    C -->|after fixed, committed| Committed["Cancellation does not interrupt<br/>the committed snapshot"]
    Committed --> Drain["Drain/shutdown also wait for completion<br/>(force stop only past drain deadline)"]
```

## 5. MeshNode Relocation And Drain

- When a [MeshNode](../00-foundation/02-glossary.en.md#meshnode) — a runtime node that
  participates in a RouteMesh to send or receive messages — transitions to `Relocating`, it is
  excluded from new [ChannelName](../00-foundation/02-glossary.en.md#channelname) — the name
  identifying the Channel scope a message is sent to — selection and Logical Multicast targets.
- A unit that did not get a relocation permit keeps its application claim
  going, and is sealed only at a queue turn boundary for units that did get
  the permit.
- After `Draining`, only already-accepted application records, request
  completion, Actor relocation, and STREAM barriers proceed, up to the
  shutdown deadline.
- After the [Deadline](../00-foundation/02-glossary.en.md#deadline) — the final time point by
  which work must finish — remaining claims are revoked and pending operations complete with a
  shutdown result.

A Draining MeshNode is also excluded from new object placement candidates.

Pending activation completes the request as a terminal exactly once, and
drops the one-way payload, at whichever boundary is reached first between
the [drain deadline](../00-foundation/02-glossary.en.md#drain-deadline) and the Framework
activation deadline.

Even if cancellation, timeout, shutdown, and the
activation barrier opening all race, the pending operation and payload
reservation are cleaned up exactly once.

```mermaid
sequenceDiagram
    participant M as MeshNode
    participant U as Unit under relocation
    participant Q as Queue turn

    M->>M: Transitions to Relocating — excluded from new ChannelName/Multicast candidates
    Note over U: Unit without a permit keeps its application claim going
    U->>Q: Sealed at the queue turn boundary where the permit was obtained
    M->>M: Transitions to Draining
    Note over M: Only already-accepted record/completion/relocation/<br/>STREAM barriers proceed, up to the shutdown deadline
    alt Completes before the deadline
        M->>M: Completes normally
    else Deadline is reached
        M->>M: Revokes remaining claims, completes pending operations with a shutdown result
    end
```

## 6. Verification Requirements

The following is confirmed using only the public surface — each language's
cancellation input, the returned completion result/error kind, the Logical
Multicast public terminal, and the placement/routing results in which a
MeshNode state transition is observed. Each item leads to one test.

**Cooperative cancellation**

- Requesting cancellation on an already-completed call does not change the
  completion result.
- An already-accepted one-way message is still delivered after a
  cancellation request.
- A call made with a pre-cancelled `.NET` `CancellationToken`, or an
  already-aborted Node.js `AbortSignal`, does not start runtime admission and
  completes with a cancelled awaitable.
- On the JVM, calling `cancel(false)` after the stage is returned does not
  cancel the first admission attempt that already started.

**Handling the race**

- While the Framework queue owns a record, even if cancellation, timeout,
  shutdown, and admission happen at the same time, the caller observes exactly
  one terminal result; if cancellation wins, Core submit does not start.
- Even if caller cancellation wins after Core owns the payload through a
  successful submit, the caller observes only the cancellation result once.
  If late admission occurs once local capacity becomes available, the runtime
  cleans up the completion and native payload without completing the caller
  again.

**Logical Multicast cancellation**

- Cancellation before the publish transaction commits can block the
  operation from starting.
- After the transaction commits, none of `.NET`, Node.js, Java, or Kotlin
  changes its completion because of a cancellation signal, and none exposes
  a per-target individual result as a public value.

**Relocation and drain**

- A MeshNode that transitions to `Relocating` is thereafter excluded from
  new ChannelName selection and Logical Multicast target candidates.
- After the `Draining` transition, already-accepted records and completions
  proceed up to the shutdown deadline, and pending operations that outlast
  the deadline complete with a shutdown result.
- A Draining MeshNode is excluded from new object placement candidates.
- Even if the drain deadline and the activation deadline race, a pending
  activation completes as a terminal exactly once.

---

[Execution topic table of contents](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 02. Handler Turn And Execution Gate](02-handler-turn-and-execution-gate.en.md) · [Next: 04. Application Job Queue And Backpressure](04-application-job-queue-and-backpressure.en.md)
