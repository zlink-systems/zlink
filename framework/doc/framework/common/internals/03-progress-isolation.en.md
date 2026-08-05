---
title: "3. Application And Infrastructure Execution Separation"
---

# 3. Application And Infrastructure Execution Separation

[Internal structure table of contents](README.en.md) · [Previous: 2. Spot · Actor Execution Serialization — splitting queue and execution gate](02-serialization.en.md) · [Next: 4. Operation Completion Confirmation — Only One Finalizes](04-completion.en.md)

> **What this chapter answers** — what must keep progressing while a
> handler is stuck.
>
> **Contract ownership** — the receive bound and backpressure contract
> is owned by [the Framework API](../spec/06-framework-api.en.md), and
> the meaning of async completion by
> [the Async Execution Policy](../spec/05-async-execution-policy.en.md).
> This chapter covers the **structure** that satisfies that contract,
> and the mismatches actually observed across the four implementations.

While an application handler is waiting for a remote response, who
times out that call? If the timeout also waits in the same line as
the stuck handler, the call never finishes. This document covers how
to prevent that self-deadlock structurally.

## 1. The Core Decision — Split Into Two Execution Regions

Runtime work splits into two groups of different character.

| Region | What it does | Progress condition |
|---|---|---|
| application | Handler execution, Spot · Actor message, timer callback, session callback | Keeps order per Spot owner |
| infrastructure | Peer accept, send-ready notification, call-completion confirmation, owner-info update, move procedure, shutdown procedure | **Progresses regardless of what application is waiting on** |

The formal spec's requirement is stronger than "progresses
independently" — infrastructure work progresses in **an execution
region the application handler can't occupy**
([Framework API 「8.2 Handler Execution Object And Dependency Lifetime」](../spec/06-framework-api.en.md#82-handler-execution-object-and-dependency-lifetime)).

```mermaid
flowchart LR
    H["handler is waiting for<br/>a remote response"]
    subgraph I["infrastructure region"]
        T["times the call timeout"]
        P["handles peer connections"]
        S["progresses the shutdown procedure"]
        L["updates owner info"]
    end
    H -. "even while application is stuck" .-> I
    I --> R["timeout fires and<br/>the handler's wait is released"]
```

This diagram is the whole of this document. If the arrow breaks, the
handler ends up blocking the very timeout of the response it's
waiting for.

## 2. Why Separation, Not A Reserved Section

There's also an approach that reserves "N slots for infrastructure
only" within the same queue. One implementation actually does this.
This approach also prevents starvation, but **loses two things.**

First, the bound overlaps across two axes. When submission is
rejected, neither the caller nor the operator can tell "did this hit
the overall bound, or my own share's bound."

Second, and more importantly — a reservation guarantees only a
**slot**, not **progress**. If application work is waiting while
running, that execution resource is still tied up. Even with a slot
free, if there's no one to run it, infrastructure work can't progress.

The way the formal spec solves this problem is also separation, not
reservation — when pending-processing bytes hit the ceiling, **only
new application receiving stops**, while response receiving, runtime
control processing, and send-ready notification keep being processed
([Framework API 「2.1 Keeping Received Payload From Growing Memory Indefinitely」](../spec/06-framework-api.en.md#21-keeping-received-payload-from-growing-memory-indefinitely)).

**Decision — don't set aside a reserved section that pre-carves out
execution slots.** Starvation is prevented by region separation. This
is a part the formal spec left undecided that internals decides.

This doesn't mean "there's only one bound." The spec requires three
bounds with different purposes to coexist — the per-process
pending-processing byte ceiling (`06:100-113`), the internal budget for
response processing (`06:115-119`), and the bound on slots waiting for
send space (`05:69-73`). Each blocks a different target, so they
aren't merged into one. Only **splitting shares within the same
queue** is what's eliminated.

## 3. Observation Doesn't Block Progress

A status subscriber and metric collector **occupy no region's progress
authority.** If a slow subscriber slowed down message processing, the
service would get slower simply because observation was turned on.

The slot sent to a subscriber has a bound, and when it overflows, it
catches up by **coalescing into the latest status per source**. The
stream isn't cut just because the slot filled up
([Runtime Status And Operational Diagnostics 「3. Querying Current State And Observing Changes」](../spec/24-runtime-monitoring.en.md#3-querying-current-state-and-observing-changes)).
Conversely, it doesn't slow down message processing either.

## 4. How Resources Are Split Between The Two Regions

§1 only says the two regions must progress independently. How much
resource to give each is the remaining decision, and both extremes are
a problem.

| Allocation | Problem |
|---|---|
| Only one resource for infrastructure | Completion processing, peer management, and moves all pass through that one resource. As peers grow, this becomes the bottleneck |
| Generous for infrastructure too | Conflicts with [2. Spot · Actor Execution Serialization](02-serialization.en.md)'s resource constraint. Combining both allocations exceeds core count |

**Decision — resources are allocated per process, and don't grow with
topology or [Spot](../spec/01-glossary.en.md#spot) count.**
Infrastructure work is mostly short and non-blocking, so it's covered
by fewer resources than application.

### Dedicated Resource Doesn't Mean A Physical Thread

**Decision — the contract is not "a dedicated thread" but
"infrastructure progresses whenever application is entirely waiting."**
This is because the four languages' execution models differ.

| Language | Execution resource | How dedication is satisfied |
|---|---|---|
| C++ | OS worker pool | Keeps a dedicated worker for infrastructure |
| .NET | Serial drain over a thread pool | Submits infrastructure work to a separate lane |
| Java | Virtual thread per task | Attaches the infrastructure lane to a separate executor |
| Node | **A single event loop** | Physical separation is impossible. Only the lane is separated |

Node can't physically build a dedicated resource since it has a single
event loop. So the contract is split as follows.

- **Guaranteed** — after an application handler yields with `await`,
  infrastructure work progresses. This holds even when every
  application work item is waiting for a result.
- **Not guaranteed** — progress while an application handler holds the
  CPU without yielding. This isn't a contract violation but the
  application's own responsibility. The guidance is to move a
  long-running synchronous computation to a worker.

`Task`, `Promise`, and virtual thread are all recognized as execution
resources under this contract. The criterion isn't the data type but
**whether it progresses after yielding.**

The observation standard for this decision isn't resource count but
§1's progress condition. Put every application work item into a
waiting state (yielded) at once and confirm infrastructure still
progresses.

## 5. How Far Backpressure Propagates Upward

If it's undecided how far the fact "sending is blocked" propagates,
some implementations wait and others fail immediately under the same
situation.

**Decision — these three steps apply only to the send · publish ·
one-way family.**

1. If the first submission is rejected, **wait for send space to open
   up until a fixed time.**
2. If space opens within the time, submit **once.**
3. If the time runs out first, end with `DeadlineExceeded`
   ([Async Execution Policy 「1.3 One-way submit」](../spec/05-async-execution-policy.en.md#13-one-way-submit)).

**The Request family doesn't wait.** If the same runtime's Spot · Actor
queue is full, it ends immediately with `CapacityExceeded`; if it's a
different node's queue, with `Unavailable`
([Spot Messaging 「5.3 Work Put On The Spot Application Queue」](../spec/12-spot-messaging.en.md#53-work-put-on-the-spot-application-queue)).
Request has no reason to wait since the caller can receive a result
and judge whether to retry. The send family, by contrast, has no
result to return, so the caller can't judge, and so it waits.

**These steps apply only to the span before the public result is
finalized.** A failure that happens after an already completed call
doesn't fall here — a skipped local target after publish has started,
a dropped one-way during a move, or a target admission failure of a
completed send are examples. These have no result to return to the
caller, so they're left only as observations.

While waiting, that work must not hold execution authority. Holding it
while waiting blocks another request to the same Spot for as long as
it waits for send space.

**Decision — the waiting slot itself also has a bound.** If the
waiting slots are full, it ends immediately with `DeadlineExceeded`
without waiting (`05:72-73`). The fact of being backpressured itself
isn't a value the caller receives — `Backpressured` isn't a public
terminal result (`05:70`). Without a bound, when the peer is slow,
this side's memory would keep growing indefinitely, following the
peer's processing speed.

The receive-side bound goes the other direction. When
pending-processing bytes hit the ceiling, **only new application
receiving stops**, while response receiving, runtime control
processing, and send-ready notification keep being processed
([Framework API 「2.1 Keeping Received Payload From Growing Memory Indefinitely」](../spec/06-framework-api.en.md#21-keeping-received-payload-from-growing-memory-indefinitely)).
Stopping receiving altogether would also block the responses mixed in
there, causing a deadlock.

However, a multiplexed receive path where the binding doesn't announce
the complete message length before `Recv` can't tell application from
control right before a raw `Recv`. This path lets `MaxMessageSize` be
`M` and the count of concurrent raw-receive reservations be `R`, and
allows a raw receive for classification purposes only within
`HWM + R * M`. Control isn't counted into application pending bytes
and its reservation is returned immediately, while application records
its payload bytes and keeps the reservation until a terminal state.
So the meaning of the HWM policy is **blocking unbounded admission of
new application work**, and only the classification span needed for
control progress stays bounded. A binding that can check the length in
advance can stop new application `Recv` directly at the HWM without
this classification span.

A StreamNode checks its complete client-to-server message separately on
the Core STREAM inbound path. The measured size is header bytes plus
payload bytes, excluding the 6-byte prefix, and the StreamNode default is
`64 KiB`. This cap isn't applied to a server-to-client outbound message.

**Decision — `R` isn't grown with load.** HWM is not an exact cutoff
line but **a signal that pressure has risen**, and stopping receiving
is the only way to feed that pressure back to the sender
([Framework API 「2.1 Keeping Received Payload From Growing Memory Indefinitely」](../spec/06-framework-api.en.md#21-keeping-received-payload-from-growing-memory-indefinitely)).
Scaling `R` with connection count or pending message count means **the
harder it's pushed, the more it swallows, weakening the signal exactly
when the pressure is needed.** `R` must be a fixed allowance decided
by configuration, and what this clause requires isn't exact
accounting but that the excess stays independent of load.

`R` is a different axis from the count/byte/elapsed-time bounds in
[7. Receive And Dispatch Loop 「6. Read Multiple Items At Once From A Socket」](07-dispatch-loop.en.md#6-read-multiple-items-at-once-from-a-socket).
Those three decide **how much to read from one connection per
wake-up**, while `R` decides **how many raw receives that haven't
finished classification can be outstanding at once.** They aren't
merged into the same value.

## 6. Don't Silently Drop On Exceeding A Bound

**Decision — work that can't be processed ends in a result the caller
can observe.** It's neither piled up indefinitely, nor silently
dropped, nor secretly resubmitted later
([Async Execution Policy 「1.3 One-way submit」](../spec/05-async-execution-policy.en.md#13-one-way-submit)).

There's an actual counter-example observed in one implementation. When
the slot holding a response temporarily is full, it **drops and
cleans up the response.** The waiting caller receives no notice at all
and hangs until the timeout fires. The cause is "not enough storage
slot," but what the caller sees is "no response" — nearly impossible
to diagnose.

A bound records its scope of application together. Even the same unit
is a different value if the purpose differs.

| Bound | Measured by | Scope |
|---|---|---|
| Execution queue | Two axes: count and bytes (includes fixed per-work cost, counts running work too) | One execution authority |
| Pending processing | Sum of payload bytes | One process's application receiving |
| Pending during a move | Both count and bytes | 1,024 items / 16 MiB per move |

Why the bound is measured in bytes rather than count is covered by
[8. Object Kind And Activation 「6. Which Unit Memory Accounting Uses」](08-object-lifecycle.en.md#6-which-unit-memory-accounting-uses).

The basis for the pending-during-a-move bound is
[Host Relocate And Shutdown 「9. Moving Pending Messages, Timers, And Sessions」](../spec/28-graceful-drain-handoff.en.md#9-moving-pending-messages-timers-and-sessions),
and the result of exceeding it is covered by
[5. Continuity During A Move](05-relocation-continuity.en.md).

## 7. The Implementation Constraint This Decision Produces

Splitting into two regions requires being able to **tell which region
is currently executing.** If infrastructure-only work is called from
an application context, or the reverse, the point of splitting them
disappears.

One implementation confirms this with an execution-context marker, and
**ends in failure without waiting** on a wrong combination. Treating
it as a wait would deadlock, and letting it pass would break the
separation, so failure is correct.

**Per-language discretion.** Whether to build the regions as two
queues or as a single priority, and how to mark the context, is free.
There's one observation standard — with an application handler
artificially kept waiting, do the call timeout, the shutdown
procedure, and peer connection handling still progress.

## 8. Result To Confirm

- With an application handler kept waiting, that call's timeout fires.
- With an application handler kept waiting, the shutdown procedure
  progresses.
- With an application handler kept waiting, a new peer connection is
  accepted.
- A slow status subscriber doesn't slow down message processing speed.
- When pending-processing bytes hit the ceiling, only application
  receiving stops while response receiving continues.
- Work that exceeds a bound within the span before the public result
  is finalized doesn't silently disappear, and ends in a result the
  caller observes.
- A failure after an already completed call (a skip after publish has
  started, a target failure of a completed send) doesn't change the
  caller's result and is left only as an observation.
- Calling infrastructure-only work from an application context fails
  without waiting.
- Infrastructure execution resources don't grow with topology count or
  Spot count.
- Work waiting for send space doesn't hold execution authority.
- When the send-wait slot is full, it fails without waiting.

---

[Internal structure table of contents](README.en.md) · [Previous: 2. Spot · Actor Execution Serialization](02-serialization.en.md) · [Next: 4. Operation Completion Confirmation](04-completion.en.md)
