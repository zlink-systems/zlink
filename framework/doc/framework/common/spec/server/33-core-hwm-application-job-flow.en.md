---
title: "Core Byte HWM And Application Job Flow"
---

# Core Byte HWM And Application Job Flow

[Specification Index](README.en.md) · [Previous: Framework Error Model](32-framework-error-model.en.md)

> **This chapter defines** — why Core queue byte HWM and Framework Application Job Queue
> job-count pressure are separate, the only runtime boundary where the controls meet, and the
> permit ordering for ordinary ingress.

## 1. Design Intent

Core and Framework observe different overloads.

- Core knows the frame bytes currently held by transport queues and limits queue-memory bursts
  caused by connection count and payload size.
- Framework knows how many application jobs are waiting before handler start and limits ingress
  that exceeds application processing capacity.

Copying one layer's values into the other layer's counters creates overlapping authority. Core
must not infer handler capacity, and Framework must not calculate socket-queue bytes. The two
limits therefore do not share settings, profiles, units, accounting boundaries, or observations.

Core HWM is the transport path's last safety boundary. Even when Framework reduces remote traffic
early, data can already be present in remote Core queues, OS buffers, the network, and local Core
queues. Conversely, a Core queue can be empty while handler jobs wait for a long time. Both
protections are required, but they are not the same protection.

## 2. Two Independent Capacity Authorities

| Authority | What it limits | Accounting or acquisition | Release |
|---|---|---|---|
| Core byte HWM | Physical-frame charge currently owned by a Core application-direction queue | When the Core queue owns a frame | When the Core queue gives up frame ownership, including receive dequeue |
| Framework Application Job Queue | Application job permits accepted by a host before callback start | Reserve immediately before ordinary receive or claim, then transfer to a handler turn | Immediately before the callback's actual first instruction, or at a pre-callback terminal |

Core frame charge includes the payload and metadata bytes defined by the Core contract. Once the
Core queue hands a record to the binding, that record's Core HWM charge ends. Framework does not
request a retained-credit lease or extend Core byte charge through handler or reply lifetime.

Post-receive payload storage follows the ordinary message ownership in [Payload Ownership And
Copying](50-internal-message-ownership.en.md). Copying, moving, and releasing that storage manage
payload lifetime; they are not HWM credit, a second job permit, or another byte-pressure authority.

Framework pressure uses this count:

```text
application job permits in use
  = reserved supply permits
  + queued application jobs
```

A capacity waiter has not received a permit and is not included. Converting a reservation to a
queued job does not change the total; only returning the permit decreases it. A record that creates
1:N callbacks uses one permit for each actual callback turn.

## 3. Configuration And Profile Boundary

Core and Framework profiles may use the same labels, but they are different public types with
different calculations.

| Setting group | Owner | Default profile | Manual override |
|---|---|---|---|
| `CoreHwmMemoryLimitBytes`, `CoreHwmBudgetBytes`, `CoreHwmProfile` | Core | `Balanced` | Core memory or budget bytes |
| `ApplicationJobQueueProfile`, `MaxQueuedApplicationJobs` | Framework host | `Balanced` | Exact host job-permit limit |

Framework may pass Core configuration values to binding context options during startup, but it
does not apply profile ratios or divide a budget by connection count. Projecting a Core snapshot
into Framework status is also read-only observation and is not an input to Framework pressure.

At runtime, the only feedback that Framework job pressure gives Core is one absolute
`RUNNING`/`PAUSED` receive-flow state applied to supported sockets. Framework does not change Core
HWM settings or queued-byte counters when this state changes.

## 4. Ordinary Ingress Permit Order

Ordinary ingress follows this order.

1. Wait for a host-shared permit in oldest-live-source order.
2. Receive or claim a record from Core or the binding only after acquiring the permit.
3. Transfer an application record's permit to its handler turn in an owner mailbox or serial queue.
4. Return the reservation after finite internal handling of a control or malformed ordinary record.
5. Return the permit at the common invocation boundary immediately before the callback's actual
   first instruction.

An `await`, coroutine suspension, continuation, or reply wait after handler start does not acquire
the same permit again. A non-runnable durable backlog such as relocation returns its initial
reservation after a finite handoff to the payload owner defined by that relocation specification;
each runnable callback turn later acquires a new permit.

Only supply identified before receive as a terminal reply or error-reply completion bypasses the
ordinary ingress permit. A record first received from an ordinary connection cannot be classified
later and retroactively gain this bypass. This separation lets terminal completions of operations
already in progress continue while the ordinary queue is saturated.

Framework heartbeat, topology, relocation, and service-wire `SendReady` kind `12` are not this
completion supply. Those control records remain on the application data line under its existing
FIFO and liveness contract; they are not moved to the Core completion connection or to a separate
Framework control queue.

The following are not allowed:

- Receiving first without a permit and incrementing a separate counter afterward
- Reimplementing Core HWM with retained-credit leases or a Framework byte HWM
- Replacing saturation with rejection, dropping, fixed-delay polling, or busy spinning
- Storing records in an unbounded or hidden side backlog not owned by a specification

## 5. Pressure State And Core Connection

For effective maximum `M` and configured pause and resume percentages `P` and `R`, startup computes:

```text
pause permit count  = ceil(M * P / 100)
resume permit count = floor(M * R / 100)
```

`P` is in `1..100` and defaults to `80`; `R` is in `0..99`, defaults to `60`, and must satisfy
`R < P`. The state changes from `running` to `paused` when permits in use reach the pause count,
and from `paused` to `running` when they fall to the resume count. The current state is retained
between the two thresholds.

Framework applies the new absolute state only to paired DEALER/ROUTER sockets used by RouteMesh
and ClientServer. PUB/SUB, Classic fanout, and STREAM are outside this integration and retain their
existing Core byte HWM and structural queue limits.

`PAUSED` does not change a Core HWM value. Core independently composes a remote-pause blocker and a
local byte-HWM blocker. `RUNNING` removes only the remote-pause reason, so a send remains waiting
while local HWM is still full. Pressure state does not directly change route readiness or transport
liveness.

## 6. Socket Lifecycle And The Single Control Point

The host queue owner computes pressure state in the same synchronization boundary as permit-count
changes. On a transition, it applies the new absolute state to a snapshot of supported sockets. It
does not repeat an already-applied state, and an older transition sequence cannot overwrite a newer
state.

A new socket receives the current host pressure state before publication in the receive-target
registry. Close removes the socket from the registry first. Binding calls run outside queue,
registry, and user-callback locks. Configuration failures other than lifecycle results racing with
close are recorded in diagnostics and metrics.

This receive-flow state API is the only runtime control point between Framework pressure and Core
send flow. Framework does not create raw flow frames or use the Core control lane as a general
Framework channel.

## 7. Composition With Send Completion

Core and the binding own HWM waiting, internal retries, and per-operation completion. Framework
selects an exact target and starts one binding operation. After the operation starts, Framework
does not select another target or create a second operation for the same payload because of PAUSE
or HWM.

Framework has no removed `send_ready` callback or event, readiness waiter, or retry adapter.
Deadline, cancellation, detach, and shutdown follow the existing first-terminal rule of the
operation state machine. Framework service-wire `SendReady` kind `12` is a Framework service-control
record and is a different contract from the removed binding callback.

## 8. Large Payloads And Operational Values

The Application Job Queue limits job count; it does not weight jobs by payload bytes. An empty
payload and a large payload each consume one job. The Framework queue limit is therefore not a
process-memory byte hard cap.

For workloads that retain large payloads for a long time, measure production-equivalent payload
distribution, permits in use, process memory, throughput, and latency, then lower
`MaxQueuedApplicationJobs`. Limit an individual message separately with `MaxMessageSize`. Do not
connect the Core profile to the Framework profile or restore retained-credit leases to solve this
problem.

Core HWM remains the final safety boundary for Core queue memory. When Framework stops ordinary
receive while waiting for permits, bytes accumulate in the local Core receive queue, and finite
Core HWM plus TCP backpressure limits sender progress.

## 9. Contract Test Requirements

Each Framework runtime verifies at least the following:

- Core profile and Application Job Queue profile can be configured independently, and each
  defaults to `Balanced`.
- Without a permit, the next ordinary record is not received early.
- Reservation, queued-job, and callback-first-instruction counts follow the same permit rule.
- The 80% pause, 60% resume, hysteresis interval, and duplicate-state suppression are exact.
- New-socket synchronization, close races, and stale transitions preserve the latest absolute state.
- Receive-flow state is applied only to supported paired sockets.
- Completion supply progresses independently of ordinary permit saturation.
- Framework uses neither retained receive, a `send_ready` waiter, nor a separate send retry.

Exact configuration values are defined by [Framework API](06-framework-api.en.md); status and metrics
by [Runtime Status](24-runtime-monitoring.en.md) and [Runtime Metrics](25-runtime-metrics.en.md); and the
permit implementation by [Receive And Dispatch Loop](46-internal-dispatch-loop.en.md).
