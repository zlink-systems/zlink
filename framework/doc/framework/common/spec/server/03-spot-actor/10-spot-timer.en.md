---
title: "Spot Timer"
---

# Spot Timer

[Spot And Actor topic index](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 09. Object Lifecycle](09-object-lifecycle.en.md)

> This document defines the contract for timers that a [Spot](../00-foundation/02-glossary.en.md#spot)
> registers for repeating or deferred execution, and the implementation that keeps resources from
> growing in proportion to the number of registered timers. A timer callback runs through the
> same [execution gate](../01-execution/02-handler-turn-and-execution-gate.en.md) as the same Spot's other
> [application work](../00-foundation/02-glossary.en.md#spot-turn), so this document defines when a callback
> runs and what it receives when it falls behind, and points to
> [02. Handler Turn And Execution Gate](../01-execution/02-handler-turn-and-execution-gate.en.md) for the rules
> of the gate itself.

## 1. Timer Generation and Cancel

A Spot timer runs its callback on the same Spot application turn as network records. Each
language's service runtime turns a platform timer's expiration into a Spot queue record,
preserving the following semantics regardless of the backend.

**Re-registering the same timer key increases the generation.** A record from an earlier
generation that's already on the queue does not run its callback — if a tick scheduled
before re-registration ran after re-registration, it would use a period and callback other
than the new ones the caller expects.

**Cancel only blocks the start of callbacks from that generation onward.** A callback already
started is not forcibly interrupted — forcibly terminating a handler already running at the
moment of cancel could leave the state that handler was working on inconsistent.

**Even if a repeating timer expires faster than the handler runs, the same key's callback
never runs concurrently.** Duplicate expirations may be collapsed into a single pending
record — allowing concurrent execution would let two callbacks of the same timer change the
same state at the same time.

| Situation | Behavior |
|---|---|
| Re-registering the same key | Generation increases |
| A queue record from an earlier generation | Does not run its callback |
| Cancel | Blocks callbacks from that generation onward from starting (an already-started callback is not interrupted) |
| A repeating timer expires faster than the handler | The same key's callback is never run concurrently; duplicate expirations may merge into one pending record |

The callback receives the following tick information.

```text
DeliveryIndex  // A contiguous, one-based number for callbacks actually started in this timer generation
ScheduledIndex // The nominal tick represented by this callback, where the first nominal due time is 1
SkippedTicks   // The number of nominal ticks between the previously delivered ScheduledIndex and this value for which no callback was built
```

`DeliveryIndex` increases by exactly one per callback. `ScheduledIndex` never decreases and
maintains `ScheduledIndex >= DeliveryIndex`. `SkippedTicks` is `ScheduledIndex - 1` on the
first callback; afterward it is `current ScheduledIndex - previous ScheduledIndex - 1`. The
scheduler's wall-clock error and nanosecond-precision timing are not public results — these three
fields alone are the timing contract the caller observes.

## 2. Overrun Policy

A repeating timer uses one of three overrun policies to decide how to handle a passed tick
when the handler doesn't finish before the next tick. The default is `SkipLateTicks`.

| Policy | Next callback when the handler finishes late |
|---|---|
| `SkipLateTicks`(default) | Skips nominal ticks that have already passed and delivers only the single latest due tick at observation time. |
| `CatchUpBounded` | Delivers elapsed nominal ticks in order, but at most `MaxCatchUpTicks` in one catch-up interval; older ticks are skipped. |
| `DelayNextTick` | Recomputes the period after the handler terminal and schedules the next tick, without catching up missed ticks. |

`MaxCatchUpTicks` defaults to `1`. Under `CatchUpBounded` it must be in the range `1..INT_MAX`;
under the other policies this value does not affect behavior. Relocation encoding may
normalize an ignored value to a valid default instead of treating its original value as
publicly meaningful.

## 3. Owner Lease and Admission

A Spot timer can only be admitted after the service runtime checks the current
[owner lease](../00-foundation/02-glossary.en.md#owner-lease) and the admission deadline.

If lease renewal stops and the monotonic deadline is exceeded, no new tick is queued and no callback is started
after resuming, even if the Framework process had been suspended.

Pending ticks from a previous object/owner authority are not run either — if a timer kept running outside the
owner lease, the old owner's callback could touch the state of an object that has already
passed to a different owner.

## 4. Shared Scheduler — Resources Do Not Scale with Registration Count

Because each Spot can register several timers, timer count grows faster than Spot count. Ten
thousand Spots with two timers each produce 20,000 timers.

**Timers are managed by one shared scheduler, and no dedicated resource is created per
registration.** Creating a resource for every registration and managing everything with one
scheduler require different numbers of resources.

| Approach | Resources needed for 10,000 Spots × 2 timers |
|---|---|
| A dedicated resource per registration (OS timer, wait loop, deferred call) | **20,000** such resources |
| A shared scheduler + a deadline-priority queue | One thread and 20,000 queue entries |

The shared-scheduler approach is the common structure across every runtime. The number of
queue entries is the same, but only one scheduler and one core thread are needed. The scheduler
manages every Spot's timer in a deadline-priority queue.

*Internal verification condition — that scheduler resources (dedicated threads, OS timer
count) don't grow in proportion to the number of registered timers can be confirmed only by
an internal resource count. It cannot be observed from the public surface of timer
registration and callback execution alone.*

## 5. Internal Implementation of Late-Tick Handling

Section 2 defines the public option for handling a tick that has passed because execution ran
beyond the period. The implementation doesn't select one of the three policies as fixed — in particular,
`DelayNextTick` is just one of the three (a fixed delay), meaning "the next schedule happens
after processing completes," not a rule that eliminates the fixed period.

**The default behavior (`SkipLateTicks`) is to coalesce backed-up ticks into one.** The
 scheduler directly implements the §1 rule that "duplicate expirations may be collapsed into one
pending record." If the application chose `CatchUpBounded`, the ceiling is the count that
option defines, and the implementation doesn't arbitrarily reduce that count to one.

**Tick statistics are not accumulated indefinitely for a timer's lifetime.** If delivered-tick
and failure records keep accumulating, memory usage for a long-running timer keeps growing.

*Internal verification condition — that a long-running timer doesn't keep growing memory via
tick statistics is confirmed only by an internal memory usage measurement. It cannot be
observed from the public surface (the tick information fields) alone.*

## 6. The Path by Which a Tick Enters Execution Authority

A timer callback runs through that Spot's execution authority. `SpotWide` uses the shared
authority; `PerActor` uses a separate authority for each timer name. If a timer can't acquire
its own authority, that tick stays in the holding slot and is retried later. The rules for
acquiring and releasing execution authority itself are owned by
[02. Handler Turn And Execution Gate](../01-execution/02-handler-turn-and-execution-gate.en.md).

## 7. Batch Processing of High-Frequency Timers

Even a high-frequency timer does not make a round trip across the native callback boundary in a managed
language on every tick. When the platform timer sends a wakeup signal to the Framework
scheduler, the scheduler processes the expired records as a batch.

## 8. Verification Requirements

The following is confirmed using only the public surface (timer registration, re-registration,
and cancel calls; `DeliveryIndex`, `ScheduledIndex`, and `SkippedTicks` delivered to the
callback; overrun policy configuration and the resulting number of ticks delivered). Each item
maps to one contract test.

**Generation and cancel**

- Re-registering the same key means an already-pending tick from an earlier generation does
  not run its callback.
- After cancel is called, a tick from that generation onward doesn't start its callback, but a callback
  already running at the moment of cancel runs to completion.
- Even if the repeating period is shorter than the handler's execution time, the same key's
  callback is never run twice concurrently.

**Tick information**

- `DeliveryIndex` increases by exactly one per callback.
- `ScheduledIndex` never decreases and is always at least `DeliveryIndex`.
- `SkippedTicks` equals `ScheduledIndex - 1` on the first callback, and afterward equals
  `current ScheduledIndex - previous ScheduledIndex - 1`.

**Overrun policy**

- When a timer registered with `SkipLateTicks` falls behind, the next callback skips the
  backed-up ticks and receives only the single latest tick (`SkippedTicks` reflects the number
  skipped).
- When a timer registered with `CatchUpBounded` falls behind, it receives up to
  `MaxCatchUpTicks` ticks in order, and anything beyond that is skipped.
- A timer registered with `DelayNextTick` has its next tick period recomputed based on the
  moment the handler terminates.

**Owner lease**

- After the owner lease expires, a new tick doesn't start its callback, and while the lease
  isn't renewed, backed-up ticks don't all run at once even if the process was suspended and
  then resumed.

---

[Spot And Actor topic index](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 09. Object Lifecycle](09-object-lifecycle.en.md)
