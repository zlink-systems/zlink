<!-- framework-adapter-nav:start -->
[E2E table of contents](README.en.md) | [Previous: Channel Egress Routing](config-12-channel-egress-routing.en.md) |
[Next: Instance Spot Activation](config-14-instance-spot.en.md)
<!-- framework-adapter-nav:end -->

# Config 13 — One-Way Submit Admission

One-way send and publish don't return a payload. Normal completion
means that operation family's source admission accepted the message —
it doesn't mean remote handler execution finished. If the queue can't
accept immediately, it waits for capacity within the public send
deadline, and ends with exactly one of timeout, cancellation, or
Shutdown, whichever is confirmed first.

This config verifies the completion and failure of the public one-way
API across real processes. The client calls the role server's
application endpoint, and the role server starts send/publish/reply
via the Framework public API. Transport attempt count, private queue,
socket buffer size, and a test-only snapshot barrier aren't used.

## 1. Verification Scope

- Immediate admission, and admission after capacity recovers
- Bounded pending admission, deadline, cancellation, and Shutdown
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
| Mesh target | 2 | Provides Channel, Spot, Actor, and Logical Multicast handlers. Creates admission conditions via public HWM and an application handler gate. |
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

A backpressure scenario creates a receive-stop condition with public
`ApplicationHwmBytes`, a deterministic payload size, and an
application gate. An internal waiter or queue limit is neither
configured nor read. Pending is confirmed by bounded-polling whether
the public awaitable the source endpoint started isn't yet terminal.
If the needed pending state isn't created within the common setup
timeout, it ends as a scenario setup failure, without growing the
payload size, socket buffer, or repeat count during the run.

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
  [Error Model §4](../spec/32-framework-error-model.en.md#4-send-completion-and-failure).

#### SA-E2E-02 Accept A Pending Send Once HWM Recovers

Priority: `P0`

Even if the target's application handler waits and hits HWM, if the
handler finishes before the deadline, the application must not call
the same operation again — the original awaitable must complete.

**Verification question:** After HWM recovers, does the pending send
complete normally, processed at most once by the handler?

- Start condition: Set public HWM to a small value, and keep a blocker
  handler processing a deterministic payload larger than HWM at the
  application gate. Send the blocker payload first to confirm handler
  entry, then confirm public status's Application receive paused is
  `true`.
- Procedure: The source endpoint sends the next payload, larger than
  HWM, and confirms the awaitable is pending. Open the blocker
  handler gate to recover HWM.
- Verification: The original send completes normally with no result
  payload, and the marker appears only once in handler evidence. The
  application doesn't call send again.
- Detailed behavior: verifies waiting for send-ready from
  [Error Model §4](../spec/32-framework-error-model.en.md#4-send-completion-and-failure).

#### SA-E2E-03 End A Pending Send In A Bounded Terminal

Priority: `P0`

Even while HWM has stopped remote application receiving, a bounded set
of sends must each have a terminal result within its own deadline.
This scenario doesn't verify the internal pending waiter's size.

**Verification question:** Do sends pending on HWM not stay
indefinitely, each ending once in either success or
`DeadlineExceeded`?

- Start condition: Prepare two different target processes, each with a
  public HWM, a deterministic payload larger than HWM, and a blocker
  handler gate. Send a blocker payload to each target first, to
  confirm handler entry and the Application receive paused state.
  Set both targets' send deadline short and finite.
- Procedure: Start a send with a different operation ID to each
  target. Open the first target's gate before its deadline, and keep
  the second target's gate closed through its deadline.
- Verification: Every awaitable has exactly one terminal within the
  bounded time. The first target's marker appears at most once, and
  the second target's operation ends in `DeadlineExceeded`, absent
  from handler evidence.
- Detailed behavior: verifies
  [Error Model §4](../spec/32-framework-error-model.en.md#4-send-completion-and-failure).

#### SA-E2E-04 Late Capacity After Deadline Doesn't Revive The Operation

Priority: `P0`

If the send deadline ended first, subsequent queue capacity must not
submit a completed operation.

**Verification question:** After opening the gate following
`DeadlineExceeded`, is the previous marker not delivered to the
handler?

- Start condition: Configure HWM and the handler gate so a send
  becomes pending.
- Procedure: Keep the gate closed until the public send deadline ends.
  After confirming the `DeadlineExceeded` terminal, open the gate and
  send with a new operation ID.
- Verification: The previous marker is absent from handler evidence,
  and only the new marker is processed once.
- Detailed behavior: verifies blocking late admission from
  [Error Model §4](../spec/32-framework-error-model.en.md#4-send-completion-and-failure).

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
  [Error Model §4](../spec/32-framework-error-model.en.md#4-send-completion-and-failure).

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
  [Graceful Drain §4](../spec/28-graceful-drain-handoff.en.md#4-conditions-checked-before-selecting-a-target)
  and
  [§5](../spec/28-graceful-drain-handoff.en.md#5-selecting-a-target-matching-the-mode).

#### SA-E2E-07 Distinguish A Cancellation Winner From A Publish Commit

Priority: `P1`

If cancellation ends a pending admission first, the handler must not
run. Once Logical Multicast's public submit has already completed
normally, a caller cancellation doesn't roll back a fanout already
started.

**Verification question:** Does a pre-commit cancellation block
delivery, while a cancellation after a normal publish terminal doesn't
cancel existing delivery?

- Start condition: Configure a normal send to become pending, and a
  multicast target handler to wait at the application gate.
- Procedure: Cancel the pending send awaitable. Publish a separate
  multicast, receive a normal terminal, then cancel the caller scope
  and open the handler gate.
- Verification: There's no cancelled send marker. The multicast marker
  is processed at most once at the accepted target, and the public
  publish terminal doesn't change.
- Detailed behavior: verifies
  [Spot Messaging §4](../spec/12-spot-messaging.en.md#4-channel-scoped-logical-multicast)
  and
  [Error Model §4](../spec/32-framework-error-model.en.md#4-send-completion-and-failure).

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
  [Interaction Model §3](../spec/03-interaction-model.en.md#3-node-direct-and-channel-select-one).

#### SA-E2E-09 Apply The RouteMesh Channel Send Deadline

Priority: `P0`

A RouteMesh Channel send must also wait until its family's send
deadline when queue capacity is unavailable, then end in exactly one
terminal.

**Verification question:** Is a Channel send pending before capacity
recovers, and does it succeed if capacity recovers before the
deadline?

- Start condition: Configure a success variant and a timeout variant
  with different ChannelNames and target processes. Set public HWM
  and a handler gate on each target, and send a blocker payload first
  to confirm pending and Application receive paused state.
- Procedure: Start a send to the success target and open its gate
  before the deadline; start a separate operation to the timeout
  target and keep its gate closed through the deadline.
- Verification: The success target's send completes normally with the
  handler run once. The timeout target's send is `DeadlineExceeded`
  with no handler marker.
- Detailed behavior: verifies
  [Channel Messaging §7](../spec/08-channel-messaging.en.md#7-failure-and-termination).

#### SA-E2E-10 Apply The ClientServer Channel Send Deadline

Priority: `P0`

ClientServer also uses the same public send terminal as RouteMesh.

**Verification question:** Does ClientServer send's capacity-recovery
and deadline result match RouteMesh Channel?

- Start condition: Configure a success variant and a timeout variant
  with different ClientServer ChannelNames and target processes, each
  with a handler gate and a small public HWM. Send a blocker payload
  to each target first to confirm handler entry and Application
  receive paused state.
- Procedure: Open the success target's gate before the deadline while
  keeping the timeout target's gate closed through the deadline,
  starting a separate send to each target.
- Verification: Success is a payload-less terminal with the handler
  once; timeout is `DeadlineExceeded` with the handler zero times.
- Detailed behavior: verifies
  [ClientServer Channel §6](../spec/09-client-server-channel.en.md#6-send-request-and-reply).

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
  [Failover Policy §2](../spec/31-failure-failover-policy.en.md#2-common-judgment-criteria).

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
  [Failover Policy §4.1](../spec/31-failure-failover-policy.en.md#41-logical-id-messaging-and-objectgeneration).

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
  [Spot Messaging §4](../spec/12-spot-messaging.en.md#4-channel-scoped-logical-multicast).

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
  [Framework API §11](../spec/06-framework-api.en.md#11-classic-fanout).

#### SA-E2E-15 Compare Bound Session's And Session Actor Relay's Local/Remote Results

Priority: `P0`

The one-way terminal semantics must not change based on whether the
Session owner and Actor owner are local or remote.

**Verification question:** Do local/remote bound Session send and
Actor relay use the same deadline and non-replay rule?

- Start condition: Configure a local binding and a remote binding on
  fresh Sessions each. The pending variants place each Session on a
  separate gateway process so they don't share the host-wide public
  HWM boundary, and create the capacity wait with that gateway's HWM
  and application gate.
- Procedure: Run a normal send once for each of the four combinations,
  and a separate pending send that doesn't open capacity through the
  deadline.
- Verification: Normal sends complete with no result payload, and
  target evidence appears once each. Pending sends are
  `DeadlineExceeded` and don't replay after a later capacity recovery.
- Detailed behavior: verifies
  [Session Actor Dispatch §5](../spec/20-session-actor-dispatch.en.md#5-actor-relocation-route-barrier)
  and
  [Error Model §4](../spec/32-framework-error-model.en.md#4-send-completion-and-failure).

#### SA-E2E-16 Keep Server Stream Send Order

Priority: `P0`

Server sends accepted on the same Stream Session must keep the
application submission order. A send that failed by deadline must not
later show up at the client.

**Verification question:** Does the Stream client receive only the
successful sequence in submission order?

- Start condition: The public stream connector is connected to the
  server Session, and the server send HWM is set small. Keep the
  server send gate closed, and set a long deadline for the success
  marker and a shorter deadline for the `timeout` marker.
- Procedure: Start server sends in order `1`, `timeout`, `2`, `3`.
  After confirming the `timeout` operation ended by deadline, open the
  gate before the long deadline ends.
- Verification: The success sequence `1,2,3` the client received
  matches the source's successful-terminal order with no duplicates.
  The `timeout` marker doesn't arrive at the client.
- Detailed behavior: verifies
  [Stream Session §5](../spec/19-stream-session.en.md#5-codec-layer-separation).

#### SA-E2E-17 A Stream Reply Token Is Used Only Once

Priority: `P0`

A request reply token is used by whichever valid first reply call
uses it. Even if the first call ends in timeout or cancellation, the
same token can't be used again.

**Verification question:** Of two calls on the same reply token, does
only one start admission, with the client reply also at most one?

- Start condition: A Stream peer sends a request and the server
  handler receives the public reply token.
- Procedure: Build two reply calls with the same token and start them
  at an application barrier simultaneously. Repeat the normal,
  timeout, and cancellation variants on fresh requests.
- Verification: For each request, only one call gets a normal or first
  terminal, and the other gets a local invalid-state error. The client
  reply is at most one, and reusing a timed-out/cancelled token doesn't
  produce a reply.
- Detailed behavior: verifies one-shot state from
  [Error Model §3](../spec/32-framework-error-model.en.md#3-errors-checkable-before-the-call).

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
  [Interaction Model §3](../spec/03-interaction-model.en.md#3-node-direct-and-channel-select-one)
  and
  [Failover Policy §2](../spec/31-failure-failover-policy.en.md#2-common-judgment-criteria).

#### SA-E2E-19 Route Recovery After Terminal Doesn't Resubmit The Operation

Priority: `P0`

An operation that ended in timeout, cancellation, or Shutdown doesn't
return to a pending state even after the route recovers.

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
  [Transport Liveness §6](../spec/29-transport-liveness.en.md#6-connection-loss-and-reconnect).

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
  [Error Model §4](../spec/32-framework-error-model.en.md#4-send-completion-and-failure).

## 5. Completion Criteria

- Every procedure uses only the public one-way call, public Host/route
  status, and the role server's application evidence.
- Transport attempt, send-ready signal, private waiter, snapshot pass,
  socket buffer, and raw frame aren't an E2E pass condition.
- A pending state is created via the public awaitable's incomplete
  state combined with public HWM/payload/application gate, and fails
  if not reproduced within the setup timeout. It isn't retried by
  changing a runtime value.
- Exactly one of normal terminal, timeout, cancellation, or Shutdown
  result occurs per operation.
- Capacity/route recovery after terminal doesn't automatically
  resubmit an existing operation.
- Public API shape and internal resource cleanup are separately
  verified by the per-language interface and contract test.
