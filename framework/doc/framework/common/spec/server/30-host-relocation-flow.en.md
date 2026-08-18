---
title: "Complete Host Relocation Flow"
---

# Complete Host Relocation Flow

[Spec table of contents](README.en.md) · [Previous: Transport Connection Liveness](29-transport-liveness.en.md) · [Next: Failure Handling And Failover Scope](31-failure-failover-policy.en.md)

> **What this chapter defines** — the complete sequence and results in which Host `Relocate`
> fixes the stateful workload as relocation units, selects targets, moves the units, returns
> `Relocated`, and removes source resources through Message Follow and `Shutdown`.


## 1. Questions This Document Answers

This document defines which operations the application calls when moving a host's
stateful workload to another node or shutting the host down, the order in which the
framework processes them, and what result each call ends with.

The unit that runs message handlers and Actors is called a
[Spot](01-glossary.en.md#spot). In this document, stateful workload means the Actors/
Spots a host is processing and the not-yet-finished messages and timers.

To inspect or reboot a node while keeping the application version, call `Relocate` with
`PlannedMaintenance`. To switch to a prepared new application version, call it with
`RollingUpdate`. The way the target version is chosen is called a
[relocation mode](01-glossary.en.md#relocation-mode).

On success, both modes only detach the stateful workload from the source host. The host
and infrastructure connections are kept. The application or deployment orchestrator
confirms this result and then separately calls
[Shutdown](01-glossary.en.md#shutdown).

To shut down a host without guaranteeing stateful workload continuity, call `Shutdown`
alone, without `Relocate`.

### 1.1 Failure-Handling Scope

`Relocate` only supports a graceful handoff that runs until the source runtime, the
chosen target runtime, and the Location Store finish the operation. A transient Store or transport error within the same process can be retried
within the deadline. But once the source or target process terminates, a different
runtime doesn't take over the relocation, and there's no automatic recovery by picking a
different target. Recovery after the source or target process terminates is outside this
contract.

The rule against creating two owners at once still applies. If
a Location Store change's result isn't received, success or failure isn't guessed — the
same record is re-read. Source admission isn't reopened, and target application message
processing isn't started, before confirming the actual owner.

The name distinguishing a RouteMesh group of nodes connected under the same name is
called a [MeshName](01-glossary.en.md#meshname). The name selecting a target among nodes
participating in the same Channel is [ChannelName](01-glossary.en.md#channelname). The
application doesn't directly assemble a shutdown sequence by picking only some
components via MeshName, ChannelName, or node RID.

A [RouteMesh](01-glossary.en.md#routemesh), the connection group where several nodes
exchange messages, a runtime node participating in that group
([MeshNode](01-glossary.en.md#meshnode)), a ClientServer server, and a
[Classic fanout](01-glossary.en.md#classic-fanout) publisher are all coordinated
together at the host level.

This document owns the host lifecycle observed by the application, the rules for fixing Actor
and Spot relocation units, the complete order for removing source resources after cutover, and
the handoff result. The node currently processing an Actor/Spot is called the
[owner](01-glossary.en.md#owner). The record by which multiple nodes together judge an
Actor/Spot's owner and location is called
[authority](01-glossary.en.md#authority). The order in which the framework uses
authority and the two Stores is defined by
[40 Location Runtime](21-location-runtime.en.md). The provider contract for the
[Location Store](01-glossary.en.md#location-store), which stores the current owner and
location, is defined by
[41 Location Store Provider](22-location-store-redis.en.md). The application state and
unexecuted queue/timer payload being moved doesn't pass through a store — it's sent
directly from source memory to the target, and that transfer contract is owned by
[Complete Actor And Spot Relocation Flow](28-relocation-flow.en.md). The provider
contract for the responsibilities remaining in the Relocation Store — the record made
when an Instance Spot is newly created by its first message, and the terminal result of
a pending request completing after relocation — is defined by
[42 Relocation Store Provider](23-relocation-store-redis.en.md). This document doesn't
repeat transfer format — it only defines the public order of host operations.

## 2. Operations The Application Chooses

### 2.1 Choosing A Relocate Mode

The caller must specify a mode when calling `Relocate`. The only difference between the
two modes is the target application version. The subsequent queue seal, state restore,
authority transition, and session handoff rules are the same.

| Value | Mode | Value the caller specifies | Target the framework selects |
|---:|---|---|---|
| 0 | `PlannedMaintenance` | Doesn't specify `TargetApplicationVersion`. | Selects only a node whose application version exactly matches the source. |
| 1 | `RollingUpdate` | Specifies a `TargetApplicationVersion` greater than the source. | Selects only a node whose application version exactly matches what the caller specified. |

`PlannedMaintenance`'s effective target version is the source's `ApplicationVersion`.
Specifying a target version alongside it is an argument error. `RollingUpdate` is an
argument error if there's no target version or it's at or below the source version. The
framework rejects these invalid combinations before changing runtime state or admission.

The final time an operation must finish is called a
[deadline](01-glossary.en.md#deadline). Both modes use 30 seconds if `Deadline` is
omitted. A specified value must be greater than 0.

### 2.2 Public Operation

The following .NET declaration is one expression of the common contract. The exact name
and signature in other languages is defined by that language's interface document.

```csharp
public enum ZLinkFrameworkRelocationMode
{
    PlannedMaintenance = 0,
    RollingUpdate = 1
}

public sealed record ZLinkFrameworkRelocationOptions
{
    // choose whether this is maintenance at the same version or a new version rollout.
    public required ZLinkFrameworkRelocationMode Mode { get; init; }

    // only for RollingUpdate — specify the exact version, greater than the source.
    public long? TargetApplicationVersion { get; init; }

    // uses 30 seconds if omitted.
    public TimeSpan? Deadline { get; init; }
}

public readonly record struct ZLinkFrameworkRelocationResult(
    ZLinkFrameworkRelocationMode Mode,
    long TargetApplicationVersion,
    ZLinkFrameworkRelocationOutcome Outcome,
    ZLinkFrameworkRelocationReason Reason);

public interface IZLinkFrameworkRuntime
{
    // provides the current host lifecycle state and last result.
    ZLinkFrameworkRuntimeStatus Status { get; }

    // observes host state and terminal result changes in order.
    IAsyncEnumerable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> ObserveAsync(
        CancellationToken cancellationToken = default);

    // moves stateful workload; stays in the Relocated state on success.
    ValueTask<ZLinkFrameworkRelocationResult> RelocateAsync(
        ZLinkFrameworkRelocationOptions options,
        CancellationToken cancellationToken = default);

    // cleans up accepted work and host resources without a new relocation.
    ValueTask<ZLinkFrameworkTerminationResult> ShutdownAsync(
        TimeSpan? deadline = null,
        CancellationToken cancellationToken = default);
}
```

A typical rolling-update call sequence looks like this.

```csharp
var relocation = await runtime.RelocateAsync(
    new ZLinkFrameworkRelocationOptions
    {
        // only use nodes prepared as N+1 as target candidates.
        Mode = ZLinkFrameworkRelocationMode.RollingUpdate,
        TargetApplicationVersion = currentVersion + 1,
        Deadline = TimeSpan.FromSeconds(30)
    },
    cancellationToken);

if (relocation.Outcome == ZLinkFrameworkRelocationOutcome.Relocated)
{
    // once relocation success is confirmed, clean up host resources separately.
    // shutting down immediately also removes the Message Follow routes and
    // the cutover retransmission copies (§11).
    await runtime.ShutdownAsync(cancellationToken: cancellationToken);
}
```

Calling `Shutdown` right after `Relocated`, as in the example above, is allowed. A
deployment that wants to use both old-route message forwarding and cutover
retransmission to the end waits until [`SafeToShutdown`](01-glossary.en.md#safe-to-shutdown) — the observation value
indicating the source runtime can shut down safely — is published in the runtime status
before calling `Shutdown`; the publication condition is explained by
[§11](#11-the-race-between-shutdown-and-relocate).

A `Relocate` result always includes the mode and effective target version. Even when no
target is found, the same values are preserved so the caller can confirm what condition
it was waiting on. If `Relocate` ends `Blocked`, the caller can retry, or `Shutdown`
without continuity.

## 3. Host State And Completion Results

The host lifecycle is owned by a single `FrameworkRuntimeState`.

Information a host publishes to the Store — its endpoint, run generation, and
capabilities — is called a [descriptor](01-glossary.en.md#descriptor). The state where
it's ready to accept new application work is called
[ready](01-glossary.en.md#ready).

| Value | State | Meaning |
|---:|---|---|
| 0 | `Preparing` | Proceeds with registration, bind, descriptor verification, and recovery; doesn't accept application messages. |
| 1 | `Serving` | The host is ready and accepts new application work. |
| 2 | `Relocating` | Excluded from new placement and selection, but local units not yet sealed keep processing messages and timers. |
| 3 | `Relocated` | Every stateful object has been detached from source dispatch. Host and infrastructure connections are kept. |
| 4 | `Draining` | `Shutdown` has closed new admission and is cleaning up already-accepted work and resources. |
| 5 | `Stopped` | Application resource, infrastructure resource, and listener cleanup are finished. |
| 6 | `Error` | Can't serve due to a startup or runtime error. |

`IsReady` is true only in `Serving`.
A component lifecycle snapshot is information observing each component's state and
doesn't substitute for host state. Per-component `Drain`, `AwaitDrained`, `Stop`, or a
public operation targeting only some Meshes isn't provided.

```mermaid
stateDiagram-v2
    [*] --> Preparing
    Preparing --> Serving: required component ready
    Preparing --> Error: startup error
    Serving --> Relocating: Relocate intent published
    Serving --> Draining: Shutdown seals admission
    Serving --> Error: runtime error
    Relocating --> Serving: source processing restored after Blocked
    Relocating --> Relocated: every relocation unit detached
    Relocating --> Draining: Shutdown requested
    Relocated --> Draining: Shutdown requested
    Error --> Draining: bounded cleanup starts
    Draining --> Stopped: resource cleanup complete
```

For each unit, only an explicit failure before its relay-ready reply reaches the accepted
state may clean tentative work and restore source processing. Even when another unit has
crossed that boundary, the host may restore source workload that hasn't crossed it and
return to `Serving`. A unit across the boundary never returns to source regardless of its
cutover-submit result; it continues through target cutover receipt (including a
retransmission after connection re-establishment) or the cutover-wait fallback.
Returning to `Serving` doesn't mean every unit rolled back to the source.

Relocation outcome is fixed to the following values.

| Value | Outcome | Allowed reason | Meaning |
|---:|---|---|---|
| 0 | `Relocated` | `None` | Every stateful object has been detached from source dispatch. |
| 1 | `Blocked` | `TargetUnavailable`, `StoreUnavailable`, `RelocationDisabled`, `StateIncompatible`, `DeadlineExceeded`, `RelocationFailed`, `RuntimeNotReady`, `ManualTopologyUnsupported`, `ShutdownRequested`, `OperationInProgress` | Relocation couldn't start, or the whole workload move didn't finish. |

The wire values are `Relocated=0`, `Blocked=1`. Reason is `None=0`,
`TargetUnavailable=1`, `StoreUnavailable=2`, `RelocationDisabled=3`,
`StateIncompatible=4`, `DeadlineExceeded=5`, `RelocationFailed=6`,
`RuntimeNotReady=7`, `ManualTopologyUnsupported=8`, `ShutdownRequested=9`,
`OperationInProgress=10`. An undefined outcome/reason combination is a protocol error.

Shutdown outcome is `Stopped=0`, `ForceStopped=1`, and reason is `None=0`,
`DeadlineExceeded=1`, `TeardownFailed=2`. `ForceStopped` isn't a separate host state — it's
the result of finishing cleanup via bounded teardown, with host state `Stopped`.
Relocation failure is owned by the relocation result and isn't mixed into the
termination reason.

## 4. Conditions Checked Before Selecting A Target

`Relocate` started from `Serving` checks the whole host at once before changing host
state and application admission. At this point it doesn't block new work for the moving
targets or pre-secure target capacity.

The way of saving application state as bytes and restoring it on the target is the
[Preserve-state relocation policy](01-glossary.en.md#preserve-state-relocation-policy).
The period during which a host keeps current owner eligibility is called an
[owner lease](01-glossary.en.md#owner-lease).

| Check item | Passing condition |
|---|---|
| Concurrently in-progress work | If there's new object creation, join, Instance placement, session binding, or inbound relocation, the work to finish first is confirmed. |
| Local workload | Checks every MeshNode's Actors, Spots, timers, sessions, and in-progress infrastructure operations. |
| Store | The Location Store's current location record and the target descriptor's owner lease must be usable. Because the payload being moved is sent directly from source memory, Relocation Store readiness isn't a handoff condition — it's checked only in deployments that need the remaining responsibilities: the record made when an Instance Spot is newly created by its first message, and the terminal record of a pending request completing after relocation. |
| Unit compatibility | The relocation policy and state adapter used by an explicitly-created [User Spot](01-glossary.en.md#entry-user-instance-spot), an [Instance Spot](01-glossary.en.md#entry-user-instance-spot) created on demand by its first message, and the Actors within them, are all compatible with the target [factory](01-glossary.en.md#factory) and capacity. |
| Topology | Every service topology the host uses finds remote endpoints via Store descriptors. This method is called [automatic discovery](01-glossary.en.md#automatic-discovery). |

If even one manual RouteMesh peer, ClientServer client endpoint, fanout subscriber
endpoint, or manual fanout publisher not publishing a descriptor is registered, it's
`Blocked/ManualTopologyUnsupported`. Registration is checked, not current connection
status. A setting where an automatic component explicitly binds a listener address isn't
manual topology.

The scope the runtime checks is local registration. It doesn't check connections another
process made outside the framework, so the condition that the whole set of participating
processes uses only automatic discovery is guaranteed by the deployment.

## 5. Selecting A Target Matching The Mode

The framework narrows down targets in this order.

| Order | Condition |
|---:|---|
| 1 | `PlannedMaintenance` keeps only the same application version as the source. `RollingUpdate` keeps only the version the caller specified, excluding both lower and higher versions. |
| 2 | Keeps only an Object Server that isn't the source and is in `Serving` state. |
| 3 | Keeps only nodes compatible in [factory](01-glossary.en.md#factory) — the function the application registered to create objects — [stable type](01-glossary.en.md#stable-type) — the language-independent object type name — relocation policy, and state adapter. |
| 4 | Keeps only nodes with sufficient remaining capacity, and, if the source has a [maintenance wave](01-glossary.en.md#maintenance-wave) set, only nodes with a different value. Maintenance wave is an application setting distinguishing hosts in the same maintenance operation. |
| 5 | Keeps only nodes whose RID and [lifecycle generation](01-glossary.en.md#lifecycle-generation) both match in the same-point-in-time descriptor list and the Core peer table. Lifecycle generation distinguishes different process runs using the same RID. That peer must be `Admitted` and `Ready`. |

Version is checked before capacity and placement weight. So a different-version node's
remaining space or higher weight doesn't affect the selection result. The relative share
by which new work is assigned among several candidates is called
[weight](01-glossary.en.md#weight). This value only applies when there are multiple
targets satisfying the conditions.

A descriptor being published, or a connect intent having been made, alone doesn't judge
a target as ready. The result of copying state at a specific point in time into a
read-only value is called a [snapshot](01-glossary.en.md#snapshot). If the descriptor
snapshot is empty, contains only the source itself, or every remote peer is draining,
there's no target.

### 5.1 When There's No Target Yet

Before searching for a target, whether there's a unit to move is checked first. If the
source owns no Actor, User Spot, or Instance Spot, there's nothing to move, so a target
isn't searched for and it ends with `Relocated/None`. Relocation's purpose is workload
migration — a host with no workload to move isn't blocked just because it has no target.
Even in this case, the host state transition and admission closing are the same as any
other relocation.

If there's a unit to move but no target of the requested version exists, source state
and admission are kept while waiting up to the deadline for the descriptor and Core peer
table to converge. A process with multiple Meshes must satisfy the condition on every
Mesh. If no target is secured by the deadline, tentative coordination is cleaned up and
`Blocked/TargetUnavailable` is returned.

```mermaid
sequenceDiagram
    participant Target as Replacement node
    participant Source as Source host
    participant Store as Location Store
    participant App as Deployment orchestrator

    Target->>Store: [request] publish replacement Serving descriptor
    Store-->>Target: [reply] descriptor version fixed
    App->>Source: [request] relocate host workload to requested-version target
    Source->>Source: [local] check requested-version peer is Ready
    Source->>Store: [request] transition source host to Relocating
    Store-->>Source: [reply] Relocating state fixed
    Source->>Target: [request] prepare temporary queue, Restore, and relay for every unit
    Target-->>Source: [reply] relay reception ready for every unit
    Source->>Target: [send] ingress-hold relay and cutover for every unit
    Source->>Store: [request] transition source host to Relocated
    Store-->>Source: [reply] Relocated state fixed
    Source-->>App: [reply] host relocation result Relocated
    App->>Source: [request] clean accepted work and shut down host
    Source->>Source: [local] clean up accepted work and infrastructure
    Source-->>App: [reply] shutdown result Stopped or ForceStopped
```

`Relocated` is the source-side result that every unit's cutover submit attempt reached a
success or failure terminal; it doesn't mean the source awaited a target-CAS completion
reply. `Relocated` keeps descriptor, connection, listener, and infrastructure resources. This
diagram shows the normal flow when a target is ready. Without a target, the deadline
rule from the previous section returns `Blocked/TargetUnavailable`.

Automatic ClientServer clients and fanout subscribers build new connections using the
replacement descriptor and reflect the source's state in selection. An existing
connection with accepted work and a remaining barrier isn't closed immediately just
because the descriptor changed.

## 6. Concurrent Calls And Cancellation

| Situation | Result |
|---|---|
| Concurrent `Relocate` with the same mode and effective target version | Shares the deadline of the first operation. A later call doesn't change the deadline. |
| Concurrent `Relocate` with a different mode or effective target version | `Blocked/OperationInProgress` without waiting. |
| `Relocate` again after `Blocked` | `Blocked` isn't stored, so host conditions are re-checked from the start. |
| `Relocate` again in `Relocated` | Returns the first `Relocated/None`'s mode and effective target version. |
| Concurrent `Shutdown` | Shares the same operation and stores the terminal result. |
| `Shutdown` again in `Stopped` | Returns the stored result. If none, `Stopped/None` with no new work. |
| `Relocate` in `Preparing`, `Error`, or `Stopped` | `Blocked/RuntimeNotReady` without changing admission. |

Caller cancellation ends only that waiter — it doesn't cancel the shared operation.
`Shutdown` interrupts startup in `Preparing` and starts bounded cleanup in `Error`.

<a id="7-relocation-units-and-concurrency-limits"></a>

## 7. Relocation Units And Execution Order

The bundle of Actors or a Spot the framework can move independently is called a
[relocation unit](01-glossary.en.md#relocation-unit). The relationship of which Entry
Spot or User Spot an Actor currently belongs to is
[Actor membership](01-glossary.en.md#actor-membership).

| Relocation unit | Boundary |
|---|---|
| `SpotWide` User Spot aggregate | The User Spot and every member Actor at the moment new work is blocked |
| Actor | One Actor belonging to an Entry Spot or `PerActor` User Spot |
| `PerActor` Spot authority transition | The target's stateless Spot shell and Spot-level queue authority |
| Instance Spot | One Spot with no Actor membership |

The Entry Spot itself doesn't move. A source Entry Spot's Actor moves as an Actor unit
to the target node's Entry Spot. A `PerActor` User Spot also moves its Actors as the
same unit without moving Spot application state.

The source runtime reads active state once immediately before the Host enters `Relocating` and
fixes the inventory. An Actor in a User Spot membership appears exactly once, either as a
`PerActor` Actor unit or inside a `SpotWide` aggregate according to the Spot execution mode.
An Entry Spot Actor and a standalone Actor not included in a User Spot membership each become an
Actor unit. The same Actor does not appear in both a Spot unit and a standalone unit. No new
stateful placement is admitted after the inventory is fixed.

Host relocation runs the following batch order. A batch means units with the same dependency;
it is not a fixed-size chunk based on a numeric unit count.

| Batch | Units | Start condition |
|---:|---|---|
| 1 | Stateless Spot shell and Spot-level queue authority for each `PerActor` User Spot | Inventory and target preflight are complete and the current Spot turn has ended. |
| 2 | Entry Spot Actors, `PerActor` member Actors, and standalone Actors | Each Actor's current turn has ended; a `PerActor` member also waits for its Spot shell transition. |
| 3 | `SpotWide` User Spot aggregates and Instance Spots | The entire aggregate's current turn and application safe point have ended. |

The next batch starts after the previous batch reaches a terminal state. Units without a
dependency in the same batch may run concurrently. A `PerActor` member Actor never starts before
its Spot shell, and a `SpotWide` member Actor is not split into a separate Actor unit. When a unit
fails, no later batch starts. Units that have already started continue to a safe terminal state.

```mermaid
flowchart LR
    I[Fix active inventory] --> S[Batch 1: PerActor Spot shells]
    S --> A[Batch 2: Entry, PerActor, and standalone Actors]
    A --> G[Batch 3: SpotWide and Instance aggregates]
    G --> C[Finish every cutover submit attempt]
    C --> R[Publish Host Relocated]
```

Relocation correctness adds no separate cap on concurrent unit count, participant count, or
relay record count. How much moves concurrently per unit is paced by the in-flight payload
budget, which limits the bytes in-progress relocation payloads occupy on a peer connection at
the same time; the budget's calculation and waiting rules are owned by
[Complete Actor And Spot Relocation Flow
§5.3](28-relocation-flow.en.md#53-no-relocation-specific-capacity-limit). When the budget is
full, the next unit waits before its source admission seal is applied, and a waiting
Actor/Spot keeps processing messages normally in the meantime — the coordinator doesn't set a
separate cap on concurrent unit count. Existing runtime and Store provider memory, frame, and
page-size limits still apply. If a resource isn't immediately available, the Framework waits
before blocking source application dispatch. Reaching a limit does not fail a relocation that
has already started.

If the target is already saturated, chunk intake slows down, so the source's budget releases
come later and the next unit's start is also delayed — a move meant to shed load is slowed by
that load. This is the intended result of the backpressure contract that never bypasses
saturation; no bypass path is created for relocation. If the move doesn't finish within the
host operation deadline, the existing rule (§7.1) that no new unit relocation starts applies,
so the operator first decides between extending the deadline and relieving target load.
Deadline sizing uses the transfer throughput observed in that deployment as input, not a
formula.

This batch order is not an Application Job Queue capacity chunk. Each target's pre-dispatch
temporary queue and saved work form an ordered durable backlog owned by a retained-byte owner;
ordinary staging ingress uses a shared reservation for receive and returns it at durable
handoff. After target-only CAS and required lifecycle work make dispatch runnable, backlog
handler turns acquire live queued-job permits one at a time in order. A compatible target whose
job limit is smaller than an aggregate backlog therefore executes it progressively instead of
returning a capacity blocker or leaving members at the source.

`SpotWide` moves the Spot and every member Actor at the end of the current turn as one
unit. `PerActor` and Entry Spot move each Actor once that Actor's current turn ends. A
`PerActor` Spot lane is only briefly blocked during authority transition and doesn't
wait for every member Actor to become ready at the same time.

A `SpotWide` User Spot using `ApplicationSignaled` readiness uses the turn boundary
registered by the application through `RelocationReady().Defer()` after target
preparation. If no relocation is prepared, a `Continued` completion callback runs on
the source in the next application turn. Cancellation before relay-ready is accepted
restores the source queue and then calls `Continued`. After that boundary, cancellation
or cutover-submit failure doesn't restore source. After owner commit, `Relocated` runs
as the first application turn on the target queue.

### 7.1 Service Interruption Time Target Per Relocation Unit

| Target | Measurement unit |
|---|---|
| Entry Spot Actor | One Actor |
| `PerActor` User Spot | One Spot direct admission and each Actor |
| `SpotWide` User Spot | One aggregate including the Spot and member Actors |
| Instance Spot | One Spot |

Measurement starts when source admission seal is applied. Wait time for target selection,
target preparation including temporary-queue registration, the current turn, or an
application safe point is excluded. This preparation must finish before source admission is
blocked. Capture, encoding, chunk transfer to the target, checksum verification, authority
change, target Restore, and queue/timer restore run after the seal and are all included in
measurement. Measurement ends when the one-way cutover submit after ordered relay reaches a
terminal result, whether success or failure. What this metric measures is the time the
source is stopped — not the full interruption the application observes until the target
resumes processing.

Each relocation unit records the following points in time. Each point is recorded, on its
own clock, by the node where the event happens.

| Point | Meaning | Recorded by |
|---|---|---|
| S0 | Source admission seal applied | Source |
| S1 | Terminal result of the one-way cutover submit | Source |
| S2 | Target's Location Store CAS confirmed | Target |
| S3 | Target application dispatch opened | Target |
| S4 | The moment the Message Follow route may be removed. Based on `MessageFollowDuration` expiry; the source owns the Message Follow route, so this is a source-side point. | Source |

| Metric | Interval | Meaning |
|---|---|---|
| Source stop time | S0→S1 | Same as the measurement interval defined above. |
| Target resume time | S2→S3 | Target-local interval from owner confirmation to opening application dispatch. |
| Route convergence time | S1→S4 | Source-local interval that grounds how long the source must keep the Message Follow route. |

No metric subtracts timestamps of different nodes directly. A whole interval crossing
nodes, like S0→S3, is observed only through same-flow correlation in
[message flow tracing](26-message-flow-tracing.en.md). S3 is the dispatch-open moment, so
because of backlog and permit ordering, the moment the application observes its first
handler result can be later. The formal definition of metric names and instruments is
owned by [50 Runtime Monitoring](24-runtime-monitoring.en.md) and
[51 Runtime Metrics](25-runtime-metrics.en.md); this section fixes only the point
definitions and who measures them.

Each unit targets under 1 second by default. 1 second isn't a timeout or a correctness
condition. Exceeding it doesn't cancel the relocation or roll back to the source. The
framework keeps the same operation going through the one-way cutover submit terminal,
and records a warning and the `zlink.relocation.interruption` histogram. Source application
close runs after that success or failure terminal. The target sends no processing-start
acknowledgement; target admission opening is observed through target-local status and tracing.

Once the host operation deadline ends, no new unit relocation is started. A unit already
started performs a safe abort only after target explicitly fails before sending its
relay-ready reply. If the reply result is indeterminate, target may have started the
cutover-wait fallback, so source dispatch doesn't reopen. A unit that attempted cutover also
doesn't roll back to source, and its target continues the owner transition until the
Restore validity deadline. If source doesn't attempt every unit's cutover, the host
doesn't become `Relocated`; success or failure of an attempted submit isn't a completion
condition.

## 8. The Order For Relocating One Unit

The single source for one Actor or Spot unit's owner transition, ordered relay, queue
merge, and Session route is
[Complete Actor And Spot Relocation Flow](28-relocation-flow.en.md). This section adds
only the host operation's unit enumeration, mode, and whole-operation completion rules.

Once host relocation starts, the framework moves the source host's workload to the
target node without the application calling a separate move API per Actor or Spot.
Actor and Spot IDs are kept, and once moved, a target continues message processing in
existing queue order. One Actor or bundle of Spots the framework moves at once is called
a [relocation unit](01-glossary.en.md#relocation-unit).

### 8.1 Who Does What

| Actor | What it does |
|---|---|
| Application | Calls the host's `Relocate`. Only a `SpotWide` User Spot that chose `ApplicationSignaled` signals a safe move moment via `RelocationReady().Defer()`. |
| Source runtime | Finishes currently running work and stops application dispatch. Fixes application state and the not-yet-executed queue/timers as a payload in source memory and sends it directly to the target, and relays only post-capture messages that arrive at the old address. It doesn't change the Location Store. |
| Target runtime | Assembles the received chunks and verifies the checksum, then creates an Actor or Spot using the same ID and restores state and existing work. After receiving the relay cutover boundary or reaching the cutover-wait fallback after relay-ready, it CASes the Location Store from source to target and opens the queue only if that succeeds. |
| Location Store | Records which node currently processes an Actor or Spot. When multiple values must change together, changes all or none. |
| Relocation Store | Holds no handoff payload. As its remaining responsibilities it records only the record made when an Instance Spot is newly created by its first message, and the terminal result of a pending request completing after relocation. |

### 8.2 The Common Order Every Actor And Spot Follows

1. The source runtime first checks whether the Actor or Spot can be created on the
   target and whether normal host admission permits it. It negotiates no per-relocation
   message/byte allowance or participant reservation. New source work isn't blocked
   before this check finishes.
2. Once ready, it finishes only currently running handlers and timer callbacks up to
   that point. Messages arriving afterward, and timers not yet started, are held in the
   source runtime's ingress hold. The hold is temporary relocation storage that exists
   only on the source. Relocation does not add its own record-count or byte bound to this
   storage.
3. The source runtime fixes not-yet-executed messages, timer information, and
   application state as one payload. If `PreserveStateWith` was chosen, the state the
   application adapter's `Capture` returned is included in the payload. The payload's
   original lives in source memory; it isn't written to the Relocation Store. This payload
   exclusively owns the confirmed queue prefix and timers; source relay doesn't recreate them.
4. The source runtime requests temporary-queue installation, object creation and Restore,
   and relay preparation from the target runtime. The Restore request carries the
   payload's total encoded length, chunk count, and whole-payload checksum. Before
   dispatching the next packet, the target dispatcher registers a
   [relocation temporary queue](01-glossary.en.md#relocation-temporary-queue) for that
   object kind, ID, and `ObjectGeneration`. Afterward, incoming messages for that object
   go into the temporary queue without looking up the real instance. The source sends the
   payload in chunks over the same ordered mesh connection relay uses, and other messages
   on that connection may flow between chunks. The exact contract for chunk size,
   negotiation, the in-flight budget, and chunk headers is owned by
   [Complete Actor And Spot Relocation Flow](28-relocation-flow.en.md).
5. The target assembles the arriving chunks, matches them against the Restore request's
   checksum, then creates the Actor or Spot and Restores application state. On a checksum
   mismatch it doesn't restore from a partial assembly and answers with an explicit
   failure. Transferred existing work and timers aren't executed yet. Once the temporary
   queue, assembly/verification, and Restore are ready,
   target reports relay reception ready to source. This isn't relocation completion.
   After receiving it, the source runtime sends ingress-hold messages over the same relay
   connection. The target dispatcher puts relayed messages into the temporary queue
   group's pre-boundary relay span.
   Normal server-to-server relay uses TCP ordering and retransmission; relocation adds
   no per-message ACK, sequence journal, or separate capacity limit.
6. After relaying the current prefix accumulated in the post-capture ingress hold, the
   source sends cutover one-way on the same ordered connection. Queue work and timers
   already transferred in the payload aren't relayed. Cutover tells
   target that all pre-boundary relay was sent. It keeps
   accepting new messages into the post-marker span, so it doesn't wait for mailbox
   drain. After Restore, target CASes the Location Store from source to target when
   cutover arrives. If the connection drops and cutover is lost, and the source is still
   running, target may receive the boundary batch and cutover again over a re-established
   connection (§10). If neither cutover nor a retransmission arrives within the cutover
   wait time (`RelocationCutoverWaitTimeout`, default 1,000ms — owned by
   [Framework API](06-framework-api.en.md)) after the relay-ready reply,
   target records a Warning and performs the same CAS. The first cutover submit's success
   or failure terminal is the completion baseline; submit success or failure changes
   neither source restoration nor target
   completion. Only target performs this CAS. If the Actor belongs to a Spot,
   Actor ownership and membership are committed in the same change. If any condition
   differs, no value changes and the target queue doesn't open.
7. The target first puts existing work transferred in the payload into the real object queue, followed by work
   relayed before cutover and then the remaining temporary work. It atomically switches
   the temporary route to the regular route while application dispatch stays closed.
   Global order across different connections isn't guaranteed, but the order of these
   three spans is preserved.
8. It runs the relocation lifecycle callbacks required by that unit. A `SpotWide` User
   Spot with `ApplicationSignaled` calls `OnRelocationReadyCompleted(Relocated)` on the
   target, while host relocation invokes no membership callback. After required callbacks
   finish, it opens application dispatch. Target sends no completion reply to
   source. For a bound Actor, target runtime sends Session owner a one-way target-route
   update after dispatch opens.
9. After cutover submit reaches a success or failure terminal, source waits for no
   completion reply. It ends source execution and keeps Message Follow. If Session owner receives the exact route update within the
   default 3,000ms from seal installation, it changes route, submits held messages, and
   releases the seal. Server configuration may change `SessionRelocationSealTimeout`.
   On timeout, Session owner closes the physical Session and cleans Session state.
10. `OnClosing(RelocationOut)` is called on an instance remaining at a User Spot's or
    Instance Spot's previous location, after the Location Store's location change. The
    Entry Spot itself doesn't move, so the Entry Spot's closing callback isn't called.

Dispatch switchover must be atomic. A message arriving before the switch stays in the
temporary queue; a message arriving after the switch goes directly to the real object
queue. A state where a message being switched over ends up duplicated in both queues, or
in neither, isn't allowed.

One relocation unit's temporary queue has no bound on record count or stored size. The
framework doesn't create an additional temporary queue for the same object.

```mermaid
sequenceDiagram
    participant Source as Source runtime
    participant Dispatch as Target dispatcher
    participant TempQueue as Relocation temporary queue
    participant ObjectQueue as Object execution queue
    participant Object as Target Actor or Spot
    participant LocationStore as Location Store
    participant SessionOwner as Session owner

    Source->>Dispatch: [request] install temporary queue, Restore request · payload length, chunk count, and checksum included
    Dispatch->>TempQueue: [local] register the temporary queue before chunks arrive
    loop payload sent chunk by chunk
        Source->>Dispatch: [send] payload chunk · same ordered connection
    end
    Dispatch->>Object: [local] assemble chunks, verify checksum, then run factory and Restore application state
    Dispatch-->>Source: [reply] temporary queue and Restore ready · source still owner
    Source->>Dispatch: [send/request relay] ingress hold
    Dispatch->>TempQueue: [local] add message to the pre-boundary relay span
    alt cutover arrives within the wait time
        Source->>Dispatch: [send] cutover · pre-boundary relay sent
    else neither cutover nor retransmission within the cutover wait time after relay-ready reply
        Dispatch->>Dispatch: [local] cutover_timeout Warning · proceed by fallback
    end
    Dispatch->>LocationStore: [request] CAS processing node if source fence still matches
    LocationStore-->>Dispatch: [reply] target owner CAS result
    Dispatch->>ObjectQueue: [local] add the payload's existing work and timers
    Dispatch->>ObjectQueue: [local] move pre-boundary relay, then remaining temporary work
    Dispatch->>TempQueue: [local] remove registration, switch to regular route · dispatch closed
    Dispatch->>Object: [local] finish required relocation lifecycle callbacks
    Dispatch->>ObjectQueue: [local] open application dispatch
    opt if a bound Actor exists
        Dispatch->>SessionOwner: [send] apply exact target route, submit held, release seal
    end
    ObjectQueue->>Object: [local] process work in queue order
```

This diagram shows normal cutover and the timeout fallback. Before relay-ready is
accepted, the source keeps the ingress-hold original. An explicit Restore failure before
that boundary discards the target temporary queue without running and restores source
work. An owner-change failure afterward discards only the target temporary queue and
doesn't restore source. After owner commit, the same target process continues the remaining procedure moving the temporary
queue into the real queue. If the same Restore request arrives again, the temporary
queue and Restore aren't recreated — the existing progress state is used. Messages
aren't put into the temporary queue of a previous target attempt or a different
`ObjectGeneration`.

| Policy | Handling |
|---|---|
| `DisableRelocation` | If that object remains, `Blocked/RelocationDisabled`. |
| `RecreateOnRelocation` | Runs the target factory with the same object ID. Application state isn't moved, but not-yet-finished framework work is moved. |
| `PreserveStateWith` | Stores the bytes the adapter returned and `Restore`s them into the target factory instance. The application manages the bytes' format, version, and migration. |

The framework doesn't add a separate state contract ID or generic state type.

### 8.3 An Actor Belonging To An Entry Spot

An Entry Spot instance belongs to the Object Server lifecycle, so it isn't moved to
another node. Only Actors belonging to a source Entry Spot are moved, each as an
independent relocation unit. The target runtime restores the Actor into the Entry Spot
the target node already created at startup. Since this move isn't a join the
application requested, it doesn't call the target Entry Spot's `OnJoinedActor` or the
source Entry Spot's `OnLeaveActor`/`OnActorJoin`.

```mermaid
sequenceDiagram
    participant SourceRuntime as Source runtime
    participant SourceActor as Source Actor
    participant TargetRuntime as Target runtime
    participant TargetTemp as Actor temporary queue
    participant TargetQueue as Target Actor queue
    participant TargetEntry as Target Entry Spot
    participant TargetActor as Target Actor
    participant LocationStore as Location Store
    participant SessionOwner as Session owner

    SourceRuntime->>SourceActor: [local] hold new messages after the current turn finishes
    SourceRuntime->>SourceRuntime: [local] fix Actor state, unexecuted queue, and timers as a payload in source memory
    SourceRuntime->>TargetRuntime: [request] install Actor temporary queue, Restore request · payload length, chunk count, and checksum included
    TargetRuntime->>TargetTemp: [local] register the Actor temporary queue before chunks arrive
    loop payload sent chunk by chunk
        SourceRuntime->>TargetRuntime: [send] payload chunk · same ordered connection
    end
    TargetRuntime->>TargetActor: [local] assemble chunks, verify checksum, then run factory and Restore state
    TargetRuntime-->>SourceRuntime: [reply] Actor Restore and temporary queue ready · source still owner
    SourceRuntime->>TargetRuntime: [send/request relay] ingress hold
    TargetRuntime->>TargetTemp: [local] add message to the temporary queue
    SourceRuntime->>TargetRuntime: [send] cutover · pre-boundary relay sent
    TargetRuntime->>LocationStore: [request] CAS Actor node/membership if source fence still matches
    LocationStore-->>TargetRuntime: [reply] target node/membership CAS succeeds
    TargetRuntime->>TargetQueue: [local] add the payload's queue/timer
    TargetRuntime->>TargetQueue: [local] move temporary queue work
    TargetRuntime->>TargetTemp: [local] remove registration, switch to existing dispatch
    TargetQueue->>TargetActor: [local] process messages in queue order
    opt if a bound session exists
        TargetRuntime->>SessionOwner: [send] apply exact binding route, submit held, release seal
        Note over TargetRuntime,SessionOwner: without a response, resend the same ReqMsg at a fixed interval
    end
    Note over SourceRuntime,TargetEntry: Entry Spot's join/leave/closing callbacks aren't called
```

### 8.4 PerActor User Spot

A `PerActor` User Spot changes the node handling Spot messages and each Actor
separately. So even while some Actors are still on the source, already-moved Actors and
new Spot messages can be handled on the target.

1. The target runtime creates an empty Spot instance using the same SpotId and
   ObjectGeneration as the source. Before the Location Store's current location
   changes, this instance isn't returned as an external lookup result.
2. The source runtime finishes the current Spot handler and in-progress Actor
   Create/Join. Spot messages arriving afterward are briefly held on the source.
3. The target runtime registers a Spot relocation temporary queue. The source runtime
   keeps relaying held Spot messages and later messages arriving on the previous route
   to this queue.
4. Once the target's Spot is ready, the Location Store changes the node handling Spot
   messages from source to target. Held Spot messages move to the real Spot queue, the
   temporary queue registration is removed, and new `ToSpot`, Actor Create, and Join are
   handled via the existing dispatch path.
5. Each source member Actor finishes its current turn and then moves to the target as
   an Actor unit. `ToActor` for a not-yet-moved Actor goes to the source; for a moved
   Actor it goes to the target.
6. Once the last Actor and all messages held on the source are delivered to the target,
   `OnClosing(RelocationOut)` is called on the source Spot.

The framework doesn't create a temporary SpotId during the move. Once the Location
Store records the target as the current node handling Spot messages, the source Spot
no longer handles new `ToSpot`, Create, or Join. The source Spot only continues
handling not-yet-moved Actors and delivering messages during the move.

```mermaid
sequenceDiagram
    participant SourceRuntime as Source runtime
    participant SourceSpot as Source User Spot
    participant TargetRuntime as Target runtime
    participant TargetSpot as Target User Spot
    participant TargetSpotTemp as Target Spot temporary queue
    participant TargetSpotQueue as Target Spot queue
    participant TargetActorTemp as Target Actor temporary queue
    participant TargetActorQueue as Target Actor queue
    participant LocationStore as Location Store
    participant SourceActor as Source Actor
    participant TargetActor as Target Actor
    participant SessionOwner as Session owner

    SourceRuntime->>TargetRuntime: [request] prepare same-SpotId temporary queue, empty Spot, and relay
    TargetRuntime->>TargetSpot: [local] create a Spot not yet exposed externally
    SourceRuntime->>SourceSpot: [local] hold new messages after the current Spot handler ends
    TargetRuntime->>TargetSpotTemp: [local] register the Spot temporary queue
    TargetRuntime-->>SourceRuntime: [reply] empty Spot and temporary queue ready · source still owner
    SourceRuntime->>TargetRuntime: [send/request relay] held Spot messages
    TargetRuntime->>TargetSpotTemp: [local] hold Spot messages
    SourceRuntime->>TargetRuntime: [send] Spot cutover · pre-boundary relay sent
    TargetRuntime->>LocationStore: [request] CAS Spot processing node if source fence still matches
    LocationStore-->>TargetRuntime: [reply] target Spot owner CAS succeeds
    TargetRuntime->>TargetSpotQueue: [local] move Spot messages from the temporary queue
    TargetRuntime->>TargetSpotTemp: [local] remove registration, switch to existing dispatch
    TargetRuntime->>TargetSpot: [local] start handling ToSpot/Create/Join
    loop for each member Actor independently
        SourceRuntime->>SourceActor: [local] capture state/queue/timer after current handler ends
        SourceRuntime->>TargetRuntime: [request] install Actor temporary queue, Restore, prepare relay
        TargetRuntime->>TargetActorTemp: [local] register the Actor temporary queue
        TargetRuntime->>TargetActor: [local] run factory and Restore state
        TargetRuntime-->>SourceRuntime: [reply] Actor Restore and temporary queue ready · source still owner
        SourceRuntime->>TargetRuntime: [send/request relay] Actor messages
        TargetRuntime->>TargetActorTemp: [local] hold Actor messages
        SourceRuntime->>TargetRuntime: [send] Actor cutover · pre-boundary relay sent
        TargetRuntime->>LocationStore: [request] CAS Actor processing node if source fence still matches
        LocationStore-->>TargetRuntime: [reply] target Actor owner CAS succeeds
        TargetRuntime->>TargetActorQueue: [local] move the payload's queue/timer and temporary work
        TargetRuntime->>TargetActorTemp: [local] remove registration, switch to existing dispatch
        TargetActorQueue->>TargetActor: [local] process messages in queue order
        opt if a bound session exists
            TargetRuntime->>SessionOwner: [send] apply exact binding route, submit held, release seal
        end
    end
    SourceRuntime->>SourceSpot: [local] OnClosing(RelocationOut) after last Actor and messages
```

The empty Spot created on the target doesn't restore the source Spot's application
fields, so it doesn't call the Spot relocation adapter. Each member Actor uses its own
relocation policy and Actor adapter. Applying the Session command 44 route update doesn't
block processing of each Actor or the next Actor relocation.

### 8.5 SpotWide User Spot

A `SpotWide` User Spot moves the Spot and every member Actor, as of the moment new work
is blocked, as a single move operation. If even one Actor fails to satisfy the
relocation policy, state adapter, or target capacity condition, the Location Store
isn't changed and the whole move is aborted. The ID distinguishing this move is a
non-zero 128-bit value.

The target registers the Spot and every member Actor in the same relocation temporary
queue group. Each record preserves the actual target Spot or Actor identity. Only after
every participant's Restore and the aggregate owner change does it split saved work,
pre-boundary relay, and remaining temporary work into the real Spot and Actor queues,
then switch to the regular route. It next finishes `OnRelocationReadyCompleted` and opens
dispatch. If one participant fails, no work in the temporary queue is run and the whole
group is discarded.

There's no 1,024 cap on the total number of Actors belonging to a User Spot. The
framework splits the relocation target list across multiple Location Store pages. One
page records at most 1,024 entries, and one encoded page's size is at most 1 MiB. For
example, with 2,500 Actors, at least three pages are used. The framework confirms the
total Actor count and each page's content matches the originally stored list. Only when
everything matches does it change the node processing the User Spot and all Actors from
source to target, all at once. If a conflict occurs mid-way, only some Actors' location
isn't changed. This method — changing everything or nothing, only if the first-read
Store version is unchanged — is called
[CAS](01-glossary.en.md#compare-and-set).

```mermaid
sequenceDiagram
    participant SourceRuntime as Source runtime
    participant SourceSpot as Source User Spot
    participant TargetRuntime as Target runtime
    participant TargetTemp as Relocation temporary queue group
    participant TargetQueues as Target Spot and Actor queues
    participant TargetSpot as Target User Spot
    participant TargetObjects as Target Spot and Actors
    participant LocationStore as Location Store
    participant SessionOwner as Session owner

    SourceRuntime->>SourceSpot: [local] hold new Spot/Actor work after current handler ends
    SourceRuntime->>SourceRuntime: [local] fix all Spot/Actor state, unexecuted queue, and timers as an aggregate payload
    SourceRuntime->>TargetRuntime: [request] install aggregate temporary queue, Restore request · payload length, chunk count, and checksum included
    TargetRuntime->>TargetTemp: [local] register temporary queue group for Spot and every Actor before chunks arrive
    loop aggregate payload sent chunk by chunk
        SourceRuntime->>TargetRuntime: [send] payload chunk · same ordered connection
    end
    TargetRuntime->>TargetObjects: [local] assemble chunks, verify checksum, then create Spot and every Actor and Restore state
    TargetRuntime-->>SourceRuntime: [reply] aggregate Restore and temporary queue ready · source still owner
    SourceRuntime->>TargetRuntime: [send/request relay] Spot/Actor messages
    TargetRuntime->>TargetTemp: [local] hold target identity in pre-boundary relay span
    SourceRuntime->>TargetRuntime: [send] aggregate cutover · pre-boundary relay sent
    TargetRuntime->>LocationStore: [request] CAS all Spot/Actor owners if aggregate fence still matches
    LocationStore-->>TargetRuntime: [reply] aggregate target owner CAS succeeds
    TargetRuntime->>TargetQueues: [local] add the payload's queue and timers first
    TargetRuntime->>TargetQueues: [local] move pre-boundary relay, then remaining temporary work
    TargetRuntime->>TargetTemp: [local] remove group, switch to regular route · dispatch closed
    opt if an application-signaled boundary was used
        TargetRuntime->>TargetSpot: [local] OnRelocationReadyCompleted(Relocated)
    end
    TargetRuntime->>TargetQueues: [local] open application dispatch
    TargetQueues->>TargetObjects: [local] process messages in queue order
    loop for each bound Actor
        TargetRuntime->>SessionOwner: [send] apply exact binding route, submit held, release seal
    end
    SourceRuntime->>SourceSpot: [local] OnClosing(RelocationOut)
```

Member Actors' `OnActorJoin`, `OnJoinedActor`, and `OnLeaveActor` aren't called. Bound
Session location updates proceed per Actor after the Spot and Actors start message
processing, and one Session owner's response doesn't block processing of a different
Actor or Spot.

### 8.6 Instance Spot

An Instance Spot can't contain Actors, so one Spot is the relocation unit. Once the
source's current handler finishes, direct messages and timers are held. The target
runtime creates the Instance Spot with the same SpotId, and if it's
`PreserveStateWith`, restores the directly transferred application state via `Restore`. Once the Location
Store records the target as the current processing node, the target processes the
restored queue and timers. Since an Instance Spot has no Actor, Actor location or
Session binding isn't updated.

```mermaid
sequenceDiagram
    participant SourceRuntime as Source runtime
    participant SourceSpot as Source Instance Spot
    participant TargetRuntime as Target runtime
    participant TargetTemp as Target Spot temporary queue
    participant TargetQueue as Target Spot queue
    participant TargetSpot as Target Instance Spot
    participant LocationStore as Location Store

    SourceRuntime->>SourceSpot: [local] hold new direct messages/timers after current handler ends
    SourceRuntime->>SourceRuntime: [local] fix Spot state, unexecuted queue, and timers as a payload in source memory
    SourceRuntime->>TargetRuntime: [request] install Instance Spot temporary queue, Restore request · payload length, chunk count, and checksum included
    TargetRuntime->>TargetTemp: [local] register the Instance Spot temporary queue before chunks arrive
    loop payload sent chunk by chunk
        SourceRuntime->>TargetRuntime: [send] payload chunk · same ordered connection
    end
    TargetRuntime->>TargetSpot: [local] assemble chunks, verify checksum, then run factory and Restore state
    TargetRuntime-->>SourceRuntime: [reply] Instance Spot Restore and temporary queue ready · source still owner
    SourceRuntime->>TargetRuntime: [send/request relay] direct messages
    TargetRuntime->>TargetTemp: [local] hold messages in the temporary queue
    SourceRuntime->>TargetRuntime: [send] cutover · pre-boundary relay sent
    TargetRuntime->>LocationStore: [request] CAS Instance Spot owner if source fence still matches
    LocationStore-->>TargetRuntime: [reply] target Instance Spot owner CAS succeeds
    TargetRuntime->>TargetQueue: [local] move the payload's queue/timer and temporary work
    TargetRuntime->>TargetTemp: [local] remove registration, switch to existing dispatch
    TargetQueue->>TargetSpot: [local] process messages in queue order
    SourceRuntime->>SourceSpot: [local] OnClosing(RelocationOut)
```

Host relocation only moves an Instance Spot already existing on the source. It doesn't
start [cold activation](01-glossary.en.md#cold-activation), which creates an Instance
Spot not on the source from its first message.

### 8.7 Callbacks Not Called During The Move

Entry Spot and `PerActor` User Spot Actor relocation isn't a join or leave the
application requested. So `OnActorJoin`, `OnJoinedActor`, and `OnLeaveActor` aren't
called. The framework moves Actor state, not-yet-executed queue, and timers, and
changes the current processing node.

A `SpotWide` User Spot also only changes the processing node without changing which
Spot each Actor belongs to, so member Actors' join/joined/leave callbacks aren't
called. If `ApplicationSignaled` was used, only `OnRelocationReadyCompleted(Relocated)`
is called on the target after the regular-route switch and immediately before dispatch opens.

`OnClosing(RelocationOut)` is called on a User Spot's or Instance Spot's source instance
after the Location Store's location change. Since an Entry Spot instance doesn't move,
the Entry Spot's closing callback isn't called. An Instance Spot has no Actor, so there's
no Actor lifecycle callback at all.

A cross-node Actor join uses the same policy and adapter, but the exact lifecycle is
owned by [23 Spot Actor](15-spot-actor.en.md). A same-node join doesn't call the
adapter. Instance Spot maintenance relocation doesn't newly create an Instance Spot not
present on the source.

### 8.8 Which Location Is Kept On A Mid-Way Failure

The sequence diagrams above show only the normal path where the location change
succeeds. If the target explicitly fails before the relay-ready reply is accepted, the
source keeps processing messages. The framework doesn't expose the target instance,
discards the temporary queue, and restores source messages and timers to the original
queue. The target doesn't create a request terminal result or run a one-way message from
the temporary queue.

After the relay-ready reply is accepted, source dispatch doesn't reopen while Location
Store still points to source, even if cutover hasn't been sent yet or its submit fails.
The target continues CAS after receiving cutover (including a retransmission after
connection re-establishment) or through the cutover-wait fallback. If target CAS ultimately fails, target removes its object and queue, the
Session cleans under its own seal timeout, and source Message Follow ends after its
defined duration.

Once the Location Store records the target as the current processing node, it isn't
rolled back to the source. If the target runtime is still running, a failed stage can be
retried. Location Store update retries until Restore validity expires; if target
ownership isn't confirmed by then, the prepared Actor or Spot and queue are removed and
the Session route isn't updated. If the source or target process terminates, a different
runtime doesn't take over this relocation. If the target terminates after commit, it
isn't rolled back to the source — that object is left unavailable. Automatic recovery
afterward isn't part of the contract. The source sends one-way cutover, waits for no
completion reply, and changes to Message Follow. After CAS and queue opening, the target
sends the Session route update one-way. This choice doesn't guarantee
exactly-once behavior or global ordering across a process-crash window. The exact
failure result the application observes is defined by
[§10 Relocate Completion And Failure](#10-relocate-completion-and-failure).

## 9. Moving Pending Messages, Timers, And Sessions

The number distinguishing whether an object under the same ID was deleted and
re-created is called [ObjectGeneration](01-glossary.en.md#objectgeneration). The value
distinguishing one operation, to avoid processing a message or request twice, is
[operation identity](01-glossary.en.md#operation-identity).
The window during which the source temporarily holds new messages during a move is
called [relocation ingress hold](01-glossary.en.md#relocation-ingress-hold).

| Resource | Move rule |
|---|---|
| A message arriving after new work is blocked | The source holds arriving messages with no bound on record count or stored size. If the owner change succeeds, operation identity and ObjectGeneration are kept and delivered to the target. On an explicit cancellation before the relay-ready reply is accepted, it's restored to the source queue in arrival order; afterward it isn't restored to source. |
| `SpotWide`/Instance Spot timer | The runtime handle and continuation aren't moved. Logical registration, next fire time, and pending tick are moved, and the target automatically restores them in queue order. The application doesn't duplicate-capture a timer or re-register it in restore. |
| Entry/`PerActor` Actor timer | Moves with the Actor queue to the Actor owner. Spot-level application timers aren't moved — a schedule that must be kept is managed in the application's external state. |
| A session connected to an Actor | The physical connection of a [STREAM session](01-glossary.en.md#stream-session), which exchanges request/reply and push on the same connection, is kept. Before relocation starts, the Session owner seals that binding and holds Session messages. After target preparation and owner CAS complete, it changes the [binding route](01-glossary.en.md#binding-route) and the bound-session's current Actor location snapshot to the target MeshName/NodeRid, then releases the seal. The Session neither selects the target nor changes the Location Store. |

Operation identity and authority generation are also kept when delivering a
late-arriving message to the target via the previous owner. Independently of Session
command 44 application, a Message Follow route only delivers packets arriving on the
previous route to the target Actor within `MessageFollowDuration`. Packets and replies
of a previous generation are rejected. A newly created Actor under the same ActorId
must be rebound by the application. The detailed route-change order is defined by
[31 Session-Actor Dispatch](20-session-actor-dispatch.en.md#5-actor-relocation-route-barrier).

An Instance Spot's `Close` and relocation are ordered within the same authority commit.
If `Closing` comes first, close finishes and it isn't moved. If relocation comes first,
a late `Close` is a moving result and isn't automatically resubmitted.

## 10. Relocate Completion And Failure

Once every unit is detached from source dispatch and the one-way cutover submit attempt
for each target that sent a relay-ready reply reaches a success or failure terminal, the
host transitions to `Relocated` and returns `Relocated/None`.
This result is not confirmation that target Location Store CAS completed. Descriptor
lease, listener, peer connection, and raw transport resources aren't cleaned up at this
point.

| Completion point | Observer | Meaning |
|---|---|---|
| Restore and relay-ready reply | Source unit | The target temporary queue and Restore are ready, and the source is still the owner. |
| One-way cutover submit terminal | Source unit | Source attempted cutover once after pre-boundary relay and obtained a success or failure terminal. Neither result confirms target CAS. |
| `Relocated/None` reply | Source host and caller | Every source unit dispatch has ended and every cutover submit attempt reached a terminal result. Submit success isn't a completion condition. |
| Successful Location Store CAS | Target unit | The target is the owner and may open the transferred existing queue and the relay queue in order. |
| Applied Session route update | Session owner | The exact binding route changed to the target, held messages were submitted, and the seal was removed. |

The target sends neither a cutover reply nor a Session route-update reply. The source Host does
not create an acknowledgement journal or numeric high-water to wait for target CAS or Session
route application.

Because cutover can be lost to a connection failure, the source keeps a copy of each unit's
pre-boundary relay batch and cutover for the same duration as the cutover wait time
(`RelocationCutoverWaitTimeout`) after the first cutover submit terminal. That duration is the
unit's cutover retransmission window. If the connection to the target is re-established within
the window, the source resends the batch and cutover over the new connection, and the target
discards its partially received pre-boundary relay span and atomically replaces it with the
whole retransmitted batch — a full replacement, not per-message deduplication or partial
merging, so the span's order is fixed by batch order. Retransmission resends one batch; it's
no per-message ACK or journal. The copy is source-memory retention that occupies no pipe, and
when the window ends the source cleans it exactly once and never retransmits afterward. The
retransmission window changes none of the completion points in the table above — the host
still transitions to `Relocated` at the first cutover submit terminal, and retransmission is a
recovery action after it. Once the source process has been cleaned up or terminated,
retransmission is impossible and the target proceeds through the cutover-wait fallback. The
per-unit retransmission and replacement rules are owned by
[Complete Actor And Spot Relocation Flow](28-relocation-flow.en.md).

The result when an operation doesn't satisfy its completion condition by the deadline
is called [`DeadlineExceeded`](01-glossary.en.md#deadlineexceeded).

| Timing and cause | Result |
|---|---|
| No target candidate satisfying the requested application version and registered factory/type eligibility is ready by the deadline. | `Blocked/TargetUnavailable` |
| Store read, write, or owner lease check fails before relay-ready reply acceptance. | Cleans up temporary records without changing owner and returns `Blocked/StoreUnavailable` |
| A `DisableRelocation` policy remains. | `Blocked/RelocationDisabled` |
| After target selection, the transferred state schema/type adapter is incompatible, or `Capture` and `Restore` both fail across every allowed retry. | `Blocked/StateIncompatible` |
| Before relay-ready reply acceptance, the framework cancels a callback due to the deadline or work exceeds the deadline. | `Blocked/DeadlineExceeded` |
| Target explicitly rejects Restore before relay-ready reply, so the source queue can be restored. | Restores source workload that hasn't attempted cutover and returns `Blocked/RelocationFailed` |

An explicit failure before relay-ready is accepted cleans temporary records and lets
that source authority and queue accept new work again. A unit that crossed this boundary
doesn't roll back to source regardless of cutover-submit success or failure. Only source
workload that hasn't crossed the boundary may be reprocessed before the host transitions
to `Serving`.

A target CAS, queue opening, or Session route-update failure after relay-ready is accepted is not a Host
result delivered synchronously to source. The target retries CAS until the Restore
validity deadline. If it cannot confirm target ownership, it removes the prepared unit
and records an Error log. It doesn't change an already returned `Relocated` result or reopen
source dispatch.

If the directly transferred payload's checksum differs from the assembly result, the
target doesn't restore from a partial assembly and answers with an explicit failure
before the relay-ready reply, and the source restores its queue from the payload kept in
memory — the same path as `Blocked/RelocationFailed` in the table above. If the
`SpotWide` relocation target list's content checksum differs from the originally stored
list, it's an unrecoverable `DataLost` even on retry. It doesn't guess a previous list or
roll back to the source.

If some MeshNodes' `Relocating` descriptor write result can't be confirmed, every
attempted descriptor is rolled back to `Serving`. Only once every rollback is confirmed
is it `Blocked/StoreUnavailable`. If even one can't be confirmed, no new work is
accepted, cleanup proceeds for a fixed maximum time, and it ends as
`ForceStopped/TeardownFailed`.

## 11. The Race Between Shutdown And Relocate

`Shutdown` isn't blocked by an absent target, policy, capacity, or Relocation Store.
The action that changes state to stop accepting new application work is called
[admission seal](01-glossary.en.md#admission-seal). Shutdown first applies an admission
seal to the whole host. It doesn't guarantee stateful workload continuity and completes
within a fixed time, in this order.

1. Changes the host to `Draining` and closes new application admission and the start of
   new relocation units.
2. Publishes a `Draining` descriptor, excluding it from new selection and placement.
3. Processes already-accepted handlers, request completions, relocation units, and
   session barriers up to the deadline.
4. Doesn't start new object relocation. While Actor membership and local instances
   remain valid, delivers a `HostShutdown` closing context to every Entry, User, and
   Instance Spot. Per-Actor closing callbacks aren't called.
5. After Spot callbacks, cleans up local Actor and Spot scope, owner record, descriptor,
   listener, and transport, in order.
6. If finished within the deadline, ends with `Stopped/None`; if not, ends with
   `ForceStopped/DeadlineExceeded` or `ForceStopped/TeardownFailed` after bounded
   teardown.

When a listener and transport are cleaned up, already-accepted transport
callbacks and in-flight read/write operations are completed or cancelled first.
In particular, TLS/WebSocket resources and a per-connection write queue are
not destroyed until cancellation completion has been observed on the owning
transport execution context. A late callback accessing a destroyed resource or
turning an accepted operation terminal twice is not a valid bounded teardown.

| Operation confirmed first | Handling |
|---|---|
| `Shutdown`'s admission seal | Returns capacity secured on the target, and ends a pending Relocate call with `Blocked/ShutdownRequested`. |
| `Relocating` publication | Only confirms the current unit to a terminal state and doesn't start the rest. Preserves published authority; the waiter gets `Blocked/ShutdownRequested`. |

`Shutdown` in `Relocated` only cleans up accepted work and infrastructure. Calling it
directly from `Serving` doesn't move objects.

While `Relocated`, the source retains Message Follow routes and the descriptors, peer
connections, and listeners required to forward sends and requests that still arrive at the old
address, and also keeps each unit's cutover retransmission copy until its retransmission
window (§10) ends. A deployment that needs the full configured `MessageFollowDuration` calls
`Shutdown` after that period. Calling `Shutdown` earlier removes the remaining Message Follow
routes and retransmission copies with the source transport.

For a relocation operation it started, the source runtime publishes `SafeToShutdown` — the
observation value saying it's safe to shut down — into its own runtime status after every
unit has reached the moment its Message Follow route may be removed (S4, §7.1) and every
unit's retransmission window (§10) has ended. Both conditions are events happening on the
source, so no other node's clock is needed for this judgment. The value is not a completion
ACK sent by the target or anyone else — the source publishes it and other parties observe
it; the exact status surface is owned by
[50 Runtime Monitoring](24-runtime-monitoring.en.md). A deployment orchestrator can confirm
it via status query and change observation before calling `Shutdown`. Calling `Shutdown`
before publication is also allowed, but as the first paragraph of this section says, the
remaining Message Follow routes and retransmission copies disappear with it — a request from
a sender that still caches the previous route can end with `Unavailable`.

Descriptor and owner lease keep renewing during `Draining`. To avoid losing owner
eligibility before already-accepted requests, relocation, and session route changes
finish, lease use ends only after all work finishes. The cleanup order is as follows.

Information a fanout publisher publishes to the Store — its endpoint, identity, and run
generation — is called a
[fanout publisher descriptor](01-glossary.en.md#fanout-publisher-descriptor).

1. While keeping Actor membership and local instances, finishes the Spot closing
   callback and cleans up local scope.
2. Only the source holding current authority changes or removes owner and relocation
   target records to the next state.
3. Releases the MeshNode, ClientServer server, and fanout publisher descriptor and
   owner lease.
4. Closes peer connections, listeners, executors, and binding transport.

A language supporting standard cooperative cancellation passes a cleanup cancellation
signal representing the remaining deadline to the Spot closing callback. An
already-accepted handler's token isn't reused.
A callback exception is `ForceStopped/TeardownFailed`; deadline expiry is
`ForceStopped/DeadlineExceeded`. Callback execution isn't guaranteed on hardware
failure or `SIGKILL`. It doesn't guarantee that a different runtime automatically takes
over an interrupted relocation or cleanup.

## 12. Admission Per State

The way the framework picks one Server candidate among several under the same
ChannelName is called [select-one](01-glossary.en.md#select-one). A call where the
caller directly specifies a node RID is [Node direct](01-glossary.en.md#node-direct). A
feature that sends a message to several Spots participating in the same Channel is
[Logical Multicast](01-glossary.en.md#logical-multicast).

The path from the source runtime to the current owner is called an
[owner route](01-glossary.en.md#owner-route).

| Public feature | `Relocating` | `Relocated` | `Draining` |
|---|---|---|---|
| ChannelName select-one | Excluded from new selection, but existing direct owner routes are kept. | Excluded from new selection; infrastructure connections are kept. | Closes new admission; only already-submitted operations process to a terminal state. |
| Logical Multicast | Excluded from the new target snapshot; already-accepted submissions are kept. | Excluded from the new target snapshot. | Closes new admission; only already-accepted submissions are processed. |
| Node direct application request | An existing owner's requests are accepted until unit seal. | No local stateful owner, so new requests aren't accepted. | New requests end with a shutdown result. |
| Node direct infrastructure control | Keeps accepting relocation, completion, binding, and recovery control. | Keeps accepting monitoring and shutdown control. | Only accepts the control needed for the termination barrier, up to the deadline. |
| Spot/Actor direct | Keeps processing payload and timers until unit seal. | No local stateful owner, so new payload isn't accepted. | Rejects new payload; only already-accepted turns are processed. |
| Spot/Actor create and join | Rejects new owner and membership admission. | Keeps the same rejection. | Keeps the same rejection. |
| Instance Spot placement | Excluded from new target claims; existing direct routes are kept until seal. | Excluded from new target claims. | Only activations accepted before seal are processed to a terminal state. |
| STREAM | Excluded from new binding; existing sessions are handled via unit barrier. | Excluded from new binding; infrastructure connection is kept. | Doesn't accept new sessions; only pending reply and binding barrier are processed. |
| ClientServer server | Excluded from new selection; accepted handlers and reply routes are kept. | Service connection is kept but excluded from new selection. | Closes handler admission; only accepted requests' reply routes are kept. |
| Classic fanout publisher | Doesn't create new automatic subscriber connections; processes accepted events. | Infrastructure is kept but new publish admission isn't accepted. | Closes publish admission; only accepted events are processed. |

An already-accepted request only ends once, via reply, error, timeout, or shutdown.
Even while an application callback waits, infrastructure execution keeps proceeding
with request completion, peer lifecycle, recovery, and session binding. An observer or
monitoring callback doesn't own a claim that blocks maintenance.

## 13. Observability Information

State and relocation result changes are observed via
`zlink.runtime.host.relocation_changed`; shutdown result changes via
`zlink.runtime.host.termination_changed`. Terminal events aren't lost to observer
overflow. Relocation events and a limited set of diagnostic states include mode and
effective target version. Version isn't added as a metric label.

Host state and terminal results are checked in host status and structured logs. When
aggregation is needed, host state, relocation mode/outcome/reason, and shutdown
outcome/reason are recorded on the instruments defined by
[51 Runtime Metrics](25-runtime-metrics.en.md#5-host-relocation-and-shutdown). Object
relocation instruments and host-wide operation instruments use different names.

The point definitions and measuring parties for the per-unit points S0–S4 and the three
interval metrics (source stop S0→S1, target resume S2→S3, route convergence S1→S4) are
fixed by §7.1. The status surface of the `SafeToShutdown` observation value (§11) is owned
by [50 Runtime Monitoring](24-runtime-monitoring.en.md); the interval-metric instruments
and the `cutover_timeout` counter, which counts cutover-wait fallbacks, are owned by
[51 Runtime Metrics](25-runtime-metrics.en.md).

The global string address for finding a Spot system-wide is called a
[Spot ID](01-glossary.en.md#spot-id). Metric labels don't include Actor ID, Spot ID,
node RID, endpoint, session ID, or relocation ID. Individual blocker and relocation
state is checked via count-limited diagnostic queries and traces. Telemetry provider
failure doesn't block operation progress. The full observability contract is owned by
[50 Runtime Monitoring](24-runtime-monitoring.en.md) and
[51 Runtime Metrics](25-runtime-metrics.en.md).

## 14. Contract Test Verification Requirements

| Scope | Item that must be verified |
|---|---|
| Mode and target | Verify that planned maintenance only selects the same version, and rolling update only selects the requested higher exact version. Version must apply before capacity and weight, exclude the same wave, and only proceed once the exact Core peer is ready on every Mesh. It must wait when there's no target, and block on manual topology. |
| Lifecycle | Verify that a blocked preflight keeps `Serving`, and success becomes `Relocated` with infrastructure kept. `Shutdown` is called separately, with a default deadline of 30 seconds. Caller cancellation must only end the waiter, and mustn't change admission in an invalid runtime state. |
| Concurrency | Verify that relocation and concurrent shutdown with the same option each share one operation. A different relocation option ends with `OperationInProgress`; shutdown during relocation ends with `ShutdownRequested`; and repeated calls must return the same terminal result. |
| Inventory and batches | Verify that Entry Spot Actors, `PerActor` shells and member Actors, `SpotWide` aggregates, Instance Spots, and standalone Actors each appear once while the Entry Spot instance is excluded. Enforce the `PerActor` shell, Actor, and aggregate batch order and its dependencies, and start only independent units in the same batch concurrently. Do not require relocation-specific limits on unit count, participant count, or relay records; reaching the in-flight payload budget must appear only as waiting before the seal and must not fail a unit that already started. |
| Handoff | Verify that a `SpotWide` User Spot aggregate commits at once and moves queue, timer, and pending tick together. The target dispatcher must register the Spot and every member Actor in the same relocation temporary queue group while preserving each record's actual target. After every Restore and aggregate commit, saved work, pre-boundary relay, and remaining temporary work must enter each real queue in order before the regular-route switch. `OnRelocationReadyCompleted` then finishes before dispatch opens, and no participant application work may run before that opening. A Message Follow route must be removed after `MessageFollowDuration` independently of command 44 application. An Instance Spot must not be secretly re-created. |
| PerActor handoff | Verify that Entry Spot and `PerActor` User Spot move only Actors independently, without calling a Spot adapter or membership callback. After the Spot authority transition, `ToSpot`/Create/Join must use the target, and `ToActor` must use each Actor's current owner. Spot and Actor relocation temporary queues must be registered independently. The order of transferred existing work, temporary work, and post-switchover direct work must be preserved, and resending the same relocation request must not create the temporary queue and Restore twice. |
| Interruption target | Measure a source-local 1 second, for each of Actor, Instance Spot, `SpotWide` User Spot, and `PerActor` Spot direct message, from when the source blocks new work through the one-way cutover submit's success or failure terminal. Don't create a target processing-start acknowledgement or use exceeding this as a failure, rollback, or retry condition. After the host deadline, don't start a new unit, and process an already-started unit to a safe terminal state. |
| Pacing and retransmission | Verify that when the in-flight payload budget is full, a new unit waits before its source admission seal and the waiting Actor/Spot keeps processing messages meanwhile. The coordinator must not set a separate cap on concurrent unit count. When the connection is re-established within the retransmission window, the source must resend the boundary batch and cutover and the target must replace its partially received staging with the whole retransmitted batch; once the window ends, the copy must be cleaned exactly once and no retransmission may occur afterward. |
| Metrics and SafeToShutdown | Verify that source stop time (S0→S1), target resume time (S2→S3), and route convergence time (S1→S4) are measured on the clock of the node where each point happens, and no metric directly subtracts timestamps of different nodes. `SafeToShutdown` must not be published before every unit reaches S4 and every unit's retransmission window ends, and neither judgment may use another node's clock. `Shutdown` before publication is allowed and its result must match §11's route and copy cleanup. |
| Failure | Only on an explicit abort before relay-ready is accepted must the target temporary queue be discarded without running and the source original restored to the queue. After that boundary, source isn't restored regardless of cutover-submit result. A request's terminal result must not be duplicated across two runtimes. If the same target runtime fails after owner commit, don't roll back to the source or automatically pick a different target. Return the exact `Blocked` reason, complete the terminal result exactly once, and perform bounded teardown if descriptor rollback can't be confirmed. Automatic relocation resumption after process termination isn't a verification target. |
| Cleanup and observability | Verify that lease keeps renewing until the barrier finishes, and an accepted request completes exactly once. Callback failure must be classified with a defined reason, and state, outcome, reason, event, and metric must match the wire values. Topology cleanup must not change a different authority. |
