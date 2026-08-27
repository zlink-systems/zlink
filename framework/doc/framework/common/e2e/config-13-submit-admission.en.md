<!-- framework-adapter-nav:start -->
[E2E table of contents](README.en.md) | [Previous: Channel Egress Routing](config-12-channel-egress-routing.en.md) |
[Next: Instance Spot Activation](config-14-instance-spot.en.md)
<!-- framework-adapter-nav:end -->

# Config 13 — One-Way Submit Admission

One-way send and publish don't return a payload. Normal completion
means that operation family's source admission accepted the message —
it doesn't mean remote handler execution finished. If the queue can't
accept immediately, it waits for capacity within the public send
deadline, and ends with exactly one of timeout or
Shutdown, whichever is confirmed first.

This config verifies the completion and failure of the public one-way
API across real processes. The client calls the role server's
application endpoint, and the role server starts send/publish/reply
via the Framework public API. Transport attempt count, private queue,
socket buffer size, and a test-only snapshot barrier aren't used.

## 1. Verification Scope

- Immediate admission, and admission after capacity recovers
- Bounded pending admission, deadline, Shutdown, and cancellation in supporting languages
- One-way semantics for Node/Channel/Spot/Actor/Session/STREAM/classic
  fanout
- Logical Multicast's partial delivery and the absence of a per-target
  result
- The target-selection difference between a direct logical target and
  a select-one Channel
- Route recovery after terminal, and the ban on automatic resubmission
- STREAM send ordering and one-shot use of a reply token

## 2. Deployment Configuration

| Role | Count | What it does and why it's separate |
|---|---:|---|
| Location Store | 1 | Provides automatic topology and global Spot/Actor location. |
| Admission caller | 1 | An Object Client that starts Node/Channel/Spot/Actor and Logical Multicast operations. |
| Mesh target | 2 | Provides Channel, Spot, Actor, and Logical Multicast handlers. Creates admission conditions via a public job queue cap and handler-start gate. |
| ClientServer target | 2 | Provides ClientServer send handlers and weighted select-one candidates. |
| Session gateway | 2 | Provides bound Session send, Session Actor relay, and server Stream send/reply. |
| Fanout publisher/subscriber | 1 each | Separately verifies classic fanout publish terminal and subscriber delivery. |
| Stream peer | 1 | Receives server messages and sends requests via the public stream connector. |
| E2E client | 1 | Calls only the role server's public application endpoint and Stream endpoint. |

Each target handler records the operation ID, sequence, and
application payload into evidence. The source endpoint can provide the
public awaitable's pending state and terminal result as application
operation state. This state doesn't expose a Framework internal waiter
or queue length.

## 3. Common Backpressure And Judgment Method

A backpressure scenario creates shared-permit capacity wait with public
`MaxQueuedApplicationJobs` and a handler-start gate. It reads only public effective-max,
reserved/queued, and waiter status. Pending is confirmed by bounded-polling whether
the public awaitable the source endpoint started isn't yet terminal.
If the needed pending state isn't created within the common setup
timeout, it ends as a scenario setup failure, without growing the
payload size, socket buffer, repeat count, or queue cap during the run.

Send terminal and remote execution are judged with separate evidence.
Confirm the normal send terminal first, then confirm handler
completion after opening the application gate. An operation that ended
in deadline or cancellation must not run on the handler even after the
gate and route recover.

Logical Multicast doesn't return a per-target delivery report. Only
the public result and the admittable target's handler evidence are
confirmed; the private snapshot member and admission attempt count are
verified by an internal test.

## 4. Scenarios

### Track A — Confirm The Public One-Way Terminal

#### SA-E2E-01 Submit Immediately To A Ready Target

Priority: `P0`

If the queue has capacity, a one-way call must return a normal
terminal with no payload.

**Verification question:** Does each one-way family complete normally
while ready, with the target handler running once?

- Start condition: Node direct, RouteMesh/ClientServer Channel, Spot,
  Actor, bound Session, Session Actor relay, Stream, and classic
  fanout targets are ready and the handler gate is open.
- Procedure: Start one send or publish with a unique operation ID per
  family.
- Verification: Each public awaitable completes normally with no
  result payload. The corresponding handler records the operation ID
  once.
- Detailed behavior: verifies normal send completion from
  [Error Model §4](../spec/server/00-foundation/07-framework-error-model.en.md#4-send-completion-and-failure).

#### SA-E2E-02 Accept A Pending Send Once Shared Job Capacity Recovers

Priority: `P0`

Even if all target shared permits are reserved, if a handler starts and returns one before
the deadline, the application must not call
the same operation again — the original awaitable must complete.

**Verification question:** After shared job capacity recovers, does the pending send
complete normally, processed at most once by the handler?

- Start condition: Set target `MaxQueuedApplicationJobs = 1` and close a handler-start
  gate. Reserve the permit with a blocker job and confirm reserved/queued 1 in public status.
- Procedure: The source sends the next marker and confirms its awaitable is pending. Open
  the gate so the blocker handler starts and returns its permit.
- Verification: The original send completes normally with no result
  payload, and the marker appears only once in handler evidence. The
  application doesn't call send again.
- Detailed behavior: verifies waiting for send-ready from
  [Error Model §4](../spec/server/00-foundation/07-framework-error-model.en.md#4-send-completion-and-failure).

#### SA-E2E-03 End A Pending Send In A Bounded Terminal

Priority: `P0`

Even during shared job capacity wait, a bounded set
of sends must each have a terminal result within its own deadline.
This scenario doesn't verify the internal pending waiter's size.

**Verification question:** Do sends pending on shared capacity not stay
indefinitely, each ending once in either success or
`DeadlineExceeded`?

- Start condition: Set `MaxQueuedApplicationJobs = 1` and a handler-start gate on two
  target processes. Send a blocker job to each and confirm reserved/queued 1. Set both send
  deadlines short and finite.
- Procedure: Start a send with a different operation ID to each
  target. Open the first target's gate before its deadline, and keep
  the second target's gate closed through its deadline.
- Verification: Every awaitable has exactly one terminal within the
  bounded time. The first target's marker appears at most once, and
  the second target's operation ends in `DeadlineExceeded`, absent
  from handler evidence.
- Detailed behavior: verifies
  [Error Model §4](../spec/server/00-foundation/07-framework-error-model.en.md#4-send-completion-and-failure).

#### SA-E2E-04 Late Capacity After Deadline Doesn't Revive The Operation

Priority: `P0`

If the send deadline ended first, subsequent queue capacity must not
submit a completed operation.

**Verification question:** After opening the gate following
`DeadlineExceeded`, is the previous marker not delivered to the
handler?

- Start condition: Configure `MaxQueuedApplicationJobs = 1` and the handler-start gate so a send
  becomes pending.
- Procedure: Keep the gate closed until the public send deadline ends.
  After confirming the `DeadlineExceeded` terminal, open the gate and
  send with a new operation ID.
- Verification: The previous marker is absent from handler evidence,
  and only the new marker is processed once.
- Detailed behavior: verifies blocking late admission from
  [Error Model §4](../spec/server/00-foundation/07-framework-error-model.en.md#4-send-completion-and-failure).

#### SA-E2E-05 Distinguish Target Absence From Route Disconnection

Priority: `P0`

The application must be able to distinguish the case where a logical
target doesn't exist from the case where the target exists but its
current route is unusable.

**Verification question:** Is a missing target `NotFound`, and a known
target's disconnected route `Unavailable`?

- Start condition: One ID is never created; another Actor/Spot is
  created, then the runner blocks its owner route.
- Procedure: Start one send each to the two logical IDs.
- Verification: The missing ID ends in exactly one `NotFound` terminal,
  and the known ID in exactly one `Unavailable` terminal. Neither
  marker appears in target handler evidence.
- Detailed behavior: verifies error mapping from
  [Error Model §4](../spec/server/00-foundation/07-framework-error-model.en.md#4-send-completion-and-failure).

#### SA-E2E-06 Honor The Relocate And Shutdown Admission Seal

Priority: `P0`

A send started after the host closes new-work acceptance must not
enter the queue.

**Verification question:** Does a new send started while Relocating or
ShuttingDown get rejected with the formal terminal?

- Start condition: The source host and target are ready, and an
  admission seal can be started via a public Host operation.
- Procedure: Run the Relocate variant and Shutdown variant on a fresh
  host each. Once public Host status stops accepting new work, start
  one send.
- Verification: The Relocate variant ends in that operation contract's
  rejection result, and the Shutdown variant in `ShuttingDown`, with
  no target handler evidence.
- Detailed behavior: verifies
  [Host State And Completion Results](../spec/server/05-location-relocation/05-host-relocation-flow.en.md#3-host-state-and-completion-results)
  and [Admission Per State](../spec/server/05-location-relocation/05-host-relocation-flow.en.md#15-admission-per-state).

#### SA-E2E-07 Distinguish An Admission Terminal From A Publish Commit

Priority: `P1`

If timeout or Shutdown ends pending admission first, the handler must
not run. Languages with public admission cancellation also run that
variant. Once Logical Multicast submit completes normally, ending the
caller scope does not roll back fanout already started.

**Verification question:** Do pre-commit timeout/Shutdown and supported-language cancellation block
delivery, while ending the caller scope after a normal publish terminal does not cancel existing
delivery?

- Start condition: Configure a normal send to become pending, and a
  multicast target handler to wait at the application gate.
- Procedure: End the pending send by timeout or Shutdown; supporting
  languages run cancellation on a separate fixture. Publish a separate
  multicast, receive a normal terminal, then end the caller scope
  and open the handler gate.
- Verification: There's no terminal send marker. The multicast marker
  is processed at most once at the accepted target, and the public
  publish terminal doesn't change.
- Detailed behavior: verifies
  [Spot Messaging §4](../spec/server/03-spot-actor/02-spot-messaging.en.md#4-channel-scoped-logical-multicast)
  and
  [Error Model §4](../spec/server/00-foundation/07-framework-error-model.en.md#4-send-completion-and-failure).

### Track B — Operation Families Use The Same Admission Semantics

#### SA-E2E-08 Compare Node Direct's Local And Remote Send

Priority: `P0`

Regardless of whether the target node is the same process or a
different one, send terminal means source admission.

**Verification question:** Do local and remote Node direct send
produce the same terminal and handler evidence?

- Start condition: The caller can send to both a local Node RID and a
  remote Node RID.
- Procedure: Send with the same payload semantics to the local and
  remote target each once.
- Verification: Both awaitables complete normally with no result
  payload, and each node's handler processes its own marker once.
- Detailed behavior: verifies
  [Interaction Model §3](../spec/server/00-foundation/04-interaction-model.en.md#3-node-direct-and-channel-select-one).

#### SA-E2E-09 Apply The Send Deadline For Each Channel Topology

Priority: `P0`

RouteMesh and ClientServer Channel sends must wait until the family
send deadline when queue capacity is unavailable, then end in the same
single public terminal.

**Verification question:** In both RouteMesh and ClientServer, is a
Channel send pending before capacity recovers, does it succeed when
capacity recovers before the deadline, and does it return the same
timeout result when capacity does not recover?

- Start condition: Configure RouteMesh and ClientServer as actual,
  separate topologies. In each topology, the success and timeout
  variants use different ChannelNames and target processes. Set a small
  `MaxQueuedApplicationJobs = 1` and a handler-start gate on each target, and use a blocker
  job to confirm reserved/queued and pending state.
- Procedure: For each topology, start a send to the success target and
  open its gate before the deadline. Keep the timeout target's gate
  closed through the deadline while a separate send is pending.
- Verification: In both topologies, the success send has a payload-less
  normal terminal and one handler execution. The timeout send is
  `DeadlineExceeded` with zero handler executions.
- Detailed behavior: verifies
  [Channel Messaging §7](../spec/server/02-channel-transport/02-channel-messaging.en.md#8-failure-and-termination)
  and
  [ClientServer Channel §6](../spec/server/02-channel-transport/03-client-server-channel.en.md#5-send-request-and-reply).

#### SA-E2E-11 Keep SpotId Send's Admission And Logical Identity

Priority: `P0`

Even if the route disappears while a Spot send is pending, the target
must not be swapped to a different Spot.

**Verification question:** Does a pending send that lost its original
Spot route end in `Unavailable`, without a different Spot processing
it?

- Start condition: `spot-a` is ready and can make a send pending.
  `spot-b` is the same stable type but a different ID.
- Procedure: Confirm `spot-a`'s send is pending, then terminate the
  owner route. Then recover the route and send a new operation ID.
- Verification: The previous operation is `Unavailable`, with no
  marker on either A or B's handler. Only the new operation is
  processed once, at A.
- Detailed behavior: verifies
  [Failover Policy §2](../spec/server/05-location-relocation/06-failure-failover-policy.en.md#2-common-judgment-criteria).

#### SA-E2E-12 Keep ActorId Send's Admission And Logical Identity

Priority: `P0`

An Actor direct send also keeps the original `ActorId`, and doesn't
resubmit a completed operation just because the route recovered.

**Verification question:** Does an operation that failed due to Actor
route loss not get automatically delivered after recovery, with only
the new send processed?

- Start condition: The Actor is ready and a pending send can be made.
- Procedure: Terminate the owner route while pending, confirm the
  terminal. After the route recovers to ready, send with a different
  operation ID.
- Verification: The previous send is `Unavailable`, with no handler
  marker. The new send completes normally, processed once by the
  Actor handler.
- Detailed behavior: verifies
  [Failover Policy §4.1](../spec/server/05-location-relocation/06-failure-failover-policy.en.md#41-logical-id-messaging-and-objectgeneration).

#### SA-E2E-13 Logical Multicast Processes Admittable Targets Once

Priority: `P0`

Logical Multicast delivers one-way to current matching targets and
doesn't return per-target success/failure results to the caller. Even
if some targets are unavailable, it doesn't roll back delivery to
already-admittable targets.

**Verification question:** Even with one matching target unavailable,
does another target process the marker once?

- Start condition: Targets A and B of the same subscription are ready.
  The runner makes B's route unavailable, then confirms via public
  status.
- Procedure: The source publishes a unique marker once.
- Verification: The public terminal completes with the formal meaning
  and no per-target result payload. A processes the marker once, and
  B doesn't. A variant with zero targets also completes normally. The
  per-target result of a partial delivery, where only some targets are
  processed, isn't returned or aggregated in the public result or in
  publish-dedicated monitoring. E2E doesn't read the private snapshot
  or attempt count.
- Detailed behavior: verifies
  [Spot Messaging §4](../spec/server/03-spot-actor/02-spot-messaging.en.md#4-channel-scoped-logical-multicast).

#### SA-E2E-14 Complete A Classic Fanout Publish Even With No Subscriber

Priority: `P0`

A classic fanout publish doesn't return subscriber count or delivery
acknowledgement.

**Verification question:** Does a publish with zero subscribers
complete normally, without replaying to a late subscriber?

- Start condition: The publisher is ready and the subscriber process
  hasn't started.
- Procedure: Publish a marker and confirm the terminal, then start the
  subscriber and let it become ready. Don't send a new marker.
- Verification: The publish completes normally with no result payload.
  The late subscriber's handler has no previous marker.
- Detailed behavior: verifies
  [Framework API §11](../spec/server/00-foundation/06-framework-api.en.md).

#### SA-E2E-15 Compare Bound Session's And Session Actor Relay's Local/Remote Results

Priority: `P0`

The one-way terminal semantics must not change based on whether the
Session owner and Actor owner are local or remote.

**Verification question:** Do local/remote bound Session send and
Actor relay use the same deadline and non-replay rule?

- Start condition: Configure a local binding and a remote binding on
  fresh Sessions each. The pending variants place each Session on a
  separate gateway process so they don't share host job capacity, and create the capacity
  wait with that gateway's `MaxQueuedApplicationJobs = 1` and handler-start gate.
- Procedure: Run a normal send once for each of the four combinations,
  and a separate pending send that doesn't open capacity through the
  deadline.
- Verification: Normal sends complete with no result payload, and
  target evidence appears once each. Pending sends are
  `DeadlineExceeded` and don't replay after a later capacity recovery.
- Detailed behavior: verifies
  [One-Way Submit](../spec/server/01-execution/README.en.md),
  [Admission Deadline](../spec/server/01-execution/01-submit-and-completion.en.md), and
  [Session Actor Inbound Dispatch](../spec/server/04-session/02-session-actor-binding.en.md).

#### SA-E2E-16 Keep Server Stream Send Order

Priority: `P0`

Server sends accepted on the same Stream Session must keep the
application submission order. A send that failed through its public
per-call `Timeout(...)` modifier must not
later show up at the client.

**Verification question:** Does the Stream client receive only the
successful sequence in submission order?

- Start condition: The public stream connector is connected to the
  server Session, and the server send HWM is set small. Keep the
  server send gate closed, and set a long call timeout for the success
  marker and a shorter timeout for the `timeout` marker.
- Procedure: Start server sends in order `1`, `timeout`, `2`, `3`.
  After confirming the `timeout` operation ended by deadline, open the
  gate before the long deadline ends.
- Verification: The success sequence `1,2,3` the client received
  matches the source's successful-terminal order with no duplicates.
  The `timeout` marker doesn't arrive at the client.
- Detailed behavior: verifies
  [Async Execution — Per-STREAM-Send-Call Timeout](../spec/server/01-execution/README.en.md) and
  [Stream Session Codec-Layer Separation](../spec/server/04-session/01-stream-session.en.md).

#### SA-E2E-17 A Stream Reply Token Is Used Only Once

Priority: `P0`

A request reply token is used by whichever valid first reply call
uses it. Even if the first call ends in normal completion, socket send
timeout, or Shutdown, the same token cannot be reused. Cancellation is
an additional variant only in supporting languages.

**Verification question:** Of two calls on the same reply token, does
only one start admission, with the client reply also at most one?

- Start condition: A Stream peer sends a request and the server
  handler receives the public reply token.
- Procedure: Build two reply calls with the same token and start them
  at an application barrier simultaneously. Repeat the normal,
  socket-send-timeout and Shutdown variants on fresh requests, plus
  cancellation in supporting languages.
- Verification: For each request, only one call gets a normal or first
  terminal, and the other gets a local invalid-state error. The client
  reply is at most one, and reusing a terminal token doesn't
  produce a reply.
- Detailed behavior: verifies one-shot state from
  [Error Model §3](../spec/server/00-foundation/07-framework-error-model.en.md#3-errors-checkable-before-the-call).

### Track C — Confirm Target Selection And Post-Terminal Behavior

#### SA-E2E-18 Distinguish Direct Target From Channel Select-One

Priority: `P0`

A direct send keeps the logical identity the caller specified. A
Channel select-one can select one of the currently eligible members
when starting an operation.

**Verification question:** When a direct target is unavailable, does
it not swap to a different ID, while the Channel selects a remaining
ready member?

- Start condition: Direct target A's route is unavailable, and a
  different logical target B is ready. Of the same ChannelName's
  members, Server A is unavailable and Server B is ready.
- Procedure: Start one direct-A send and one ChannelName send each.
- Verification: The direct send is `Unavailable`, and logical target B
  doesn't process it. The Channel send completes normally, and ready
  Server B processes the marker once.
- Detailed behavior: verifies
  [Interaction Model §3](../spec/server/00-foundation/04-interaction-model.en.md#3-node-direct-and-channel-select-one)
  and
  [Failover Policy §2](../spec/server/05-location-relocation/06-failure-failover-policy.en.md#2-common-judgment-criteria).

#### SA-E2E-19 Route Recovery After Terminal Doesn't Resubmit The Operation

Priority: `P0`

An operation that ended in timeout, connection loss, or Shutdown does
not return to pending after route recovery. Languages with public
admission cancellation also run that variant.

**Verification question:** After route recovery, is the previous
marker not delivered, with only the new operation processed?

- Start condition: Keep a route unavailable long enough to end one
  send in terminal failure.
- Procedure: Confirm the failure terminal, then recover the route,
  and once public status is ready, send with a new operation ID.
- Verification: The previous marker is absent from target evidence,
  and only the new marker is processed once. The previous awaitable's
  terminal also doesn't change.
- Detailed behavior: verifies non-replay from
  [Transport Liveness §6](../spec/server/02-channel-transport/05-transport-liveness.en.md#6-connection-loss-and-reconnect).

#### SA-E2E-20 Separate Submit Completion From Remote Handler Completion

Priority: `P0`

If the application interprets send terminal as remote business
completion, it misses an actual handler failure or delay.

**Verification question:** Does send terminal complete first while the
remote handler is still waiting?

- Start condition: Channel, Spot, Actor, fanout subscriber, bound
  Session, and Stream target handlers wait at an application signal
  after receiving the marker.
- Procedure: Start a send per family and wait for the public terminal
  first. After confirming handler-entered evidence, send the release
  signal.
- Verification: Each send completes normally with no result payload,
  before handler completion. Handler completion is recorded once,
  after the gate opens.
- Detailed behavior: verifies source-admission completion from
  [Error Model §4](../spec/server/00-foundation/07-framework-error-model.en.md#4-send-completion-and-failure).

## 5. Completion Criteria

- Every procedure uses only the public one-way call, public Host/route
  status, and the role server's application evidence.
- Transport attempt, send-ready signal, private waiter, snapshot pass,
  socket buffer, and raw frame aren't an E2E pass condition.
- A pending state is created via the public awaitable's incomplete
  state combined with public `MaxQueuedApplicationJobs` and handler-start gate, and fails
  if not reproduced within the setup timeout. It isn't retried by
  changing a runtime value.
- Exactly one of normal terminal, timeout, cancellation, or Shutdown
  result occurs per operation.
- Capacity/route recovery after terminal doesn't automatically
  resubmit an existing operation.
- Public API shape and internal resource cleanup are separately
  verified by the per-language interface and contract test.
