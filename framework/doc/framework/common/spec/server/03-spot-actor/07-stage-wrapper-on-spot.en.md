---
title: "Stage Wrapper on Spot"
---

# Stage Wrapper on Spot

[Spot And Actor topic index](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 06. Spot Address Messaging](06-spot-address-messaging.en.md) · [Next: 08. Spot/Actor Routing](08-routing.en.md)

> Defines the execution boundary an application's domain wrapper must keep when building a
> higher-level execution model such as room/stage/zone on top of the framework's public
> [Spot](../00-foundation/02-glossary.en.md#spot) contract — a logical instance with an address
> and state, reachable by the same global Spot ID even when its executing node changes.

## 1. Stage Wrapper Overview

A Stage wrapper must safely maintain the state owned by a Spot while preserving the
Spot/Actor/timer execution boundary that matches the selected User Spot execution mode.

The framework doesn't provide a separate Stage runtime or a common Stage base type. The
application's domain wrapper composes the Spot's public registration/messaging/timer/
lifecycle surface. Each language's wrapper shape is determined by that language's
public interface document.

## 2. Responsibility Boundary

| Responsibility | Owner |
|---|---|
| Spot identity, create/close, and the application turn | Owned by the framework Spot runtime. |
| Spot direct and [Logical Multicast](../00-foundation/02-glossary.en.md#logical-multicast) (a method of delivering one message to multiple Spots of the same Channel by ChannelName and topic) dispatch | Owned by the framework Spot runtime. |
| Timer admission and callback turn | Owned by the framework Spot runtime. |
| Actor queue and Actor business handler | Owned by the framework Actor runtime. |
| Actor join/leave and lifecycle control | Owned by the Spot/Actor-dedicated queue the framework uses to process lifecycle work. |
| Admission rights, stage state, membership policy, and broadcast content | Owned by the Stage wrapper or application. |
| The policy mapping a domain key to a global Spot ID | Owned by the Stage wrapper and application domain store. |

**A Stage wrapper doesn't expose transport RID, endpoint, internal queue, native timer
handle, or message storage reference on the public surface.** This is because the
wrapper's public surface must consist only of the framework's public Spot/Actor/timer/
location surface.

## 3. Preserving the Spot Turn

A callback that reads or changes state owned by the Stage must run on the target Spot's
application turn — the [Spot turn](../00-foundation/02-glossary.en.md#spot-turn), the unit in
which one Spot callback holds the application queue's execution gate to run. The following
callbacks are subject to this rule.

- [Spot direct](../00-foundation/02-glossary.en.md#spot-direct) handler
- Logical Multicast subscription handler
- Spot timer callback
- Actor join/leave and lifecycle control callback
- A domain operation the Stage wrapper explicitly submitted to the Spot

A User Spot's execution mode is fixed when registering the factory.

- **In the default `SpotWide`, the work above and the member Actor handler use the same
  Spot gate.** This is so two callbacks don't change Spot state at the same time.
- **In `PerActor`, Spot direct/Logical Multicast/lifecycle control are serialized on the
  Spot lane, and each member Actor's payload is serialized on its own Actor lane.** The
  same timer's callback is serialized on that timer's lane. Different Actor lanes, the
  Spot lane, and different timer lanes can run concurrently, so the application is
  responsible for synchronizing shared Stage state.

The meaning of keeping or returning the turn while a callback waits on an async operation
is defined by the [Async Execution Policy](../01-execution/01-submit-and-completion.en.md). A wrapper
doesn't change that contract with a separate scheduler or lock rule.

**If a request-reply continuation changes Spot state, it must be resubmitted on the
original [Spot turn](../00-foundation/02-glossary.en.md#spot-turn).** Spot state isn't changed directly
on a transport or completion thread.

## 4. Actor Boundary

Even if an Actor joins a Spot serving as a Stage, the Actor's business payload is still delivered
directly to the Actor queue. Actor payload isn't converted into a Spot callback or put on
the Spot application queue. Consequently, an Actor handler doesn't directly reference the
Stage's mutable state.

For an Actor to change Stage state, it submits an explicit send/request to the Stage
Spot. That handler makes
[membership](../00-foundation/02-glossary.en.md#actor-membership) — which Stage Spot the Actor
currently belongs to — as well as score, world state, and broadcast decisions on the Spot turn.

- In `SpotWide`, the Actor handler also uses the shared Stage execution gate.
- In `PerActor`, the per-Actor gate and Spot lane can run independently, so the boundary
  where an Actor handler doesn't directly reference mutable Stage state is preserved.

The framework processes an Actor's join, leave, relocation, and lifecycle notification on
a dedicated queue separate from business messages. This queue doesn't run the Actor's
regular business handler, and doesn't convert business payload into a lifecycle callback
either. The detailed Actor queue and lifecycle handling contract is owned by
[Actor Model](04-actor-model.en.md).

## 5. Timer and Yield

A Stage timer is registered within the Spot lifecycle, and its tick is submitted to the Spot
application queue.

- In `SpotWide`, the timer callback is serialized with the entire Stage, including Spot
  direct, member Actors, and other timer callbacks.
- In `PerActor`, only the same timer's callback is serialized, and different timers,
  Actor lanes, and the Spot lane can run concurrently.
- **Spot shutdown closes admission of new timer ticks.** The order between
  already-accepted ticks and the shutdown callback is determined by the Spot lifecycle
  rule.
- Fixed-rate, delay, catch-up, and overrun options are expressed through each language's
  public timer contract.
- The wrapper doesn't expose a native handle or scheduler thread to the application.

`Yield` only returns the current Spot gate in a `SpotWide` User Spot and Instance Spot
callback. In a `SpotWide` Stage, it can be used while waiting for a Channel/Spot/Actor
request or a CPU/I/O worker result. The continuation resumes as a new turn on the same
execution gate. `Yield` can't be used in a `PerActor` Stage or Entry Spot callback.

**Even when a member Actor handler yields, it keeps the right to run the current Actor
queue head.** A different Actor, a Spot handler, and a timer can use the common Stage
gate, but the next job of the same Actor doesn't run until the continuation re-acquires
the gate and completes the current job. A request the same Actor sent to itself also
doesn't run ahead of the current queue head or re-enter inline.

A call that would await a request needing the same gate via `Async`, or await a
self-sent request, is rejected with `InvalidOperation` before submit.

Even after a host `Relocate` starts, a Stage Spot that hasn't yet obtained execution
rights to start relocation keeps processing its existing message and timer turns. The
internal notification the framework uses to announce relocation readiness isn't an
application event, so it doesn't run a Stage callback.

After execution rights are obtained and new turn admission is closed, timer ticks not yet
run and timer registration information are included in the relocation payload. Since the
target framework automatically restores these, the Stage wrapper's `Restore` doesn't
re-register the same timer.

## 6. Creation and Membership

The Stage wrapper passes a stable type and a domain creation payload to the User Spot
manager's explicit Create/GetOrCreate, and builds the initial Stage state inside the
creation callback. Even if multiple nodes try to create the same Spot concurrently, the
Framework only runs the one factory that obtained creation authority. The condition for
allowing new work and the business state restored after reactivation are decided by
domain rules.

Actor join checks the Stage membership policy on the framework's lifecycle-dedicated
queue. If the join succeeds, the Actor's current Spot location and the member state owned
by the Stage are updated together. The method for confirming a concurrent change as one,
and the message admission boundary during relocation, are owned by
[Spot And Actor Membership](05-spot-actor-membership.en.md).

A Stage-wide notification uses whichever path matches its meaning.

- Use Logical Multicast to notify multiple Spots of the same ChannelName.
- To notify based on one Stage's member state, pick the target Actor or bound session on
  the Spot turn and submit an explicit message.

**Logical Multicast isn't used as the durable source of the Stage member list.**

## 7. Location and Lifetime

An external service obtains a global [Spot ID](../00-foundation/02-glossary.en.md#spot-id) from a
domain key and sends a message to the Stage Spot. When closing that incarnation or
displaying it as operational information, use the `SpotRef` the manager lookup returned.
The owner RID and endpoint aren't stored in wrapper state. The meaning of location updates and
stale routes is defined by
[Spot Address Messaging](06-spot-address-messaging.en.md).

Stage shutdown closes new application admission and new joins, and finishes cleanup of
already-accepted Spot turns and membership within the drain deadline. Timers,
[subscriptions](../00-foundation/02-glossary.en.md#subscription), and direct messages after shutdown
don't create a new Stage callback.

## 8. Metadata and Observability

The Stage wrapper provides the immutable metadata snapshot of the
[Message Model](../00-foundation/05-message-model.en.md) to the handler as-is, and doesn't interpret
the transport frame or storage ownership.

Observability information must distinguish
[MeshName](../00-foundation/02-glossary.en.md#meshname) (the name that identifies one
[RouteMesh](../00-foundation/02-glossary.en.md#routemesh) physical connection group), Stage
type, Spot turn backlog, timer delay, membership control result, and shutdown state. Stage ID
and Actor ID aren't used as metric labels.

## 9. Implementation and Contract-Test Verification Requirements

Confirm the following using only the public surface — the framework's public
Spot/Actor/timer/location surface the Stage wrapper uses, the point at which `Yield`
returns, and the submit-time error. Each item maps to one contract test.

**Preserving The Spot Turn**

- Spot direct, Logical Multicast, timer, and explicit Stage operations preserve the same
  Spot turn.
- In `SpotWide`, the member Actor handler also uses the same Spot gate.
- In `PerActor`, the Spot lane, per-Actor lanes, and per-timer lanes can run concurrently
  with each other while each keeps its own FIFO serialization.
- A request continuation doesn't directly change Stage state on a transport thread.

**Yield And Gate**

- A `SpotWide` Actor's `Yield` only returns the Spot gate and keeps execution rights on
  the Actor queue head.
- `Async` and a self-awaited request needing the same gate are rejected with
  `InvalidOperation` before submit.

**Actor Boundary**

- Actor payload doesn't pass through a Stage Spot callback or the
  [Spot application queue](../00-foundation/02-glossary.en.md#spot-application-queue).
- The Actor handler uses an explicit Spot call to change Stage state.
- The framework's Spot-lifecycle-dedicated queue only contains join/leave and lifecycle
  control, and doesn't include Actor business payload.

**Surface Compliance And Shutdown**

- The Stage wrapper only uses the framework's public Spot/Actor/timer/location surface.
- No new timer or message callback runs after Spot shutdown.
- A Stage Spot isn't sealed before a relocation permit, and after seal, timer
  registration and pending ticks are automatically restored on the target.

---

[Spot And Actor topic index](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 06. Spot Address Messaging](06-spot-address-messaging.en.md) · [Next: 08. Spot/Actor Routing](08-routing.en.md)
