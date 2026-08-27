---
title: "Application Job Queue And Backpressure"
---

# Application Job Queue And Backpressure

[Execution topic table of contents](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 03. Cancellation And Shutdown](03-cancellation-and-shutdown.en.md) · [Next: 05. Payload Ownership And Codec](05-payload-ownership-and-codec.en.md)

> This document defines the order in which two independent capacity authorities — Core's
> [Core byte HWM budget](../00-foundation/02-glossary.en.md#core-hwm-budget) and Framework's
> [Application job queue](../00-foundation/02-glossary.en.md#application-job-queue) — pass ordinary
> ingress, and when and how Framework observes and propagates the resulting pressure
> state. It states the responsibility boundary among Application, Core, Framework host,
> and provider as a contract that callers depend on, and states, alongside that contract,
> the implementation structure that every language runtime must satisfy in common.

## 1. Two Independent Capacity Authorities

Core and Framework observe different overloads.

- Core knows the frame bytes currently held by transport queues and limits queue-memory
  bursts caused by connection count and payload size.
- Framework knows how many application jobs are waiting before handler start and limits
  ingress that exceeds application processing capacity.

- **Core byte HWM and the Application job queue do not share settings, profiles, units,
  accounting boundaries, or observations.** Copying one authority's values into the other
  authority's counter creates overlapping responsibility — Core must not infer handler
  processing capacity, and Framework must not calculate socket-queue bytes.
- **Core byte HWM is the transport path's last safety boundary, and the Application job
  queue is the safety boundary for handler ingress speed — both are required, but they
  are not the same protection.** Even when Framework pressure reduces remote traffic
  early, data can already be present in the remote Core queue, OS buffers, the network,
  and the local Core queue. Conversely, a Core queue can be empty while a handler job
  waits for a long time.

| Authority | What it limits | Accounting or acquisition | Release |
|---|---|---|---|
| Core byte HWM | Physical-frame charge currently owned by a Core application-direction queue | When the Core queue owns a frame | When the Core queue gives up frame ownership, including receive dequeue |
| Application job queue | Application job permits accepted by a host before callback start | Reserve immediately before receiving or claiming ordinary ingress, then transfer to a handler turn | Immediately before the callback's actual first instruction, or at a pre-callback terminal |

- **Core byte HWM charge includes the payload and metadata bytes defined by the
  contract, and once the Core queue hands a record to the binding, that record's charge
  ends.** Framework does not request a retained-credit lease or extend Core byte charge
  through handler or reply lifetime.
- Post-receive payload storage follows the ordinary message-ownership rules in [Payload
  Ownership And Codec](05-payload-ownership-and-codec.en.md). Copying, moving, and
  releasing that storage is payload lifetime management; it is not Core byte HWM credit,
  a second Application job queue permit, or a separate byte-pressure authority.

Framework pressure count (permits in use) is the following value:

```text
application job permits in use
  = reserved supply permits
  + queued application jobs
```

A capacity waiter has not received a permit and is not included. Converting a
reservation to a queued job does not change the total; only returning the permit
decreases it. A record that creates 1:N callbacks uses one permit for each actual
callback turn.

## 2. Configuration And Profile Boundary

Core and Framework profiles may use the same labels, but they are different public
types with different calculations.

| Setting group | Owner | Default profile | Manual override |
|---|---|---|---|
| `CoreHwmMemoryLimitBytes`, `CoreHwmBudgetBytes`, `CoreHwmProfile` | Core | `Balanced` | Core memory or budget bytes |
| `ApplicationJobQueueProfile`, `MaxQueuedApplicationJobs` | Framework host | `Balanced` | A precise host job-permit limit |

- **Framework only passes Core configuration values to binding context options at
  startup; it does not calculate profile ratios or divide a budget by connection
  count.** Projecting a Core snapshot into Framework status is also read-only
  observation and is not an input to Framework pressure calculation.
- **The only feedback that Framework job pressure gives Core is one absolute
  `RUNNING`/`PAUSED` receive-flow state applied to supported sockets.** Framework does
  not change Core HWM settings or queued-byte counters to match this state transition.

## 3. Ordinary Ingress Permit Order

Ordinary ingress — every path that receives or claims from Core or the binding a record
that has not yet received a permit — follows the same rule regardless of the context in
which it arrives. STREAM application packets, cross-node Session application records
(see [STREAM Server Session](../04-session/01-stream-session.en.md), [Session And Actor
Binding](../04-session/02-session-actor-binding.en.md)), handshake/bind/unbind, and every
other ordinary application job ingress follow this order without exception.

Ordinary ingress follows this order.

1. Wait for a host-shared permit in oldest-live-source order.
2. Receive or claim a record from Core or the binding only after acquiring the permit.
3. Transfer an application record's permit to its handler turn in an owner mailbox or
   serial queue.
4. Return the reservation after finite internal handling of a control or malformed
   ordinary record.
5. Return the permit at the common invocation boundary immediately before the
   callback's actual first instruction.

```mermaid
sequenceDiagram
    participant S as Socket / connection
    participant P as Host-shared permit
    participant Q as Owner queue
    participant H as Handler callback

    S->>P: Request permit in oldest-waiting-source order
    alt permit acquired
        P-->>S: permit granted
        S->>S: receive/claim record
        alt identified pre-receive as terminal reply/error completion
            S->>H: bypass permit, process immediately, release right after internal handling
        else application record
            S->>Q: transfer permit to owner queue's handler turn
            Q->>H: assign handler turn
            H-->>P: release permit immediately before actual first instruction
        else control/malformed record
            S-->>P: release permit after finite internal handling
        end
    else cancel/close/shutdown while waiting
        P-->>S: cancellable wait ends (not a reject/drop)
    end
```

An `await`, coroutine suspension, continuation, or reply wait after handler start does
not acquire the same permit again. A non-runnable durable backlog such as relocation
returns its initial reservation after a finite handoff to the payload owner defined by
that relocation specification; each runnable callback turn later acquires a new permit.

**Only supply identified before receive as a terminal reply or error completion
bypasses this permit.** A record first received from an ordinary connection cannot be
classified later and retroactively gain this bypass. This separation lets terminal
completions of operations already in progress continue while the ordinary queue is
saturated.

Each source has one outstanding permit waiter, handed off in oldest-waiter
order. A source that processed a batch moves to the tail of the queue.

Neither a
batch nor a 1:N callback publishes more application jobs than the permits it secured.

Framework heartbeat, topology, relocation, and service-wire `SendReady` kind `12` are
not this completion supply. Those control records remain on the application data line
under its existing FIFO and liveness contract; they are not moved to the Core
completion connection or to a separate Framework control queue.

The following are not allowed:

- Receiving first without a permit and only afterward incrementing a separate counter
- Reimplementing Core HWM with a retained-credit lease or a Framework byte HWM
- Replacing saturation with rejection, dropping, fixed-delay polling, or busy spinning
- Storing a record in an unbounded or hidden side backlog not owned by a specification

### The Set Of Ready Owners (Implementation)

For a record that has acquired a permit to enter an owner queue, the execution resource
must know that this owner currently has work to do.

- **Keep as state which owners currently have work to do.** This state represents
  "this is the current state," not a one-time "something changed" notification, and the
  same owner never enters it twice. Because an owner with remaining work must
  eventually be processed even if a notification is lost, a woken execution resource
  always rechecks this state.

Internal confirmation condition — that a woken execution resource always rechecks the
set of ready owners, so that no wakeup is missed even when a notification is lost, is a
white-box invariant of this state management.

### Deciding Whether To Admit And Admitting Are Not Split Apart (Implementation)

Deciding which owner's queue to put a message into requires checking several
conditions — whether that owner is still on this node, whether there is a slot, and
whether it is sealed for a move.

- **These checks and the actual enqueue finish inside the same span.** If the owner
  changes between the check and the enqueue, the message ends up in the queue of a node
  that is no longer the owner, and that queue is processed by no one — the sender waits
  until timeout.

Handle the following as one commit inside that span.

1. Is the host and topology currently accepting application work
2. Is the target object on this node and is the owner information valid
3. Is it not sealed for a move, not waiting for creation, and not waiting for a session
   connection
4. Can both the lane's item count and bytes be reserved together
5. Commit the accepted-order sequence and append the message to the owner queue
6. If the queue was empty, put that owner into the set of ready owners and notify the
   execution resource immediately

- **A message that fails a check does not appear in the queue.** It is not built as
  enqueue-then-remove — enqueuing and then removing lets it possibly execute in
  between, and the removal cannot be distinguished from observation either. A call
  waiting for a response receives the failure reason as its result. A failed
  reservation or enqueue also leaves the item/byte usage and accepted sequence
  unchanged. A failed attempt does not change the ordering or admission result of the
  next valid work item.

**Per-language discretion.** Whether this span is built as a lock or another method is
free. The check is that only the check and the enqueue are inside this span, and that
work that would lengthen the span — such as deserialization or handler lookup — happens
outside it. As long as that condition holds, the observable result — that the span
stays short and the owner does not change between the check and the enqueue — is the
same no matter which implementation method is used.

Internal confirmation condition — that a message whose owner changed between the check
moment and the enqueue moment does not go into the old owner's queue is a white-box
invariant confirmed only by the fact that the above commit procedure is a single atomic
span.

### Permit Return And No Resource Holding While Waiting (Implementation)

An application permit releases at the actual first instruction of its own
target's callback; a control or malformed record's permit releases immediately after
internal handling. This release point is the same point at which a
[STREAM session](../00-foundation/02-glossary.en.md#stream-session) — the server-side
execution unit kept alive from accepting one STREAM connection until it closes —
callback starts — no separate rule exists per context. Cancellation, source close, and
shutdown clean up waiters and handed-off permits exactly once.

- **Same-host relay, fanout, serial-owner, and relocation paths must not wait for a new
  permit acquire of the same authority while holding a gate, execution authority, or
  resource needed to return a permit.** A sustained wait/capacity cycle is not grounds
  for a bypass — it is a protocol or runtime bug.

## 4. Reading Multiple Items From The Socket (Implementation)

Separately from batch-processing after taking ownership, the same problem exists at
**the step of pulling from the socket.** If only one item is read from the socket per
wake-up before returning, waking and reading repeat as many times as messages piled up,
and cost grows as load rises.

- **On each wake-up, read multiple items in a row within a bound.** Reading
  indefinitely while the peer keeps sending would let one connection monopolize the
  receive stage, delaying other connections and binding-operation completion
  processing.
- **The bound sets count, bytes, and elapsed time together, and applies whichever is
  hit first.** Count alone makes large messages take too long, and time alone reads the
  clock too often for small messages.
- **The next rotation starts right after the connection this one stopped at (keeping a
  cursor).** Always iterating from the start means earlier connections keep being
  processed first, and later connections get delayed even with a bound in place.

This rule applies to **every multi-connection receive path**, including fanout,
[RouteMesh](../00-foundation/02-glossary.en.md#routemesh), ClientServer, service connections, and
STREAM. If there is leftover work when the bound is hit, it continues reading on the
next wake-up.

**The count bound is fixed at 64 items per rotation.** The byte bound and the elapsed-
time bound are **per-language discretion** — even with different values, the observable
result is the same: the rotation start point always resumes right after the connection
this one stopped at, and no connection monopolizes the receive stage indefinitely, so
other connections get a chance to progress. The check is whether one connection with a
slow consumer still leaves other connections' progress unblocked.

Wiring the directional socket options of RouteMesh's
[MeshNode](../00-foundation/02-glossary.en.md#meshnode) — the runtime node that
participates in a RouteMesh to send or receive messages — `SendHighWaterMark`,
`ReceiveHighWaterMark`, `SendTimeout`, `ReceiveTimeout` — is not covered by this
document. The channel-transport topic owns the public configuration defined by
[RouteMesh Topology](../02-channel-transport/01-channel-topology.en.md) and [MeshNode
Startup](../03-spot-actor/03-mesh-node.en.md).

## 5. Separating Receipt Handling From State Change (Implementation)

- **The receive callback moves ownership of the received data to a runtime-side value
  and returns immediately.** The receive context is usually owned by the transport
  layer, so lingering here delays other receives on that connection.
- **Format validation finishes before calling the handler.** Malformed input does not
  reach the handler — a call waiting for a response ends in `ProtocolError`, and a call
  not waiting ends with only a record left.

Internal confirmation condition — that the receive callback does not call the handler
or change the state of a [Spot](../00-foundation/02-glossary.en.md#spot) — a logical
instance with an address and state that can receive messages — is a white-box invariant
so that the receive path does not create
a path that changes state without going through the [handler execution
gate](02-handler-turn-and-execution-gate.en.md#1-separating-queue-from-gate).

## 6. Pressure State And Socket Control

For effective maximum `M` and configured pause and resume percentages `P` and `R`,
startup computes:

```text
pause permit count  = ceil(M * P / 100)
resume permit count = floor(M * R / 100)
```

`P` is in `1..100` and defaults to `80`; `R` is in `0..99`, defaults to `60`, and must
satisfy `R < P`. In `running`, the state transitions to `paused` when permits in use
reach the pause count, and in `paused`, it transitions to `running` when they fall to
the resume count. Between the two thresholds, the current state is retained.

- **Framework applies the transitioned absolute state only to paired DEALER/ROUTER
  sockets used by RouteMesh and ClientServer.** PUB/SUB, Classic fanout, and STREAM are
  outside this integration and retain their existing Core byte HWM and structural queue
  limits.
- **`PAUSED` does not change a Core HWM value.** Core independently composes a
  remote-pause blocker and a local byte-HWM blocker. `RUNNING` removes only the
  remote-pause reason, so a send remains waiting while local HWM is still full.
  Pressure state itself does not change route readiness or transport liveness.
- **The host queue owner computes pressure state at a synchronization boundary such as
  a permit-count change.**
  - When the state changes, it applies the new absolute state to
    a snapshot of supported sockets.
  - It does not repeat an already-applied identical
    state, and a stale transition cannot overwrite the latest state.
  - Shutdown does not
    wait indefinitely for a final state application, and resetting observation counters
    preserves the current state and pause duration.
- **This receive-flow state API is the only runtime control point between Framework
  pressure and Core send flow.** Framework does not create raw flow frames or use the
  Core control lane as a general Framework channel.

Internal confirmation condition — the order in which a new socket applies the current
host pressure state before publication in the receive-target registry, and close
removes the socket from the registry first before proceeding, is a white-box invariant
of the registry implementation. Binding calls run outside queue, registry, and
user-callback locks. Configuration failures other than lifecycle results racing with
close are recorded in diagnostics and metrics.

## 7. Composition With Send Completion

- **Core and the binding own HWM waiting, internal retries, and per-operation
  completion.** Framework selects a specific target and starts one binding operation.
  After the operation starts, Framework does not select another target or create a
  second operation for the same payload because of PAUSE or HWM.
- **Framework has no removed `send_ready` callback or event, readiness waiter, or retry
  adapter.** [Deadline](../00-foundation/02-glossary.en.md#deadline) — the final time
  point by which work must finish — cancellation, detach, and shutdown follow the existing
  first-terminal rule of the operation state machine. Framework service-wire
  `SendReady` kind `12` is a Framework service-control record and is a different
  contract from the removed binding callback.

## 8. The Three Backpressure Stages And Kinds Of Limits

- **These three stages of [Backpressure](../00-foundation/02-glossary.en.md#backpressure) —
  flow control that limits send rate via an upper bound on the send queue — apply only to
  the send/publish/one-way family.** The request
  family does not wait — a caller can receive a result and judge whether to retry, so
  if the same runtime's Spot/Actor queue is full, it ends immediately with
  `CapacityExceeded`, and if a different node's queue is full, with `Unavailable`.

1. If the first submission is rejected, wait for send space to open up until a fixed
   time.
2. If space opens within the time, submit once.
3. If the time runs out first, end with
   [`DeadlineExceeded`](../00-foundation/02-glossary.en.md#deadlineexceeded).

- **These three stages apply only to the span before the public result is finalized.**
  A failure that happens after an already completed call is not covered here — a
  skipped local target after publish has started, a dropped one-way during a move, or a
  target admission failure of a completed send — none has a result to return to the
  caller, so it is left only as an observation.
- **While waiting, that work does not hold execution authority.** Holding it while
  waiting blocks another request to the same Spot for as long as it waits for send
  space.
- **The waiting slot itself also has a bound.** If the waiting slots are full, it ends
  immediately with `DeadlineExceeded` without waiting. The fact of being backpressured
  itself is not a value the caller receives —
  [`Backpressured`](../00-foundation/02-glossary.en.md#backpressured) is not a public terminal
  result. Without a bound, this side's memory would keep growing indefinitely,
  following the peer's processing speed, when the peer is slow.

StreamNode's client-to-server complete-message
[`MaxMessageSize`](../00-foundation/02-glossary.en.md#max-message-size) is an independent
wire guard from this capacity. It checks header plus payload excluding the 6-byte prefix,
defaults to `64 KiB`, and does not apply to server-to-client outbound.

**Terminal meaning is distinguished by kind of limit and is not silently dropped.**

| Limit | What it measures | Meaning of saturation |
|---|---|---|
| Core HWM | Directional queued/accounted bytes | Backpressure from Core queue to sender |
| Application job queue | Host-instance reserved/queued/in-use permits | Cancellable shared-cap wait |
| [Owner](../00-foundation/02-glossary.en.md#owner) FIFO — the per-MeshNode queue for the node that currently executes an Actor or Spot | Per-owner count and bytes | Structural owner-isolation error |
| Outbound admission waiter | Bounded waiter per operation family | Original send deadline/cancellation result |

No path creates a separate unbounded backlog, polling, busy-spin, or silent replay.

## 9. Large Payloads And Operational Values

- **The Application job queue limits job count; it does not weight jobs by payload
  bytes.** An empty payload and a large payload each consume one job. The Framework
  queue limit is therefore not a process-memory byte hard cap.
- For workloads that retain large payloads for a long time, measure production-
  equivalent payload distribution, permits in use, process memory, throughput, and
  latency together, then lower `MaxQueuedApplicationJobs`. Limit an individual message
  size separately with `MaxMessageSize`. Do not connect the Core profile to the
  Framework profile or restore retained-credit leases to solve this problem.
- **Core HWM remains the final safety boundary for Core queue memory.** When Framework
  stops ordinary receive because of a permit, bytes accumulate in the local Core
  receive queue, and finite Core HWM plus TCP backpressure limits the sender's
  progress.

## 10. Verification Requirements

The public surface alone — send/publish/request result values, [Application job
queue](../00-foundation/02-glossary.en.md#application-job-queue) pressure-state queries, the socket
receive-flow absolute state, and [Runtime metric](../06-observability/02-runtime-metrics.en.md)
names — confirms the following. Each item leads to one contract test.

**Configuration of the two capacity authorities**

- The Core profile and the Application job queue profile can be configured differently,
  and each defaults to `Balanced`.
- The reservation, queued-job, and callback-first-instruction permit counts follow the
  same rule.

**Permit acquisition and order**

- Without a permit, the next ordinary record is not received first.
- A send/request that failed a check does not change the owner queue's observed
  count/byte/sequence values.
- When all shared permits are reserved, ordinary ingress waits cancellably, and terminal
  reply/error completion continues to progress.
- While one connection keeps sending, another connection's receiving still progresses.
- The receive bound cuts off at whichever of count/bytes/elapsed time is hit first, and
  the next receive rotation starts right after the connection this one stopped at.
- When one socket represents multiple peers, accounting is done per peer.
- Malformed input does not reach the handler — a call waiting for a response ends in
  `ProtocolError`, and a call not waiting ends with only a record left.

**Pressure state and sockets**

- The 80% pause, 60% resume, and hysteresis between the thresholds are precise.
- New-socket synchronization, close races, and stale transitions do not break the
  latest absolute state.
- Receive-flow state is applied only to supported paired sockets (RouteMesh/
  ClientServer).

**Backpressure and Core HWM**

- Work waiting for send space does not hold execution authority.
- When the send-wait slot is full, it ends immediately with `DeadlineExceeded` without
  waiting.
- Owner structural rejection and shared-cap wait are observed as distinct
  errors/metrics.
- A failure after an already completed call (a skip after publish has started, a
  target failure of a completed send) does not change the caller's result and is left
  only as an observation.
- When the Core receive byte HWM fills, backpressure is carried to the sender and a
  record is not dropped.
- Completion supply progresses independently of ordinary permit saturation.
- Framework does not use retained receive, a `send_ready` waiter, or a separate send
  retry.

The specific configuration values are defined by [Framework API](../00-foundation/06-framework-api.en.md);
status and metric names by [Runtime Status](../06-observability/01-runtime-monitoring.en.md) and
[Runtime Metric](../06-observability/02-runtime-metrics.en.md).

---

[Execution topic table of contents](README.en.md) · [Spec table of contents](../README.en.md) · [Previous: 03. Cancellation And Shutdown](03-cancellation-and-shutdown.en.md) · [Next: 05. Payload Ownership And Codec](05-payload-ownership-and-codec.en.md)
