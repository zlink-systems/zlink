<!-- framework-adapter-nav:start -->
[E2E table of contents](README.en.md) | [Previous: Spot Actor Relocation](config-10-spot-actor-relocation.en.md) |
[Next: Channel Egress Routing](config-12-channel-egress-routing.en.md)
<!-- framework-adapter-nav:end -->

# Config 11 — Observing Flow, Metrics, And Host Maintenance

An operator must be able to trace the flow of one application message
crossing several nodes and Actors/Spots, check aggregate metrics for
connection/request/relocation, and judge how far Host maintenance has
progressed. Since this info is a formal public observability contract,
this config directly uses the flow log, metric reader, and Host status
as the verification basis.

Messaging payload and lifecycle results are confirmed with the role
server's application evidence. Location Store row, relocation
manifest, Core peer table, and private counters aren't used for
judgment.

## 1. Verification Scope

- Flow correlation continuing from STREAM to Actor to Spot
- Failure, fanout, timer, and runtime tracing-level change
- Stream connection, relocation, request, and owner lease metrics
- Actor/User Spot handoff, rolling update, and planned maintenance
- Relocate blocker, concurrent call, cancellation, and Shutdown
  contention

## 2. Deployment Configuration

| Role | Count | What it does and why it's separate |
|---|---:|---|
| Location Store | 1 | Provides global object location and automatic topology. |
| Relocation Store | 1 | Preserves `PreserveStateWith` Actor/Spot relocation payload. |
| Session gateway | 1 | Provides Stream Session, Actor binding, and Session relay. |
| Play node | 2–4 | Provides Actor, `SpotWide` User Spot, and Instance Spot factory/adapter. Configures target variants differing in version/capacity/maintenance state. |
| Order workflow | 2 | Creates fanout projection and a timer-origin flow. Doesn't participate as a Play relocation target. |
| E2E client | per scenario | Calls Stream and the role server's public application endpoint. |

Each host uses the formal message-flow configuration and that
language's standard metric reader. Flow verification parses only the
field and public trace record the spec defines. Metric is compared as
the delta between a snapshot right before the scenario starts and a
snapshot after the action completes, and a process cumulative value
isn't assumed to be fixed.

## 3. Common Run And Judgment Method

The runner creates a fresh Store namespace, object ID, flow marker,
and metric reader per scenario. It starts the operation after role
health, public topology status, and Host status reach the needed start
state. If a specific span of relocation must be held, the application
factory, adapter, or handler callback waits on a public application
signal. A hook that stops a Framework internal state transition isn't
used.

Since flow and metric are this config's verification target, they can
be used as a pass condition. General file-log strings, internal debug
events, and implementation-specific allocation counts are only used as
diagnostic material.

## 4. Scenarios

### Track A — Connecting Message Flow Across Processes

#### OBS-A1 Keep The Same Flow From STREAM To Actor To Room Spot

Priority: `P0`

Even as a client action passes through the Session gateway, Actor, and
room Spot in order, it must be searchable as one flow.

**Verification question:** Do one Stream request's connector/Session/
Actor/Spot traces share the same flow ID?

- Start condition: The client Session is bound to the Player Actor,
  and the Actor exists on a room Spot. Every role's tracing level is
  `key_transitions`.
- Procedure: The client sends one game action with a unique marker via
  Stream.
- Verification: The connector outbound, Session inbound, Actor relay,
  and room Spot dispatch records all share the same flow ID and
  marker. Each hop's application handler runs once.
- Detailed behavior: verifies
  [Flow Correlation §5](../spec/27-flow-correlation.en.md#5-propagation-rule).

#### OBS-A2 Leave A Flow Even On A Dispatch-Failure Record

Priority: `P0`

A message with no handler must also be searchable together with its
original request flow, so a cause can be analyzed.

**Verification question:** Does an unregistered packet's public error
trace include the original flow ID?

- Start condition: Caller tracing is on, and the target hasn't
  registered a negative-packet handler.
- Procedure: Send one negative packet with the public typed client,
  then send a normal packet.
- Verification: The negative dispatch's trace has the flow ID the
  caller made, and the formal error phase. The normal packet is also
  handled as an independent flow, and the two markers don't mix.
- Detailed behavior: verifies
  [Flow Correlation §7](../spec/27-flow-correlation.en.md#7-reply-and-failure).

#### OBS-A3 A Tracing-Off Span Doesn't Propagate An Inbound Flow

Priority: `P1`

A node with tracing off doesn't build a flow context or copy it to the
next hop. The next enabled node starts a new flow on an inbound
message with no flow.

**Verification question:** On an enabled→off→enabled path, does the
last node use a new ID different from the earlier flow?

- Start condition: The source and target are `key_transitions`, and
  the middle node is `off`.
- Procedure: Send one message with a unique marker crossing three
  nodes.
- Verification: The source record has a flow, and the off node has no
  flow trace. The target builds a new flow ID different from the
  source, and processes the application payload normally.
- Detailed behavior: verifies
  [Flow Correlation §4](../spec/27-flow-correlation.en.md#4-when-a-flow-is-created).

#### OBS-A4 A Fanout Branch Shares A Flow, And A Timer Builds A New Flow

Priority: `P1`

Subscriber records branched from one publish share the original flow.
Conversely, a timer callback with no existing inbound operation starts
a new timer-origin flow.

**Verification question:** Do fanout subscribers receive the same
flow, while a timer callback has a separate timer-origin flow?

- Start condition: The order workflow and N subscribers are ready, and
  a room timer is registered.
- Procedure: The workflow handler publishes a projection event, and
  separately runs a one-shot room timer.
- Verification: The N subscriber traces share the publish flow ID. The
  timer trace has a different flow ID and `origin=timer`, and the
  timer handler runs once.
- Detailed behavior: verifies
  [Flow Correlation §4](../spec/27-flow-correlation.en.md#4-when-a-flow-is-created)
  and
  [§5](../spec/27-flow-correlation.en.md#5-propagation-rule).

#### OBS-A5 Apply A Runtime Tracing-Level Change

Priority: `P0`

A tracing-level change must not stop business processing, and must
apply starting from the message sent after the change completes.

**Verification question:** During the
`key_transitions→off→errors_only→key_transitions` transition, do all
requests get processed, and does each marker's trace scope match its
level?

- Start condition: Prepare a role server offering public diagnostics
  control, and normal/error packets.
- Procedure: Right after each level-change awaitable completes, send a
  normal or error request with a different marker.
- Verification: Every request gets the formal application result. The
  off marker has no flow trace, errors-only has only error records,
  and a new flow trace resumes from the last marker.
- Detailed behavior: verifies
  [Flow Correlation §8](../spec/27-flow-correlation.en.md#8-observability-and-privacy).

### Track B — Cross-Check Runtime Metric Against Actual Events

#### OBS-B1 Confirm Stream Connection And Reconnect Metric

Priority: `P0`

The active-connection gauge and reconnect counter must match the
actual connector lifecycle.

**Verification question:** After clients connect/disconnect/auto-
reconnect, does the metric delta and current gauge match the actual
connection count?

- Start condition: Save the baseline of the session server and
  connector metric reader.
- Procedure: Connect N clients and gracefully close some. Cut one
  connector's network and let auto-reconnect complete.
- Verification: The server active-connection gauge matches the actual
  connection count at each step. The connector reconnect counter
  increases by exactly that many auto-reconnect events, and the label
  uses only the spec's closed values.
- Detailed behavior: verifies
  [Runtime Metrics §4](../spec/25-runtime-metrics.en.md#4-object-and-stream).

#### OBS-B2 Confirm Actor Relocation Metric

Priority: `P0`

The relocation counter and duration must match the actual Actor move
terminal. Exceeding the interruption target doesn't turn it into a
relocation failure.

**Verification question:** Does one Actor move get reflected exactly
once in the relocation-completed/duration/interruption metrics?

- Start condition: The Actor is ready on `play-a`, and the metric
  baseline is saved.
- Procedure: Move the Actor to `play-b` via a public Join or Host
  Relocate, and confirm the public current Actor location and the
  completion callback.
- Verification: The completed-counter delta is 1 with
  `object_kind=actor`. One each of a duration and interruption sample
  is added, and if the public move result is success, an interruption
  time exceeding its target doesn't turn the completed outcome into a
  failure.
- Detailed behavior: verifies
  [Runtime Metrics §5](../spec/25-runtime-metrics.en.md#5-host-relocation-and-shutdown).

#### OBS-B3 Confirm The Absence Of A Publish Metric And Owner-Lease Lateness

Priority: `P1`

The spec doesn't provide a per-target publish metric for Logical
Multicast or classic fanout. Owner lease renewal lateness is provided
as a low-cardinality metric.

**Verification question:** Is publish delivery visible only via
application evidence, while Store delay is recorded in the lease-
lateness metric?

- Start condition: Prepare the metric reader baseline and ready
  subscribers.
- Procedure: Publish a fanout marker and a Logical Multicast marker
  each. The runner delays the Redis response externally to create
  owner-lease-renew lateness.
- Verification: Subscribers receive the marker, but no publish
  target/receive/drop-dedicated metric is created. The lease-lateness
  sample increases, and no metric label has a flow ID, Actor ID, or
  Spot ID.
- Detailed behavior: verifies
  [Runtime Metrics §6](../spec/25-runtime-metrics.en.md#6-location-and-telemetry)
  and
  [§7](../spec/25-runtime-metrics.en.md#7-label-cardinality).

#### OBS-B4 Process Messaging Even Without A Metric Reader

Priority: `P1`

The metric reader and exporter are a collection boundary the
application chooses. A business path must not fail just because a
reader is absent.

**Verification question:** Does a host with no registered metric
reader provide the same request/send result?

- Start condition: The same-configured host A has a reader registered,
  and B doesn't.
- Procedure: Run 100 requests and sends each with the same marker on
  both hosts.
- Verification: Both hosts' reply, handler count, and payload value
  are the same. B, with no reader, doesn't require a separate exporter
  or evidence queue. Allocation and clock-read cost are a benchmark's
  responsibility.
- Detailed behavior: verifies
  [Runtime Metrics §8](../spec/25-runtime-metrics.en.md#8-collection-boundary).

### Track C — Operate Host Relocate And Shutdown

#### OBS-C1 Exclude A Relocating Host From New Placement

Priority: `P0`

A Host that started Relocate keeps existing accepted work and
infrastructure, but must drop out of new-placement candidates. After
completion, the process doesn't stop and stays in `Relocated` state.

**Verification question:** Do Host status and placement result
together reflect the `Serving→Relocating→Relocated` transition?

- Start condition: `play-a` has a stateful object, and `play-b` is an
  eligible target. The target adapter withholds restore completion at
  an application signal.
- Procedure: Start a public Relocate on `play-a`. While the target is
  restore-held, confirm Host status and new object placement, then
  release the restore.
- Verification: During the held span, the source is `Relocating`,
  not-ready, and not-accepting, and a new object isn't placed at the
  source. After completion, the source status is `Relocated`, and the
  process health endpoint is kept alive. The Host state metric also
  reflects the same closed state.
- Detailed behavior: verifies
  [Host Maintenance §13](../spec/28-graceful-drain-handoff.en.md#13-observability-information).

#### OBS-C2 Keep A Bound Session Push After Actor Handoff

Priority: `P0`

When an Actor moves to a different node, Framework updates the bound
Session's Actor location. The moved Actor's push and Session relay
must continue even without the application rebinding.

**Verification question:** After a Host Relocate, does Actor push and
relay get handled at the new owner with the same Session binding?

- Start condition: `play-a`'s Actor is bound to a Session, and the
  client has received a pre-relocation push.
- Procedure: Relocate `play-a` and confirm the completion of the
  Actor's move to `play-b`. Without calling bind again, the client
  sends a relay request and the Actor sends a post-relocation push.
- Verification: The public current Actor location is `play-b`, and the
  request handler evidence is also at B. The client receives the push
  once, and binding count and Actor identity are kept.
- Detailed behavior: verifies
  [Host Maintenance §8.3](../spec/28-graceful-drain-handoff.en.md#83-an-actor-belonging-to-an-entry-spot)
  and
  [Session Actor Dispatch §7](../spec/20-session-actor-dispatch.en.md#7-execution-and-lifecycle).

#### OBS-C3 Move A User Spot Aggregate Together With Its Member Actors

Priority: `P0`

A `SpotWide` User Spot restores Spot state and member Actor state at
the target as one aggregate. Sending a message with the same global
IDs after the move must let the target continue existing application
state.

**Verification question:** Are the User Spot and every member Actor
processed at the target with identity/generation/state kept?

- Start condition: `play-a` has a User Spot with counter state and two
  member Actors. Save the public refs and state values.
- Procedure: Run a Host Relocate and wait for public completion. Send
  a request with the same SpotId and ActorIds, and re-query the refs.
- Verification: Every current location is `play-b`, and ObjectGeneration
  matches the previous ref. The Spot counter and Actor state are
  preserved, and each handler runs once at the target. Source
  `OnClosing(RelocationOut)` and target restore application callbacks
  are also recorded the formal number of times per operation.
- Detailed behavior: verifies
  [Host Maintenance §8.5](../spec/28-graceful-drain-handoff.en.md#85-spotwide-user-spot).

#### OBS-C4 Shutdown Performs Closing Callback And Session Close Without Relocation

Priority: `P1`

Shutdown isn't an operation that moves a stateful object to a
different node. It notifies the local Spot of a closing reason, closes
active Sessions with the formal close reason, then stops the Host.

**Verification question:** Are each Spot's `OnClosing(HostShutdown)`
and the client close reason observed exactly once during Shutdown?

- Start condition: Entry/User/Instance Spot and an active Stream
  Session are on the source Host.
- Procedure: Call public Shutdown and wait for the lifecycle callback,
  client close, and Host terminal.
- Verification: Each Spot callback runs once with reason
  `HostShutdown`, and application state can be read at callback time.
  The client receives the formal server-drain close reason, and the
  Host result is `Stopped/None`. No new object is created on the
  target node.
- Detailed behavior: verifies
  [Host Maintenance §10](../spec/28-graceful-drain-handoff.en.md#10-relocate-completion-and-failure).

#### OBS-C5 Keep The Source When There's No Eligible Target

Priority: `P1`

If there's no compatible target to receive relocation, the source
object must stay unchanged and preflight must return a blocked result.

**Verification question:** When no node satisfies target
version/type/capacity conditions, does the source keep processing
requests?

- Start condition: The stateful object is on `play-a`, and other nodes
  are one of absent, wrong version, missing type, or at full capacity.
- Procedure: Build each blocker on a fresh fixture and call public
  Relocate. Send a request to the source object after terminal.
- Verification: Relocate ends in the formal `Blocked` outcome with a
  blocker reason. The source Host is Serving, its public location and
  generation are kept, and the follow-up request succeeds. It doesn't
  auto-start Shutdown.
- Detailed behavior: verifies
  [Host Maintenance §6](../spec/28-graceful-drain-handoff.en.md#6-concurrent-calls-and-cancellation).

#### OBS-C6 Move To The Exact New Version Via Rolling Update

Priority: `P0`

An application patch first makes the new-version target ready, then
moves the source workload to that exact version.

**Verification question:** After a `TargetApplicationVersion=N+1`
Relocate, is every current object processed at the N+1 target?

- Start condition: A version N source and a compatible N+1 target are
  ready, and the source has an Actor, User Spot, Instance Spot, and a
  bound Session.
- Procedure: Call a RollingUpdate Relocate during ongoing request and
  push. After completion, run a request and push with the same IDs.
- Verification: The result is `Relocated/None`, mode `RollingUpdate`,
  effective version N+1. Current locations and handler evidence point
  at the N+1 target, and binding and object generation are kept. The
  source process stays Relocated, and a subsequent explicit Shutdown
  ends in Stopped.
- Detailed behavior: verifies
  [Host Maintenance §5](../spec/28-graceful-drain-handoff.en.md#5-selecting-a-target-matching-the-mode).

#### OBS-C7 Planned Maintenance Uses A Same-Version Target

Priority: `P0`

Node maintenance can move the workload to a compatible node of the
same version without changing the application version.

**Verification question:** Does PlannedMaintenance move version-N
workload to a different version-N target?

- Start condition: Both source and target are version N, and the
  target has the needed type/capacity.
- Procedure: Call a PlannedMaintenance Relocate while the source
  stateful object has an accepted request.
- Verification: The accepted request receives exactly one terminal
  result, and the Relocate result's effective version is N. The
  current object location and follow-up handler evidence point at the
  target, with state and generation kept.
- Detailed behavior: verifies
  [Host Maintenance §5](../spec/28-graceful-drain-handoff.en.md#5-selecting-a-target-matching-the-mode).

#### OBS-C8 Perform A Bounded Forced Teardown At The Shutdown Deadline

Priority: `P1`

Even if the Spot closing callback doesn't finish, Shutdown doesn't
wait indefinitely past the host deadline.

**Verification question:** If a closing callback is blocked at an
application gate, does Shutdown end in
`ForceStopped/DeadlineExceeded`?

- Start condition: Configure the Spot's `OnClosing` to leave
  entered-evidence, then wait on an application signal.
- Procedure: Call Shutdown with a positive deadline shorter than the
  gate, wait for terminal, then release the gate.
- Verification: The absolute deadline the callback received equals the
  Host deadline. The Host result is `ForceStopped/DeadlineExceeded`,
  and the forced-shutdown metric delta is 1. A late callback
  completion doesn't change the Host terminal.
- Detailed behavior: verifies
  [Host Maintenance §10](../spec/28-graceful-drain-handoff.en.md#10-relocate-completion-and-failure).

#### OBS-C9A Automatic Topology Starts Relocation Only After The Target Is Ready

Priority: `P0`

The mere fact that a descriptor is visible doesn't mean the target
transport is ready. Relocate must move the workload only after the
exact target becomes ready in public topology status.

**Verification question:** Does it keep the source while the target is
not-ready, and complete relocation once ready?

- Start condition: A compatible target process has started, but the
  runner has blocked the source-target network so public status is
  not-ready.
- Procedure: Start Relocate and send a source follow-up request while
  pending. Recover the network and confirm target ready.
- Verification: During the not-ready span, the source request is
  processed normally and the current location is the source. After
  the target is ready, Relocate succeeds and follow-up requests are
  processed at the target.
- Detailed behavior: verifies
  [Host Maintenance §6](../spec/28-graceful-drain-handoff.en.md#6-concurrent-calls-and-cancellation).

#### OBS-C9B Manual Topology Blocks Relocate At Preflight

Priority: `P0`

A manual connection Framework can't prove replacement readiness for
can't be used as an automatic handoff target.

**Verification question:** Does a manual-only topology's Relocate end
in `ManualTopologyUnsupported`, keeping the source?

- Start condition: There's a fresh Host with only a manual RouteMesh or
  ClientServer endpoint, and a stateful source object.
- Procedure: Call public Relocate and send a source request after
  terminal. Then call explicit Shutdown.
- Verification: Relocate has blocked reason
  `ManualTopologyUnsupported`, and the source request succeeds.
  Shutdown doesn't use manual topology as a blocker and ends in a
  bounded terminal.
- Detailed behavior: verifies
  [Host Maintenance §6](../spec/28-graceful-drain-handoff.en.md#6-concurrent-calls-and-cancellation).

#### OBS-C10 Select Only The Exact Version The Relocation Mode Defines

Priority: `P0`

Even with a higher weight, a target that doesn't pass the mode and
version filter can't be selected.

**Verification question:** Does PlannedMaintenance select only an N
target, and does a RollingUpdate N+1 request select only an N+1
target?

- Start condition: Compatible N, N+1, and N+2 targets are all ready,
  and the excluded targets are set with a higher weight.
- Procedure: Run PlannedMaintenance and RollingUpdate N+1 each on a
  fresh source.
- Verification: The first result and current location are the N
  target, and the second is the N+1 target. There's no handler
  evidence on the N+2 or wrong-version target.
- Detailed behavior: verifies
  [Host Maintenance §5](../spec/28-graceful-drain-handoff.en.md#5-selecting-a-target-matching-the-mode).

#### OBS-C11 Handle Concurrent Relocate Option Conflicts

Priority: `P0`

A caller with the same relocation intent can join and wait for the
in-progress operation's result together, but a different intent must
not change that operation's target or deadline.

**Verification question:** Do same-option callers receive the same
terminal, while a different-option caller gets
`OperationInProgress`?

- Start condition: A RollingUpdate N+1 target restore can be held at
  an application gate.
- Procedure: While the first Relocate is pending, start calls with the
  same option, PlannedMaintenance, and RollingUpdate N+2. Release the
  target gate.
- Verification: The two same-option waiters receive the identical
  terminal result, and relocation runs once. The other two calls are
  `Blocked/OperationInProgress`, and don't change the first
  operation's effective version or deadline.
- Detailed behavior: verifies
  [Host Maintenance §11](../spec/28-graceful-drain-handoff.en.md#11-the-race-between-shutdown-and-relocate).

#### OBS-C12 Distinguish Relocate Waiter Cancellation From Shutdown Contention

Priority: `P0`

One caller's await cancellation doesn't cancel the shared Host
operation. A concurrent Shutdown must get its own terminal result from
Relocate per the formal contention rule.

**Verification question:** Does only the joined waiter get cancelled,
while the Relocate and Shutdown operations each return exactly one
terminal?

- Start condition: A PlannedMaintenance target restore is held at an
  application gate.
- Procedure: Start the first Relocate and a second waiter of the same
  option, and cancel only the second waiter. Start Shutdown, then
  release the target gate.
- Verification: Only the second caller is cancelled, and the first
  Relocate waiter receives the spec's `ShutdownRequested` contention
  result or the already-confirmed relocation result. Shutdown ends
  exactly once in `Stopped` or `ForceStopped`, and the terminal value
  doesn't change on repeated status query either.
- Detailed behavior: verifies
  [Host Maintenance §11](../spec/28-graceful-drain-handoff.en.md#11-the-race-between-shutdown-and-relocate).

## 5. Completion Criteria

- A flow scenario only uses formal flow-record fields, and a metric
  scenario only uses the public metric reader.
- Relocation and Shutdown are judged by public Host status/result,
  object lookup, lifecycle callback, and client result.
- Location row, relocation manifest, Core peer table, internal
  generation, and private progress counter aren't an E2E assertion.
- A metric is compared as the delta before/after a scenario, without
  fixing the process cumulative value or log-flush order.
- Order is controlled by the application gate and public readiness,
  not dependent on a fixed settle sleep.
