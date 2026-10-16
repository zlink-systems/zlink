---
title: "Spot Timer"
---

# Spot Timer

[Spot And Actor topic index](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 09. Object Lifecycle](09-object-lifecycle.en.md)

> This document defines tick information, cancellation, and overrun contracts for repeating and
> delayed timers registered by a [Spot](../00-foundation/02-glossary.en.md#spot).
> [Execution contract §2](../01-execution/02-handler-turn-and-execution-gate.en.md#execution-gate)
> owns callback execution authority, the [execution gate](../00-foundation/02-glossary.en.md#execution-gate),
> and turns; [§3](../01-execution/02-handler-turn-and-execution-gate.en.md#yield-gate-and-claim) owns Yield.

## 1. Timer Generation and Cancel

The service runtime converts platform-timer expiration into callback work. Mode-specific execution
follows the execution contract above; the timer-specific semantics below apply regardless of backend.

**Re-registering the same timer key increases the generation.** A record from an earlier
generation that's already on the queue does not run its callback — if a tick scheduled
before re-registration ran after re-registration, it would use a period and callback other
than the new ones the caller expects.

**Exception: when a relocation target's `Configure` or pre-Ready `OnInitializeAsync` registers a restored key, no new generation is created.** A relocation target recreates the Spot instance and runs these two callbacks ([Spot model §5.2](01-spot-model.en.md#52-user-spot-lifecycle)). A restored key is the key of a logical timer that was in the source relocation payload. When either callback registers that key, the registration only reattaches a handler to the restored registration, and the period, options, next fire time, pending tick, delivery and scheduled index, and the timer handle the registration returns keep their restored values. A different period or option in the registration arguments is not a failure; the restored values win. Invalid registration arguments still fail by the existing validation. If the handler type in the registration differs from the restored registration, only that registration call fails with `InvalidOperation`, and the restored registration and handler remain. What follows depends on when the registering callback runs. `Configure` runs in the target restore step before the authority commit ([relocation §4.3](../05-location-relocation/04-relocation-flow.en.md#43-restore-the-target-without-running-it)), so if the exception propagates out of `Configure`, the target restore fails and the relocation-failure result of [host relocation §13](../05-location-relocation/05-host-relocation-flow.en.md#13-relocate-completion-and-failure) applies. `OnInitializeAsync` runs in the lifecycle step after the CAS succeeds ([relocation §4.6](../05-location-relocation/04-relocation-flow.en.md#46-target-opens-the-queue-progressively-starting-with-existing-work)), so a registration failure there doesn't change the settled relocation result or return to the source. This restored-key exception applies only to relocations that move logical timers. A `PerActor` relocation, which doesn't move Spot-level timers ([host relocation §12](../05-location-relocation/05-host-relocation-flow.en.md#12-moving-pending-messages-timers-and-sessions)), has no restored key. Treating it as a new generation would drop the pending tick and next fire time that [host relocation §12](../05-location-relocation/05-host-relocation-flow.en.md#12-moving-pending-messages-timers-and-sessions) requires to move, just because application code registers its timers during initialization. Registering or re-registering a key that wasn't restored, a registration from any other callback (such as the relocation completion callback), and any re-registration after Ready follow the generation rule above.

**No timer runs a tick before its Spot is Ready.** This holds for a newly created User Spot, an Instance Spot cold activation and a relocation target alike. A timer registered by `Configure`, `OnCreateAsync` or `OnInitializeAsync` gets its generation and next fire time at registration, but a tick that expires before the Spot becomes [Ready](../00-foundation/02-glossary.en.md#ready) doesn't start its callback; it stays pending under the [§2](#2-overrun-policy) overrun policy. Once the Spot is Ready, pending ticks run in the same lane order as the Spot's other work. If creation is rejected or fails, or the activation is discarded, before Ready, that incarnation's timers are cleaned up without ever running a tick. Message handlers don't run before Ready either ([Ready](../00-foundation/02-glossary.en.md#ready) is the state in which the Spot can receive application messages), so if timers alone ran early, an incarnation not yet committed to the Location Store would start application behavior (sends, Close and so on) — if creation is then rejected, that incarnation never existed, and a Close fails because it tries to close an incarnation that isn't the current authority yet.

**`cancel` blocks new callbacks for that timer generation and completes with the generation's
single result after any callback already running and all timer-owned resource cleanup have
finished.** A callback already started is not forcibly interrupted — forcibly terminating a
handler running at the moment of cancel could leave the state that handler was working on
inconsistent. Calling `cancel` several times for the same generation gives every caller the same
success or failure. One completion owner and one terminal result per generation keep the
observation from depending on which caller went first. Cleanup failures are not swallowed; they
arrive in that result and are classified by the
[error model](../00-foundation/07-framework-error-model.en.md).

**Even if a repeating timer expires faster than the handler runs, the same key's callback
never runs concurrently.** Duplicate expirations may be collapsed into a single pending
record — allowing concurrent execution would let two callbacks of the same timer change the
same state at the same time.

| Situation | Behavior |
|---|---|
| Re-registering the same key | Generation increases (a restored key during relocation target initialization follows the exception above) |
| Expiry before the Spot is Ready | Callback doesn't start; pending under the overrun policy; runs after Ready, or is cleaned up without running if creation fails or the activation is discarded before Ready |
| A queue record from an earlier generation | Does not run its callback |
| Cancel | Blocks new callbacks for that generation and completes every cancel caller with the same success or failure after an already-running callback and timer-owned resource cleanup finish |
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

Callback gate selection, acquisition, and release follow
[execution contract §2–§3](../01-execution/02-handler-turn-and-execution-gate.en.md#execution-gate).

- **If the timer cannot acquire execution authority, it retains the tick and retries at the next
  execution opportunity.** This timer-specific retention rule prevents losing an unexecuted tick.

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
  not run its callback. A restored key during relocation target initialization follows the §1 exception.
- A short-period timer registered in a new User Spot's `OnInitializeAsync` doesn't start its callback
  before the Ready commit finishes, and runs first after Ready. A context `Close` called from that callback
  ends `true`. The same holds for an Instance Spot cold activation. For a creation that `OnCreateAsync`
  rejects or whose `OnInitializeAsync` fails, the registered timers never run a callback.
- A context `Close` requested by an Instance Spot cold activation's `OnInitializeAsync` starts only after the first
  business message handler completes and the replay-cursor update and recovery-pointer release finish
  ([Spot address messaging §7](06-spot-address-messaging.en.md#7-close-and-generation-boundary)).
- After cancel is called, a tick from that generation onward doesn't start its callback, but a callback
  already running at the moment of cancel runs to completion.
- When `cancel` is called concurrently more than once for the same generation, every result
  completes after the callback running at that time and timer-owned resource cleanup finish, and
  every caller sees the same failure when cleanup fails.
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
