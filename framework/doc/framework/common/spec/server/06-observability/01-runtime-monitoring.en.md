---
title: "Runtime Status Query and Operational Diagnostics"
---

# Runtime Status Query and Operational Diagnostics

[Observability topic table of contents](README.en.md) · [Spec table of contents](../README.en.md) · [Next: 02. Runtime Metrics](02-runtime-metrics.en.md)

> Defines the public contract for the complete host/topology status at a
> specific point in time, the status-change stream, and structured-log
> identifiers. The ownership boundary with other documents in this topic
> follows the [Observability responsibility map](README.en.md).

## 1. Runtime Status Query Overview

An application operator queries the framework runtime's current status
once, observes subsequent changes, and uses logs to determine why a status
changed.
The application uses this information to judge whether the runtime can accept new
work, the scope of a failure, and relocation/shutdown results.

This document owns the complete status at a specific point in time, the
status-change stream, and structured-log identifiers. The name/unit/label
of numbers accumulated or collected over time is owned by
[Runtime Metrics](02-runtime-metrics.en.md); the progress record of one
message is owned by
[Message Flow Tracing](03-message-flow-tracing.en.md); the state
transitions of relocation and shutdown are owned by
[Host Relocation And Shutdown](../05-location-relocation/05-host-relocation-flow.en.md). See the
[topic README](README.en.md) for the complete ownership map.

## 2. Roles and Responsibilities · Values Not Exposed Publicly

| Party | Responsibility |
|---|---|
| Application | Queries and observes status by registered name, and configures the logger provider and backend. |
| Framework | Combines internal service values into a complete status and records standard identifiers for state changes. |
| Provider | Delivers logs to the logger backend the application chose. Ensures a provider failure doesn't change the runtime result. |
| Remote runtime | Publishes its own service availability and operational state. The current runtime reflects this in topology status. |

The [descriptor revision](../00-foundation/02-glossary.en.md#descriptor-revision),
indicating the order of changes to remote registration information, and
the [owner lease](../00-foundation/02-glossary.en.md#owner-lease), indicating whether a
host can keep using its current lifecycle's ownership, are used only for
internal judgment. The public interface doesn't expose these two values,
the internal state of work acceptance, claims, capacity reservation, socket
state, exporters, storage, raw event DTOs, or native handles.

## 3. Host State — Values Read at Once

The application reads per-feature status by the name it registered at
startup. It doesn't directly combine values from several internal
services.

The registration names introduced here are as follows.

- The runtime unit that provides RouteMesh peer connections and Channel
  messaging within one process is called a
  [MeshNode](../00-foundation/02-glossary.en.md#meshnode). The logical runtime where
  multiple MeshNodes share the same messaging rules is called a
  [RouteMesh](../00-foundation/02-glossary.en.md#routemesh), and the startup registration
  name identifying one RouteMesh is called a
  [MeshName](../00-foundation/02-glossary.en.md#meshname).
- The startup registration name identifying one Channel is called a
  [ChannelName](../00-foundation/02-glossary.en.md#channelname).
- The topology where Client and Server exchange requests and replies under
  a ChannelName is called a
  [ClientServer Channel](../00-foundation/02-glossary.en.md#clientserver-channel).
- The logical execution unit with an address and state that receives
  messages is called a [Spot](../00-foundation/02-glossary.en.md#spot).
- The state in which every per-feature serving condition is met, allowing
  application messages to be received, is called
  [Ready](../00-foundation/02-glossary.en.md#ready).

| State scope | Values checked in one status |
|---|---|
| Host | Runtime state, ready status, new-work acceptance, deadline, relocation/shutdown result, and inbound dispatch state |
| RouteMesh | `MeshName`, overall state, ready peer count, per-Channel ready target count, per-peer operational state, and the current process's Actor/Spot count |
| ClientServer | `ChannelName`, local role, overall state, ready target count, and per-target operational state/weight |
| Automatic fanout | `ChannelName`, overall state, number of publishers being connected to, and ready publisher count |

**Status is an immutable value that can be kept after the call ends.** It
doesn't reference a native handle, caller buffer, payload, or application
metadata, so the caller holding onto the status doesn't pin runtime-internal
resources.

The following C# is a non-normative excerpt showing the common behavior. It
doesn't require the same signature in other languages. The precise type and
signature are set by
[.NET Topology Monitoring](../languages/dotnet/interfaces/10-topology-monitoring.en.md).

```csharp
public interface IZLinkRouteMeshRuntime
{
    ZLinkRouteMeshStatus GetStatus(string meshName); // reads the current state of the registered RouteMesh.

    IAsyncEnumerable<ZLinkObservedStatus<ZLinkRouteMeshStatus>> ObserveAsync(
        string meshName,
        CancellationToken cancellationToken = default); // receives subsequent complete states and the loss total, in order.
}
```

Host state doesn't belong to a specific `MeshName`. The final results of
relocation and shutdown are also provided just once, in host status.

Host runtime state is closed to the following values. A value not in this
table must not be added. The procedure of blocking new work and cleaning
up already-accepted work within a time limit is called
[drain](../00-foundation/02-glossary.en.md#drain). That time limit is called a
[deadline](../00-foundation/02-glossary.en.md#deadline).

| Value | Meaning the application observes |
|---|---|
| `preparing` | Validating startup configuration and preparing the runtime. |
| `serving` | Can accept new application operations. |
| `relocating` | Stopped accepting new work and is moving stateful objects to another node. |
| `relocated` | Relocation finished; infrastructure and connections are kept. |
| `draining` | Cleaning up remaining processing and resources without relocation. |
| `stopped` | Runtime and infrastructure cleanup are finished. |
| `error` | An error occurred that prevents continuing to operate the runtime. |

**`IsReady` is `true` only when `State` is `serving`.** `AcceptingWork` is a
separate value indicating whether the current host accepts new application
operations, and the two values aren't reinterpreted as conditions that
substitute for each other. The precise meaning of relocation option,
deadline, and result is set by
[Host Relocation And Shutdown](../05-location-relocation/05-host-relocation-flow.en.md).

Host status also provides the
[`SafeToShutdown`](../00-foundation/02-glossary.en.md#safe-to-shutdown) observation value,
indicating when the source runtime can be shut down safely after
relocation.

- **For a relocation operation it started, the source runtime publishes
  `SafeToShutdown` to its own host status only after every relocation unit
  has reached the point where its
  [Message Follow](../00-foundation/02-glossary.en.md#message-follow) route
  — the routing that keeps delivering messages arriving at the previous
  owner to the new owner after relocation — can be removed (based on
  follow-duration expiry), and each unit's cutover retransmission window
  has ended.** Both conditions are events occurring on the source, so this
  judgment uses no other node's clock.
- **This value isn't a completion ACK sent by the target or any other
  party — the source publishes it.** A deployment orchestrator confirms it
  through the status query and change observation in
  [§6](#6-observing-state-changes--sequence-and-the-complete-status).
- **Calling [`Shutdown`](../00-foundation/02-glossary.en.md#shutdown) — which
  puts the runtime into termination and stops it from admitting new
  operations — before it is published is still allowed.** In that case the
  remaining Message Follow routes disappear with the transport, and a
  request from a sender still caching the previous route can end with
  `Unavailable`.

The definitions of Message Follow and the cutover retransmission window are
owned by
[Complete Actor And Spot Relocation Flow](../05-location-relocation/04-relocation-flow.en.md).

## 4. The Capacity Fields of Host Status

The capacity fields in host status coherently read the Core HWM snapshot and the
[Application job queue](../00-foundation/02-glossary.en.md#application-job-queue)
snapshot — the shared supply-permit queue a host holds before an
application callback starts — from one measurement epoch. It doesn't walk
queues to build a snapshot.

The Core HWM snapshot lets you observe: configured memory limit, manual
budget, and profile; effective budget; total applied HWM; the core
queue's current, provisional, and peak accounted bytes; completion
current, peak, and pending; total messaging, monitor-queue
applied/accounted, and total-instance applied/accounted bytes; blocked
ratio; and active ordinary/completion/send/receive queue counts. The four
fields `application accounted bytes`, `outstanding application lease`,
`retired queue`, and `deferred origin credit` are reserved fields kept for
ABI compatibility and have always been `0` since version 0.13.1. This
doesn't mean an
application byte HWM or lease exists. The framework projects the Core
runtime snapshot unchanged and neither recomputes it nor assigns it a
different meaning.

Under this Core meaning, DEALER-ROUTER reply bytes are included in
`core_queue_accounted_bytes`, current, provisional, peak, and total messaging,
and are excluded from Completion current, peak, pending, and direction count.
Only reply bytes on a ROUTER-ROUTER [Completion connection](../00-foundation/02-glossary.en.md#completion-connection) are included in the
Completion fields. Framework status uses the topology-specific classification,
field names, snapshot layout, and ABI version from the Core runtime snapshot.

The application job queue snapshot lets you observe: configured
profile/manual max; configured pause/resume percent; effective processor
count/effective max; computed pause/resume permit count; reserved supply
permits; queued application jobs; permits in use/peak; the `running|paused`
pressure state; current pause duration; and capacity waiter/wait
count/duration. Reset keeps configuration, the pressure state, and current
pause duration, and advances the measurement epoch, while pressure
transition count, cumulative pause duration, and flow-state config failure
count are set to `0`. A concurrent event is included in exactly one epoch —
the previous or the new epoch — and peak can't be smaller than current. The
ownership of the measurement epoch and the instrument that continuously
tracks this snapshot's values is defined by
[Runtime Metrics §3](02-runtime-metrics.en.md#3-host-core-hwm-and-application-job-queue);
this document only covers what can be queried at one point in time.

This status doesn't include payload, Actor ID,
[Spot ID](../00-foundation/02-glossary.en.md#spot-id) — a Spot's global
logical address — session ID, RID, endpoint, message type, or a per-owner
list. A per-owner top-N isn't part of the public contract.

## 5. Topology State — RouteMesh, ClientServer, Automatic Fanout

Topology state has a different scope from host state. Host state is
the process-wide startup, relocation, and shutdown progress. Topology
state indicates whether one RouteMesh/ClientServer/automatic fanout
registered under a `MeshName` or `ChannelName` can currently process
application messages.

So even if the host is `serving`, if a specific ClientServer Channel has no
ready target, only that topology can be `degraded`. Conversely, if the
host is `relocating`, `relocated`, or `draining`, every topology's
`IsReady` is `false` even if connections remain. At this point, the
connected peer/target counts still reflect the actual current connection
state — the counts aren't changed to `0` just because the host isn't
accepting application traffic.

| State kind | Allowed values |
|---|---|
| Topology state | `starting`, `ready`, `degraded`, `stopping`, `stopped`, `failed` |
| Topology reason | `runtime_not_ready`, `no_ready_peer`, `no_ready_target`, `location_unavailable`, `capacity_exceeded`, `draining`, `internal_failure` |
| Peer state | `connecting`, `ready`, `draining`, `not_connected`, `not_required` |
| ClientServer local role | `client`, `server`, `client_and_server` |

| Topology state | Meaning |
|---|---|
| `starting` | Preparing that topology's listener, connections, and registration. |
| `ready` | The host is `serving` and that topology can process application messages. |
| `degraded` | Some peers/targets or the Location Store are unavailable, so that topology can't fully provide its functionality. |
| `stopping` | Cleaning up that topology's already-accepted work and connections due to host shutdown. |
| `stopped` | That topology's work and connection cleanup is finished. |
| `failed` | An error occurred that prevents continuing to operate that topology. |

A RouteMesh peer provides a node's transport identity,
[Routing ID](../00-foundation/02-glossary.en.md#routing-id), as the Node RID value.
Endpoint, descriptor revision, and connection generation aren't provided.

Peer state distinguishes two cases where there's no connection.

| Peer state | Meaning | Ready/failure aggregation |
|---|---|---|
| `not_connected` | The topology requires a connection but there's currently no ready connection. | Excluded from ready peer count. Reflected in topology degraded status and liveness/health failure aggregation. |
| `not_required` | Both MeshNodes are Object Client and neither has RouteMesh Channel Server membership, so no connection is needed. Automatic excludes it at the descriptor-check stage; Manual confirms it at handshake. | Excluded from ready peer count. Also not a target for liveness probe/reconnect/health failure aggregation. |

A `not_required` peer is still left in the status's peer list. This lets an
operator distinguish a normal connection omission from a connection
failure. This state alone doesn't cause a RouteMesh to become `degraded`.

RouteMesh placement state provides whether new objects are accepted and
the current active Actor/Spot count. Status separately provides the count
of Spots and the Actors processing application messages within them. The
per-type capacity reservation registered at startup, the
[activation barrier](../00-foundation/02-glossary.en.md#activation-barrier) blocking
first-message delivery before Spot initialization finishes, and internal
capacity counters, aren't provided.

**Placement's `IsAvailable` is `true` only when the host is `serving` and is
an Object Server, placement weight is positive, and there's headroom in
both Actor/Spot capacity and activation concurrency.** Activation
concurrency's current value and limit aren't exposed as a separate field
in public status.

The [weight](../00-foundation/02-glossary.en.md#weight) used for the new-target selection
ratio is a signed integer `0..10000`. A value of `0` excludes it from being
chosen as a new placement target.

A ClientServer Server in the same process is also a candidate on equal
footing with a remote Server. Status provides target count and each
target's state/weight. `client_and_server` means both roles are registered
under the same `ChannelName` — it isn't a separate registration role.

**The ready judgment of an automatic fanout publisher is owned by
[transport liveness §4](../02-channel-transport/05-transport-liveness.en.md#4-classic-fanout),
and monitoring status shows its result.** That ready starts per publisher with
the first application record or
[liveness beacon](../00-foundation/02-glossary.en.md#liveness-beacon) received
and ends with a disconnect or 15 seconds without a record is that document's
rule. A connection plan or `connect` acceptance alone doesn't make it ready.

## 6. Observing State Changes — Sequence and the Complete Status

Each language provides a current-status query and an async change
observation. Names and types are set by each language's interface.

```csharp
var current = routeMeshRuntime.GetStatus("game-mesh");
// readiness and target state are contained in one value built at the same point in time.

await foreach (var observed in routeMeshRuntime.ObserveAsync("game-mesh", cancellationToken))
{
    await RecordStatusAsync(observed.Status, cancellationToken);
    // observed.Loss is the count this observer has missed so far.
    // observation code doesn't change routing or lifecycle decisions.
}
```

Status includes a `Sequence` that monotonically increases within the
runtime instance, and an observation time. Within the same source, a
larger `Sequence` is a later state. Values from different sources aren't
compared. `Sequence` can restart from 0 when the process restarts.

**Each item in the change stream is a complete status, not an event
carrying only some fields.** A general-purpose event DTO combining
nullable fields isn't provided. If an observer notices a `Sequence` gap, it
re-queries the current status to restore every field.

## 7. When the Observer Is Slow — Source, Coalescing, and the Lost-Update Count

### 7.1 Definition of a Source

The unit of coalescing, a **source**, equals **the thing that owns a
`Sequence`**. Since each status item carries one `Sequence`, the entity
issuing that `Sequence` is the source.

| Stream kind | Source | Source key |
|---|---|---|
| Host status | This one runtime instance | The runtime instance ID. One for the process's lifetime |
| Topology status | One topology runtime | RouteMesh uses `MeshName`; ClientServer/fanout use `ChannelName` |

**Peers and object moves aren't separate sources.** They're carried as a
list inside topology status and don't have their own `Sequence`. If one
peer changes, that topology's whole status is published with a new
`Sequence`. To have a separate slot per peer or per move, **a separate
stream and per-language contract must be defined first** — until then it
isn't a unit of this coalescing rule.

A source key is created when that target first becomes observable, and
removed **after its terminal status is delivered or discarded**. While the
key is alive, `Sequence` increases monotonically within that key.

### 7.2 Coalescing

The framework can coalesce intermediate status so a slow observer doesn't
delay message dispatch, location claims, or host lifecycle. Coalescing is
a method that keeps **one latest-status slot per source**. A previous
intermediate status of the same source is replaced by the latest status.
Even so, the following results are guaranteed.

- Delivers the most recent status's `Sequence` for **kept sources**.
- Terminal statuses for relocation and shutdown aren't overwritten by an
  intermediate status.
- One observer's delay, cancellation, or failure doesn't affect other
  observers or the runtime result.
- A cumulative field reflects the latest value even after coalescing.
  Increments to backpressure and drop counters aren't lost to coalescing.

Even with the one-slot-per-source structure, if an observer continues
without reading, terminated sources' terminal statuses accumulate. The
retained amount is capped, and once the cap is exceeded, the framework **discards the
oldest terminal status first.** A structure that retains terminal status
indefinitely isn't allowed, since one slow observer would exhaust runtime
memory.

An observer must be able to learn about the loss when something is
discarded. Since loss count differs per observer, it's **not put inside the
status** — status is a value shared among observers, and putting a
per-observer value in it would make it unshareable.

Instead, the unit the stream delivers is defined as **a pair of status and
delivery information**.

| Component | Content |
|---|---|
| status | The complete status, same as before. Shared among observers |
| lost-update count | The number of items this observer has lost since starting its subscription |

**The lost-update count counts what disappeared via intermediate-status
coalescing and what disappeared via terminal discard, separately.**
Merging the two would make it impossible for an observer to distinguish
"skipped while catching up" from "never seen at all." Starting a new
subscription resets it to 0. It's clamped to the maximum value if it
exceeds the representable range.

A whole-runtime metric can't substitute for this value — it can't
determine which observer lost what. An observer learns about loss from
this value's increase and a `Sequence` gap.

A source's latest slot is removed once that source's lifecycle ends and
its terminal status is delivered or discarded. A removed source drops out
of the "kept sources" above.

**The framework doesn't end a stream just because an observer's queue is
full.** It relies only on the coalescing and retention cap above to let the
observer catch up —
the stream stays open even if an observer stays slow. Canceling
observation only ends that stream. It doesn't cancel already-accepted
runtime work or other observers.

## 8. Querying an Object's Current Location

An operational tool can precisely query the current location by Actor ID or
Spot ID, or enumerate the management scope of stored location information
by page.
This result isn't used as a messaging target or placement selector.

The fields, page size, and cache contract provided by the
[Location Store](../00-foundation/02-glossary.en.md#location-store), the reference
storage for location information, are set by
[Location Runtime's Operational Query](../05-location-relocation/01-location-runtime.en.md#74-querying-the-current-location-from-operational-tools).

Per-ID lookup and paging return `Creating`, `Ready`, and `Unavailable`
entries with the same meaning. A missing record produces an empty per-ID
result and is absent from a page. A Store query failure is an `Unavailable`
framework error and never returns part of a page as a successful result.

## 9. Structured Log

The framework records why a state changed to a standard structured
logger. The application configures the logger provider and backend. The
framework public interface doesn't provide a sink, file path, exporter
lifecycle, or event DTO.

The following identifiers use the same string in every language.
Among them, `zlink.runtime.host.relocation_changed` also records changes to
[Relocation mode](../00-foundation/02-glossary.en.md#relocation-mode) — the
caller intent that decides which application version the host's stateful
objects move to.

| Identifier | Change it records |
|---|---|
| `zlink.runtime.mesh_node.state_changed` | A MeshNode's lifecycle or ready state changed. |
| `zlink.runtime.mesh_node.peer_changed` | A peer's work acceptance, ready, or service state changed. |
| `zlink.runtime.mesh_node.channel_changed` | A Channel's weight, ready target count, or selectability changed. |
| `zlink.runtime.object.placement_changed` | Placement aggregation changed due to reservation, Ready, abort, capacity exhaustion, or relocation. |
| `zlink.runtime.mesh_node.routing_id_conflict` | An automatic Node RID owner claim failed due to an active conflict. |
| `zlink.runtime.host.relocation_changed` | Relocation mode, effective target version, host state, or terminal result changed. |
| `zlink.runtime.host.termination_changed` | Shutdown state or terminal result changed. |
| `zlink.runtime.relocation.changed` | An Actor or Spot relocation phase/recovery state changed, or one unit's admission interruption time exceeded 1 second. |
| `zlink.runtime.client_server.state_changed` | ClientServer local role, lifecycle, or ready state changed. |
| `zlink.runtime.client_server.server_changed` | A ClientServer target's weight, ready, or service state changed. |
| `zlink.runtime.fanout.publisher_changed` | An automatic publisher's connection target or ready state changed. |
| `zlink.runtime.location.store_changed` | The Location Store changed between ready and degraded. |

A log records timestamp, source kind, and registration name. For relevant
changes, it also records Node RID, weight, reason, and state. Payload,
metadata, Actor ID, Spot ID, owner token, generation, raw frame, and native handle aren't
recorded.

A structured log or dedicated metric isn't recorded when work enters or
leaves a mailbox. The framework doesn't turn a mailbox's individual
enqueue/dequeue and turn into operational events. Operation failure is
aggregated into drop/timeout/backpressure metrics, and individual message
delay is investigated via
[Message Flow Tracing](03-message-flow-tracing.en.md).

Publisher state is recorded as one of `excluded_draining`, `excluded_stale`,
`reconnecting`, or `disconnected`. A log is a judgment at the time it was
recorded and isn't the basis for current location or state. Current state
is read from fanout status.

If a relocation unit's time from source admission seal to the terminal
success or failure of the one-way cutover submit exceeds 1 second,
`zlink.runtime.relocation.changed` records `unit_kind` and, where needed,
`execution_mode`, as well as `interruption_target_exceeded=true` and the actual
duration. `unit_kind` is one of `actor`, `instance_spot`, `user_spot`.
This is an operational warning and doesn't change the relocation outcome
or recovery judgment. Actor ID and Spot ID aren't put in structured logs
and are only checked via limited-scope trace. The opening of target admission
isn't acknowledged to the source and is observed through target-local
status and tracing.

## 10. Startup and Failure

- Requesting status for an unregistered `MeshName` or `ChannelName` is a
  configuration error.
- Requesting automatic status for a fanout `ChannelName` registered only
  with a manual subscriber is a configuration error.
- A runtime with no Location Store shows store state as `not_configured`.
- If object role is `Client` or `Server` but there's no Location Store,
  host startup fails.
- Runtime status stays usable even if metrics or trace are turned off.
- A logger provider failure doesn't change message dispatch, reply,
  topology coordination, or host lifecycle results.

## 11. Verification Requirements

The following is verified using only the public surface — the
host/topology status query API, the status-change observation API, the
`SafeToShutdown` observation value, the operational tool's location query
API, and structured-log identifiers. Each item leads to one implementation
or contract test.

**Host status**

- Readiness, whether new work is accepted, and relocation and shutdown
  results can be judged from host status alone.
- Public status has no endpoint, descriptor revision, owner lease, claim,
  reservation, native handle, or raw event DTO.
- `SafeToShutdown` isn't published before every relocation unit reaches
  the point where its Message Follow route can be removed and each unit's
  cutover retransmission window ends, and neither judgment uses another
  node's clock.
- Controlled ClientServer DEALER-ROUTER reply bytes appear in ordinary Core
  HWM accounting and total messaging, and don't appear in Completion fields.
  RouteMesh ROUTER-ROUTER reply bytes appear in Completion current, peak, and
  pending.

**Topology status**

- RouteMesh, ClientServer, and automatic fanout each provide readiness and
  target state as one complete status.
- Placement weight `0`, capacity exhaustion, and recovery match public
  status.
- Automatic fanout's 15-second record timeout only turns that publisher
  unavailable.

**Observing changes**

- Re-querying the current status after a `Sequence` gap restores every
  state.
- A slow observer, observation cancellation, and logger provider failure
  don't change dispatch, reply, or lifecycle terminal results.

**Location query and log**

- Object location lookup follows Location Runtime's page and cache
  contract.
- Publish target count and per-target accept/failure results don't appear
  in status or runtime structured logs.

---

[Observability topic table of contents](README.en.md) · [Spec table of contents](../README.en.md) · [Next: 02. Runtime Metrics](02-runtime-metrics.en.md)
