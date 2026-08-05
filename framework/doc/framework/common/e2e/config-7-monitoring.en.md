<!-- framework-adapter-nav:start -->
[E2E table of contents](README.en.md) | [Previous: Store Failure/Recovery](config-6-store-failure-recovery.en.md) | [Next: Execution Turn And Terminator](config-8-execution-turn.en.md)
<!-- framework-adapter-nav:end -->

# Config 7 — Runtime Status And Change Observation

An application can read Host's and RouteMesh's current status at once
via the public runtime monitoring API, or receive a complete new
status every time it changes. Since this info is used for the
operations dashboard and readiness judgment, it must not diverge from
the actual peer/Channel state, and a slow observer must not delay
business messages.

This config creates several MeshNodes' start, stop, store failure, and
capacity change, and verifies that public `GetStatus` and the status
stream match the actual application result. Socket monitor, Location
Store record, and private runtime counters aren't used.

## 1. Verification Scope

- Distinguishing the source/sequence of Host status and RouteMesh
  status
- Addition/removal/recovery of peer and Channel readiness
- Location Store failure and recovery state
- Active Actor/Spot count and placement availability
- Independence of Logical Multicast execution from topology status
- Isolation of a slow or failed observer from other business
  processing
- Status recovery after an invalid public query and repeated restart

Per-language E2E uses only that language's formal monitoring
interface.

| Language | Formal interface |
|---|---|
| C++ | [`route_mesh_runtime_t`](../spec/server/languages/cpp/interfaces/08-monitoring.en.md) |
| .NET | [`IZLinkRouteMeshRuntime`](../spec/server/languages/dotnet/interfaces/10-topology-monitoring.en.md) |
| Java | [Java monitoring](../spec/server/languages/java/interfaces/monitoring.en.md) |
| Kotlin | [Kotlin monitoring](../spec/server/languages/kotlin/interfaces/monitoring.en.md) |
| Node.js | [`ZLinkRouteMeshRuntime`](../spec/server/languages/node/interfaces/03-location-observability.en.md) |

## 2. Deployment Configuration

| Role | Count | What it does and why it's separate |
|---|---:|---|
| Location Store | 1 | Provides automatic discovery and owner lease. Uses a dedicated namespace per run. |
| Service node | 2 | Participates in the same MeshName and ChannelName. Provides a Channel handler, Actor/Spot factory, Logical Multicast target, and public monitoring endpoint. |
| E2E client | 1 | Calls the role server's application endpoint to start status query/observation and business operations. |

The role server's evidence endpoint provides only public status
values, application handler markers, and operation results. Status
stores the immutable value received from a query or observer callback
as-is, and doesn't later re-read a Framework internal object.

## 3. Common Run And Judgment Method

The runner creates a fresh process, Store namespace, and marker per
scenario. A status transition is confirmed by bounded-polling the
observer stream, and at the end, by calling `GetStatus` again to
compare against the current state. Since the observer stream can
coalesce changes, not every intermediate state or continuous sequence
is required. It's enough that the observed sequence within the same
source increases and the final status matches the actual state.

Whether business processing happened is confirmed with the public
request result and handler application evidence. File log and
structured log aren't a pass condition for this config.

## 4. Scenarios

### Track A — Confirm Current State And Readiness

#### MON-A1 Read Host And RouteMesh Status Separately

Priority: `P0`

Host lifecycle and an individual RouteMesh topology are different
sources. If the application compares the two sequences as one
timeline, it can mistake a normal state for a stale value.

**Verification question:** Do Host status and RouteMesh status each
provide a complete current state with their own source and sequence?

- Start condition: Only `svc-a` starts, and Host and RouteMesh are
  ready.
- Procedure: Read and keep both statuses. Start `svc-b`, and once the
  peer and Channel are ready, read both statuses again.
- Verification: The second RouteMesh status reflects the increase in
  ready peer and ready target, and is larger than the first sequence
  from the same Mesh source. Host status provides the Host state and
  whether it accepts new work, comparing sequence only within its own
  source. The initially-kept status value doesn't change from a
  subsequent change.
- Detailed behavior: verifies
  [Runtime Monitoring §2](../spec/24-runtime-monitoring.en.md#2-state-the-application-reads-at-once).

#### MON-A2 Observe The Result Of A Peer Added And Removed

Priority: `P0`

The observer doesn't have to deliver every short intermediate state of
peer lifecycle, but must provide the result of the current Ready peer
change as a complete status.

**Verification question:** After a peer starts/stops/restarts, do the
observer and the latest query show the current peer exactly?

- Start condition: Open a RouteMesh observer on `svc-a` and receive the
  initial status.
- Procedure: Start `svc-b` and observe until it becomes ready.
  Gracefully stop `svc-b`, confirm it's removed from the ready list,
  then start it again as a new process.
- Verification: At each step, the latest status and `GetStatus`
  provide the same ready-peer set. After the restart, the new Node RID
  is ready and the previous RID isn't in the ready list. The observed
  sequence monotonically increases within the same Mesh source.
- Detailed behavior: verifies
  [Runtime Monitoring §2.2](../spec/24-runtime-monitoring.en.md#22-topology-state)
  and
  [§3](../spec/24-runtime-monitoring.en.md#3-querying-current-state-and-observing-changes).

#### MON-A3 Cross-Check Channel Readiness Against The Actual Request Result

Priority: `P0`

If Channel status shows ready, there must be a target that can process
a new request. Conversely, with not a single positive-weight target,
it must not show as selectable.

**Verification question:** Does the Channel's ready-target count match
the actual request success before and after a weight change?

- Start condition: Only `svc-b` provides that Server Channel at weight
  100, and `svc-a`'s status shows target count 1.
- Procedure: Send a normal request once. Change `svc-b`'s weight to 0
  via a public runtime update, and once the status reflects it, send a
  new request. Restore the weight to 100 and immediately request again
  once ready.
- Verification: The first and post-restore requests are each processed
  once by the handler. While weight is 0, status shows no ready
  target, and the request ends in the terminal result of the formal
  error model.
- Detailed behavior: verifies
  [Runtime Monitoring §2.2](../spec/24-runtime-monitoring.en.md#22-topology-state)
  and
  [Channel Topology §7](../spec/07-channel-topology.en.md#7-ready-state-and-channel-target-selection).

#### MON-A4A Restore Readiness After A Normal Replacement

Priority: `P1`

Replacing a gracefully-stopped provider with a new process must leave
the previous peer out of the ready list, with the new peer selected as
the target.

**Verification question:** After a normal replacement, does a new
request succeed from the moment the latest status shows the first
ready?

- Start condition: Two service nodes are ready, and there's a Channel
  for which `svc-b` is the only target.
- Procedure: Gracefully stop `svc-b` and confirm the target is removed
  from status. Start a new process of the same role and send a request
  the moment status becomes ready.
- Verification: The latest status shows only the new RID as a ready
  target, and the request is processed once by the new process's
  handler. No application retry or extra settle sleep is used.
- Detailed behavior: verifies
  [Runtime Monitoring §3](../spec/24-runtime-monitoring.en.md#3-querying-current-state-and-observing-changes).

#### MON-A4B Recover By Excluding A Stale Peer After A Crash

Priority: `P1`

If a provider crashes, it can't send a graceful-shutdown notice. Once
the owner lease expires, the previous descriptor must not be used as a
ready target — it must converge to the new process's state.

**Verification question:** Does a crashed peer drop out of the ready
list after its lease expires, and does a replacement request succeed?

- Start condition: In fresh topology, `svc-b` is ready as the only
  target.
- Procedure: The runner force-kills `svc-b`. After confirming target
  unavailable or removal in status, start a new process and wait for
  ready.
- Verification: The latest status doesn't leave the crashed RID as
  ready, and the new RID is ready. The request right after ready is
  processed once by the new handler.
- Detailed behavior: verifies
  [Runtime Monitoring §2.2](../spec/24-runtime-monitoring.en.md#22-topology-state).

#### MON-A5 Observe Store Failure And Recovery State

Priority: `P1`

If the Location Store becomes unavailable, topology-update reliability
drops. The application must be able to confirm a degraded state in
public status, and judge whether it becomes ready again after Store
recovery.

**Verification question:** Are Store failure and recovery reflected in
RouteMesh's current status?

- Start condition: Two service nodes and the Store are normal, and
  RouteMesh status is ready.
- Procedure: The runner stops the Store process. Observe whether
  public status turns degraded per the configured failure grace.
  Restart the Store and wait until status recovers to ready.
- Verification: During the failure, status shows the Store issue via
  the formal topology state and unavailable reason. After recovery,
  ready targets and actual request success are both restored. The
  grace boundary isn't estimated with a fixed sleep.
- Detailed behavior: verifies
  [Location Runtime §8](../spec/21-location-runtime.en.md#8-when-a-store-response-isnt-received)
  and
  [Runtime Monitoring §2.2](../spec/24-runtime-monitoring.en.md#22-topology-state).

#### MON-A6 Cross-Check Placement Count Against The Capacity Result

Priority: `P0`

An operator must be able to check active Actor/Spot count and new
placement availability in public status. If the count differs from
the actual create result, a scale-out decision would be wrong.

**Verification question:** After Actor/Spot creation and removal, do
the placement count and `IsAvailable` match the actual operation
result?

- Start condition: `svc-a`, which has a small Actor total and Spot
  total limit, is ready.
- Procedure: Create one Actor and one User Spot each via the public
  manager API and read status. Create more up to the limit, then
  create one more time; remove one existing object, then create again.
- Verification: The active count matches the completed public
  lifecycle result at each step. A create over the limit is
  `CapacityExceeded`, and status shows placement unavailable. After
  the object is removed, it returns to available and a new create
  succeeds.
- Detailed behavior: verifies
  [Runtime Monitoring §2.2](../spec/24-runtime-monitoring.en.md#22-topology-state)
  and
  [MeshNode §5](../spec/13-mesh-node.en.md#5-object-placement-capability).

### Track B — Separate Logical Multicast From Topology Status

#### MON-B1 Some Remote Targets Not Receiving Doesn't Change Topology Status Into A Delivery Result

Priority: `P0`

Logical Multicast doesn't return a per-target delivery report. Just
because one remote target's queue didn't receive the message, peer/
Channel readiness must not be changed like a delivery statistic.

**Verification question:** Does an admittable target process the
message, and does topology status keep exactly the real connection
state?

- Start condition: Two remote matching targets in different service
  node processes are ready. One target's handler is blocked at the
  application gate, and a deterministic blocker payload larger than
  the public HWM is sent first, confirming handler entry and that
  public status's Application receive paused is `true`.
- Procedure: The source submits a unique marker via Logical Multicast
  once, and reads the admittable target's evidence and RouteMesh
  status before and after.
- Verification: The public submit ends with the formal terminal
  meaning without a per-target result, and the admittable target
  processes the marker once. If network and peer state haven't
  changed, ready peer and Channel status also stay the same.
- Detailed behavior: verifies
  [Spot Messaging §4](../spec/12-spot-messaging.en.md#4-channel-scoped-logical-multicast)
  and
  [Runtime Monitoring §2.2](../spec/24-runtime-monitoring.en.md#22-topology-state).

#### MON-B2 One Local Target's Handler Wait Doesn't Block Another Target's Delivery

Priority: `P0`

Even if one local target's handler is waiting, it must not roll back
another target that's already able to process. Topology status doesn't
substitute for a per-target delivery count.

**Verification question:** Even with one local target's handler
waiting, does another matching target process the message first?

- Start condition: There are two matching targets in the same process,
  and one handler waits at the application gate. Neither a network
  block nor a public HWM boundary is used.
- Procedure: The source publishes a unique marker once, and collects
  application evidence for the gated target and the other target. Once
  the other target's evidence is confirmed, the gate is opened.
- Verification: The other target processes the marker once while the
  gate is closed, and the gated target also processes it once
  afterward. Publish doesn't return a per-target result payload, and
  RouteMesh's peer/Channel status doesn't change.
- Detailed behavior: verifies
  [Spot Messaging §4](../spec/12-spot-messaging.en.md#4-channel-scoped-logical-multicast).

### Track C — Isolate Observer From Business Processing

#### MON-C1 A Slow Or Failed Observer Doesn't Block Other Work

Priority: `P1`

A status observer is an application callback that consumes
operational info. Even if one observer processes status slowly or ends
in an exception, message dispatch and other observers must keep
progressing.

**Verification question:** Do a request and a normal observer keep
completing while a slow observer is blocked?

- Start condition: Open a slow observer and a normal observer on the
  same Mesh at `svc-a`. The slow observer waits on the first callback
  at an application signal.
- Procedure: Start and stop `svc-b` to create several status changes,
  and send a separate Channel request. Confirm the normal observer's
  latest status and the request reply, then terminate the slow
  observer with an exception.
- Verification: The request receives a reply within the deadline, and
  the normal observer provides the current status. Even if there's a
  gap in the slow observer's sequence, re-reading `GetStatus` matches
  the latest state. The slow observer terminating doesn't terminate
  the normal observer.
- Detailed behavior: verifies observer isolation from
  [Runtime Monitoring §3](../spec/24-runtime-monitoring.en.md#3-querying-current-state-and-observing-changes).

### Track D — Handle Invalid Queries And Repeated Failure

#### MON-D1A Reject A Query For An Unregistered MeshName

Priority: `P1`

If the application queries a MeshName it didn't register, it must not
return a different Mesh's state or an empty normal status.

**Verification question:** Does a query and observation start for an
unregistered MeshName end in a public validation error?

- Start condition: The host registers only the `game` Mesh.
- Procedure: The application endpoint attempts `GetStatus` and
  `Observe` start for `missing-mesh` each.
- Verification: Both calls end in the configuration or argument error
  the per-language interface defines, without affecting `game`'s
  status or observer.
- Detailed behavior: verifies
  [Runtime Monitoring §6](../spec/24-runtime-monitoring.en.md#6-startup-and-failure).

#### MON-D1B Keep Observing Status Even After Repeated Crash And Restart

Priority: `P1`

If the observer only handles one failure and misses subsequent
changes, it can't be used in a long-running operational tool.

**Verification question:** Even with peer crash and restart repeated
three times, does the same observer converge to the final ready
state?

- Start condition: `svc-a`'s observer has received the initial status.
- Procedure: Repeat force-killing `svc-b`, confirming peer removal,
  starting a new process, and confirming ready, three times.
- Verification: Each cycle's latest status reflects the actual current
  RID and ready-target count. The observed sequence of the same source
  increases, and the last `GetStatus` matches the observer's latest
  value.
- Detailed behavior: verifies
  [Runtime Monitoring §3](../spec/24-runtime-monitoring.en.md#3-querying-current-state-and-observing-changes).

## 5. Completion Criteria

- Every judgment uses only public status, the public operation result,
  and application handler evidence.
- It's not assumed the observer delivers every intermediate state and
  a continuous sequence. At the end, current state is always confirmed
  via `GetStatus`.
- Readiness, Store recovery, and handler completion are bounded-
  polled, not dependent on a fixed sleep or log flush.
- A check that there's no private field absent from the schema isn't
  built as an E2E assertion. The per-language public interface and
  contract test verify the public type shape.
- One observer's delay and exception must not change another observer,
  message dispatch, or request reply.
