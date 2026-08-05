---
title: "Runtime Status Query And Operational Diagnostics"
---

# Runtime Status Query And Operational Diagnostics

[Spec table of contents](README.en.md) · [Previous: Relocation Store Provider SPI And The Official Redis Implementation](23-relocation-store-redis.ko.md) · [Next: Runtime Metrics And Aggregation Rules](25-runtime-metrics.ko.md)

> **What this chapter defines** — how an operator queries the framework runtime's
> current status, observes changes, and finds causes in logs.


## 1. Contract This Document Defines

This document defines how an application operator queries the framework runtime's
current status once, observes subsequent changes, and finds why a status changed in
logs. The application uses this information to judge whether it can accept new work,
the scope of a failure, and relocation/shutdown results.

This document owns the complete status at a specific point in time, the status-change
stream, and structured-log identifiers. The name/unit/label of numbers accumulated or
collected over time is owned by
[Runtime Metrics](25-runtime-metrics.ko.md); the progress record of one message is
owned by [Message Flow Tracing](26-message-flow-tracing.ko.md); the state transitions
of relocation and shutdown are owned by
[Host Relocation And Shutdown](28-graceful-drain-handoff.ko.md).

| Actor | Responsibility |
|---|---|
| Application | Queries/observes status by registered name, and configures the logger provider and backend. |
| Framework | Combines internal service values into a complete status and records standard identifiers for state changes. |
| Provider | Delivers logs to the logger backend the application chose. Ensures a provider failure doesn't change the runtime result. |
| Remote runtime | Publishes its own service availability and operational state. The current runtime reflects this in topology status. |

The [descriptor revision](01-glossary.en.md#descriptor-revision), indicating the order
in which remote registration information changed, and the
[owner lease](01-glossary.en.md#owner-lease), indicating whether a host can keep using
its current lifecycle's ownership, are used only for internal judgment. The public
interface doesn't expose these two values, the internal state of work acceptance,
claims, capacity reservation, socket state, exporters, storage, raw event DTOs, or
native handles.

## 2. State The Application Reads At Once

The application reads per-feature status by the name it registered at startup. It
doesn't directly combine values from several internal services.

The registration names appearing for the first time are as follows.

- The runtime unit that provides RouteMesh peer connections and Channel messaging
  within one process is called a [MeshNode](01-glossary.en.md#meshnode). The logical
  runtime where multiple MeshNodes share the same messaging rules is called a
  [RouteMesh](01-glossary.en.md#routemesh), and the startup registration name
  identifying one RouteMesh is called a
  [MeshName](01-glossary.en.md#meshname).
- The startup registration name identifying one Channel is called a
  [ChannelName](01-glossary.en.md#channelname).
- The topology where Client and Server exchange requests and replies under a
  ChannelName is called a
  [ClientServer Channel](01-glossary.en.md#clientserver-channel).
- The logical execution unit with an address and state that receives messages is
  called a [Spot](01-glossary.en.md#spot).
- The state where every per-feature serving condition is met, so it can receive
  application messages, is called
  [Ready](01-glossary.en.md#ready).

| State scope | Values checked in one status |
|---|---|
| Host | Runtime state, ready status, new-work acceptance, deadline, relocation/shutdown result, and inbound dispatch state |
| RouteMesh | `MeshName`, overall state, ready peer count, per-Channel ready target count, per-peer operational state, and the current process's Actor/Spot count |
| ClientServer | `ChannelName`, local role, overall state, ready target count, and per-target operational state/weight |
| Automatic fanout | `ChannelName`, overall state, number of publishers being connected to, and ready publisher count |

Status is an immutable value that can be kept after the call ends. It doesn't reference
a native handle, caller buffer, payload, or application metadata.

The following C# is a non-normative excerpt showing the common behavior. It doesn't
require the same signature in other languages. The exact type and signature are set by
[.NET Topology Monitoring](server/languages/dotnet/interfaces/10-topology-monitoring.en.md).

```csharp
public interface IZLinkRouteMeshRuntime
{
    ZLinkRouteMeshStatus GetStatus(string meshName); // reads the current state of the registered RouteMesh.

    IAsyncEnumerable<ZLinkObservedStatus<ZLinkRouteMeshStatus>> ObserveAsync(
        string meshName,
        CancellationToken cancellationToken = default); // receives subsequent complete states and the loss total, in order.
}
```

Host state doesn't belong to a specific `MeshName`. Relocation's and shutdown's final
results are also provided just once, in host status.

### 2.1 Host State

Host runtime state is closed to the following values. A value not in this table must
not be added. The procedure of blocking new work and cleaning up already-accepted work
within a time limit is called
[drain](01-glossary.en.md#drain). That time limit is called a
[deadline](01-glossary.en.md#deadline).

| Value | Meaning the application observes |
|---|---|
| `preparing` | Validating startup configuration and preparing the runtime. |
| `serving` | Can accept new application operations. |
| `relocating` | Stopped accepting new work and is moving stateful objects to another node. |
| `relocated` | Relocation finished; infrastructure and connections are kept. |
| `draining` | Cleaning up remaining processing and resources without relocation. |
| `stopped` | Runtime and infrastructure cleanup are finished. |
| `error` | An error occurred that prevents continuing to operate the runtime. |

`IsReady` is `true` only when `State` is `serving`. `AcceptingWork` indicates whether
the current host accepts new application operations. Don't reinterpret the two values
as separate conditions. The exact meaning of relocation option, deadline, and result is
set by [Host Relocation And Shutdown](28-graceful-drain-handoff.ko.md).

Host status's inbound dispatch item shows how much application payload the framework
has received and is currently processing. The value is read from a running total the
dispatch accounting keeps, not built by walking the queue on every query.

| Value | Meaning |
|---|---|
| Applied HWM bytes | The [Application HWM](01-glossary.en.md#application-hwm) fixed at startup. `0` means no limit. |
| Pending payload bytes | The sum of payload bytes waiting in the queue and being handled. |
| Queued payload bytes | Payload bytes whose handler hasn't started yet. |
| Active payload bytes | Payload bytes whose handler is currently running. |
| Application receive paused | Indicates whether new application receive is stopped due to HWM. |
| Pending completion sends | The number of requests waiting for or having secured a reply permit. |
| Completion send limit | The cap on completion send permits the host can hold concurrently. |

`PendingPayloadBytes = QueuedPayloadBytes + ActivePayloadBytes` must hold. This status
doesn't include payload, Actor ID, Spot ID, message type, or a per-owner list. A
per-owner top-N isn't provided as part of the public contract. A per-owner label or
event isn't added to regular status updates or the message hot path.

### 2.2 Topology State

Topology state represents a different scope from host state. Host state is the
process-wide startup, relocation, and shutdown progress. Topology state indicates
whether one RouteMesh/ClientServer/automatic fanout registered under a `MeshName` or
`ChannelName` can currently process application messages.

So even if the host is `serving`, if a specific ClientServer Channel has no ready
target, only that topology can be `degraded`. Conversely, if the host is `relocating`,
`relocated`, or `draining`, every topology's `IsReady` is `false` even if connections
remain. At this point, the connected peer/target count still provides the actual
current connection state — the count isn't changed to `0` just because the host isn't
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
[Routing ID](01-glossary.en.md#routing-id), as the Node RID value. Endpoint,
descriptor revision, and connection generation aren't provided.

Peer state distinguishes two cases where there's no connection.

| Peer state | Meaning | Ready/failure aggregation |
|---|---|---|
| `not_connected` | The topology requires a connection but there's currently no ready connection. | Excluded from ready peer count. Reflected in topology degraded status and liveness/health failure aggregation. |
| `not_required` | Both MeshNodes are Object Client and neither has RouteMesh Channel Server membership, so no connection is needed. Automatic excludes it at the descriptor-check stage; Manual confirms it at handshake. | Excluded from ready peer count. Also not a target for liveness probe/reconnect/health failure aggregation. |

A `not_required` peer is still left in the status's peer list. This lets an operator
distinguish a normal connection omission from a connection failure. This state alone
doesn't turn a RouteMesh `degraded`.

RouteMesh placement state provides whether new objects are accepted and the current
active Actor/Spot count. Status separately provides the count of Spots and the Actors
processing application messages within them. The per-type capacity reservation
registered at startup, the
[activation barrier](01-glossary.en.md#activation-barrier) blocking first-message
delivery before Spot initialization finishes, and internal capacity counters, aren't
provided.

Placement's `IsAvailable` is `true` only when the host is `serving`, it's an Object
Server, placement weight is positive, and there's headroom in both Actor/Spot capacity
and activation concurrency. Activation concurrency's current value and limit aren't
exposed as a separate field in public status.

The [weight](01-glossary.en.md#weight) used for the new-target selection ratio is a
signed integer `0..10000`. A value of `0` excludes it from being chosen as a new
placement target.

A ClientServer Server in the same process is also a candidate on equal footing with a
remote Server. Status provides target count and each target's state/weight.
`client_and_server` means both roles are registered under the same `ChannelName` — it
isn't a separate registration role.

An automatic fanout publisher becomes ready once it connects the socket and receives
an application record, or the
[liveness beacon](01-glossary.en.md#liveness-beacon) the framework exchanges to check
connection status. If a disconnect is confirmed, or there's no record for 15 seconds,
only that publisher is excluded from candidates. A connection plan or `connect`
acceptance alone doesn't make it ready.

## 3. Querying Current State And Observing Changes

Each language provides a current-status query and an async change observation. Names
and types are set by each language's exact interface.

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

Status includes a `Sequence` that monotonically increases within the runtime instance,
and an observation time. Within the same source, a larger `Sequence` is a later state.
Values from different sources aren't compared. `Sequence` can restart from 0 when the
process restarts.

Each item in the change stream is a complete status, not an event carrying only some
fields. A general-purpose event DTO combining nullable fields isn't provided. If an
observer notices a `Sequence` gap, it re-queries the current status to restore every
field.

### Definition Of A Source

The unit of coalescing, a **source**, equals **the thing that owns a `Sequence`**.
Since each status item carries one `Sequence`, the entity issuing that `Sequence` is
the source.

| Stream kind | Source | Source key |
|---|---|---|
| Host status | This one runtime instance | The runtime instance ID. One for the process's lifetime |
| Topology status | One topology runtime | RouteMesh uses `MeshName`; ClientServer/fanout use `ChannelName` |

**A peer and an object move aren't separate sources.** They're carried as a list inside
topology status and don't have their own `Sequence`. If one peer changes, that
topology's whole status is published with a new `Sequence`. To have a separate slot
per peer or per move, **a separate stream and per-language contract must be defined
first** — until then it isn't a unit of this coalescing rule.

A source key is created when that target first becomes observable, and removed
**after its terminal status is delivered or discarded**. While the key is alive,
`Sequence` increases monotonically within that key.

### Coalescing

The framework can coalesce intermediate status so a slow observer doesn't delay
message dispatch, location claims, or host lifecycle. Coalescing is a method that
keeps **one latest-status slot per source**. A previous intermediate status of the
same source is replaced by the latest status. Even so, the following results are
guaranteed.

- Delivers the most recent status's `Sequence` for **kept sources**.
- Relocation's and shutdown's terminal status isn't overwritten by an intermediate
  status.
- One observer's delay, cancellation, or failure doesn't change another observer or
  the runtime result.
- A cumulative field reflects the latest value even after coalescing. An increment of
  backpressure and drop counters isn't lost to coalescing.

Even with the one-slot-per-source structure, if an observer keeps not reading,
terminated sources' terminal status accumulates. There's a cap on this retained
amount, and once exceeded, the framework **discards the oldest terminal status
first.** A structure that retains terminal status indefinitely isn't allowed, since one
slow observer would exhaust runtime memory.

An observer must be able to learn about the loss when something is discarded. Since
loss count differs per observer, it's **not put inside the status** — status is a
value shared among observers, and putting a per-observer value in it would make it
unshareable.

Instead, the unit the stream delivers is defined as **a pair of status and delivery
information**.

| Component | Content |
|---|---|
| status | The complete status, same as before. Shared among observers |
| loss total | The number of items this observer has lost since starting its subscription |

The loss total counts **what disappeared via intermediate-status coalescing and what
disappeared via terminal discard, separately.** Merging the two would make it
impossible for an observer to distinguish "skipped while catching up" from "never seen
at all." Starting a new subscription resets it to 0. It's clamped to the maximum value
if it exceeds the representable range.

A whole-runtime metric can't substitute for this value — it can't determine which
observer lost what. An observer learns about loss from this value's increase and a
`Sequence` gap.

A source's latest slot is removed once that source's lifecycle ends and its terminal
status is delivered or discarded. A removed source drops out of the "kept sources"
above.

The framework doesn't end a stream just because an observer's queue is full. It only
catches up via the coalescing and retention cap above — the stream stays open even if
an observer stays slow. Canceling observation only ends that stream. It doesn't cancel
already-accepted runtime work or other observers.

## 4. Querying An Object's Current Location

An operational tool can query the exact current location by Actor ID or
[Spot ID](01-glossary.en.md#spot-id) — a Spot's global logical address — or enumerate
the management scope of stored location information by page. This result isn't used
as a messaging target or placement selector.

The fields, page size, and cache contract provided by the
[Location Store](01-glossary.en.md#location-store), the reference storage for
location information, are set by
[Location Runtime's Operational Query](21-location-runtime.ko.md#64-운영-도구에서-현재-위치를-조회한다).

## 5. Structured Log

The framework records why a state changed to a standard structured logger. The
application configures the logger provider and backend. The framework public
interface doesn't provide a sink, file path, exporter lifecycle, or event DTO.

The following identifiers use the same string in every language.

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

A log records timestamp, source kind, and registration name. Needed changes add Node
RID, weight, reason, and state. Payload, metadata, Actor ID, Spot ID, owner token,
generation, raw frame, and native handle aren't recorded.

A structured log or dedicated metric isn't recorded when work enters or leaves a
mailbox. The framework doesn't turn a mailbox's individual enqueue/dequeue and turn
into operational events. Operation failure is aggregated into drop/timeout/backpressure
metrics, and individual message delay is investigated via
[Message Flow Tracing](26-message-flow-tracing.ko.md).

Publisher state is recorded as `excluded_draining`, `excluded_stale`, `reconnecting`,
`disconnected`. A log is a judgment at the time it was recorded and isn't the basis for
current location or state. Current state is read from fanout status.

If a relocation unit's time from source admission seal to target admission-open ACK
exceeds 1 second, `zlink.runtime.relocation.changed` records `unit_kind`, and where
needed, `execution_mode`, `interruption_target_exceeded=true`, and the actual
duration. `unit_kind` is one of `actor`, `instance_spot`, `user_spot`. This is an
operational warning and doesn't change the relocation outcome or recovery judgment.
Actor ID and Spot ID aren't put in structured logs and are only checked via
limited-scope trace.

## 6. Startup And Failure

- Requesting status for an unregistered `MeshName` or `ChannelName` is a configuration
  error.
- Requesting automatic status for a fanout `ChannelName` registered only with a
  manual subscriber is a configuration error.
- A runtime with no Location Store shows store state as `not_configured`.
- If object role is `Client` or `Server` but there's no Location Store, host startup
  fails.
- Runtime status stays usable even if metrics or trace are turned off.
- A logger provider failure doesn't change message dispatch, reply, topology
  coordination, or host lifecycle results.

## 7. Implementation And Contract-Test Verification Requirements

- Readiness, new-work acceptance, and relocation/shutdown results must be
  determinable from a single host status.
- RouteMesh, ClientServer, and automatic fanout must each provide readiness and
  target state as one complete status.
- Public status must not contain endpoint, descriptor revision, owner lease, claim,
  reservation, native handle, or raw event DTO.
- Publish target count and per-target accept/failure results must not be included in
  status or runtime structured logs.
- Automatic fanout's 15-second record timeout must only turn that publisher
  unavailable.
- A slow observer, observation cancellation, and logger provider failure must not
  change dispatch, reply, or lifecycle terminal results.
- Re-querying the current status after a `Sequence` gap must restore every state.
- Placement weight `0`, capacity exhaustion, and recovery must match public status.
- Object location lookup must follow Location Runtime's page and cache contract.
