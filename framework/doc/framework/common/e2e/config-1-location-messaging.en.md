<!-- framework-adapter-nav:start -->
[E2E table of contents](README.en.md) | [Next: Spot Service](config-2-spot-service.en.md)
<!-- framework-adapter-nav:end -->

# Config 1 — Location Store-Based Messaging

When an application calls the same feature offered by several
servers, it must not directly manage each server's endpoint. When a
provider starts or stops, Framework must check the current location in
the Location Store and select only a provider that can process a new
request. If this breaks, a target can go unfound even while the
server is running, or requests keep going to a terminated process.

This config runs provider and consumer as different processes to
verify that automatic discovery, peer connection, and Channel
messaging all work together. The client starts an action via the
consumer's business endpoint, and confirms the handler result at the
provider's public application endpoint. Consumer and provider use only
the public Framework API. So the E2E client doesn't directly handle
the Framework API, internal state, or a Location Store record.

## 1. Verification Scope

This config confirms the following behavior.

- Finding a peer endpoint using the current descriptor registered in
  the Location Store, and setting up a ready connection.
- Manual topology also provides the same identity check and messaging
  semantics.
- When a provider is added or removed, the current state is reflected
  in subsequent Channel target selection.
- Request, send, Node direct, timeout, missing handler, and message
  size limit end in the defined result.
- Even trying to create the same global Actor/Spot ID from different
  Meshes converges to exactly one current object.
- The client result, public status, and application evidence confirmed
  through the public endpoint across several processes are consistent
  with each other.

## 2. Deployment Configuration

The runner starts the following roles as separate processes. Only a
scenario verifying provider addition, termination, or crash changes
the process count.

| Role | Count | What it does and why it's separate |
|---|---:|---|
| Redis Location Store | 1 | Shares the provider's MeshNode descriptor and owner lease. Uses a different key prefix per run. |
| provider A/B | 2 | Processes the `profile` Channel's request and send. Each process leaves payload, RID, and handler count as evidence. Verifies discovery and target selection across different processes. |
| consumer | 1 | Executes an application request received via HTTP through the public Channel or Node direct API. Also provides public RouteMesh status via HTTP response. |
| Object Client pair | 2 | Verifies only the condition where a peer connection is needed between two Object Clients, in RM-A3. Doesn't share a process with other scenarios. |
| Object owner | 2 | Provides the same stable Actor/User Spot type on different Meshes and handles public manager calls and direct messages. |
| E2E client | 1 | Calls the consumer's business endpoint and the provider's application-evidence/control endpoint with the per-language public HTTP client. Doesn't call the Framework messaging API directly. |

Provider and consumer use the same `MeshName`, `profile-mesh`.
Providers A and B register the `profile` Channel as a Server role with
the default weight `100`. The consumer registers the same Channel as
a Client role. In automatic topology, each process uses the Location
Store, and in manual topology, the runner passes the provider endpoint
into the consumer configuration.

The two providers process the following fixture messages.

- On receiving `ProfileLookupReq`, return a `ProfileLookupRes`
  containing the request value, provider RID, and correlation marker.
- On receiving `ProfileCommandMsg`, record the command ID and provider
  RID into evidence once.
- On receiving `NodePingReq`, return a `NodePingRes` containing the
  target RID and source RID.
- If `ProfileLookupReq.Delay` is set, delay handler completion to
  reproduce a request timeout.

Every language processes the message payload via Framework's default
typed JSON path, with no separate codec registration.

## 3. Common Run And Judgment Method

The runner creates a fresh port, RID prefix, Redis key prefix, and log
directory per run. It doesn't reuse a previous run's process,
connection, or Store record in the next run.

The runner doesn't judge messaging readiness done just because a
process's port opened. It starts a client scenario only after the
consumer reports the needed peer and Channel target as `ready` in
public RouteMesh status. A state change is confirmed via the status
stream or a bounded evidence wait, not substituted with a fixed sleep.
The default wait time follows
[E2E README §2.1](README.en.md#21-local-e2e-wait-standard).

The client starts a message by calling the consumer's business
endpoint. Success is judged using the following evidence together.

- The client result confirms the reply payload, public `ErrorKind`,
  and completion timing.
- Consumer status confirms ready peer, Channel ready target, and peer
  state.
- Provider evidence confirms the payload the handler received,
  processing count, and provider RID.
- Structured log is only used when investigating the cause of a
  failure. It isn't used as a pass condition for a normal messaging
  scenario.

Provider evidence uses a unique per-scenario correlation marker. After
each scenario ends, the runner collects the client result and a
bounded counter, and confirms they weren't mixed with the next
scenario's marker. On failure, every process's log, client result, and
evidence are preserved.

## 4. Scenarios

### Track A — Find A Peer And Set Up A Ready Connection

#### RM-A1 Find A Provider Via The Location Store

Priority: `P0`

The consumer starts without knowing the provider's endpoint. Once the
provider registers its current connection info in the Location Store,
Framework reads that info, confirms the transport identity, and makes
the connection ready. Without this process, the application would have
to distribute and update the endpoint list directly.

**Verification question:** Even without the consumer specifying an
endpoint, does it find both providers via the Location Store and
receive the first request's reply?

- Start condition: Providers A and B have started in different
  processes and use the same run's Location Store key prefix. The
  consumer's configuration has no provider endpoint.
- Procedure: The runner waits until both providers are `ready` in
  consumer status, and the `profile` Channel's ready-target count is
  2. The client calls the consumer's profile-lookup endpoint once.
- Verification: The client receives a `ProfileLookupRes` containing
  the request marker and the selected provider RID. Exactly one of
  provider A or B's public application evidence records the same
  marker exactly once, and the other provider doesn't. The consumer's
  public status has both peers and two ready targets.
- Detailed behavior: verifies
  [RouteMesh Topology §6](../spec/07-channel-topology.en.md#6-how-to-find-a-peer-endpoint)
  and
  [Runtime Monitoring §2.2](../spec/24-runtime-monitoring.en.md#22-topology-state).

#### RM-A2 Connect A Provider Via A Manual Endpoint

Priority: `P0`

In a fixed deployment, the application can provide the peer endpoint
directly. Even in this case, Framework must perform the same identity
check as automatic topology, and keep only one ready connection even
if both sides connect at the same time.

**Verification question:** Even using a manual endpoint with no
Location Store, does public status show exactly one ready peer, and
does the same request produce the same result?

- Start condition: Start a provider and consumer with Object role
  `None` and fixed RIDs. This run doesn't register a Location Store.
- Procedure: In the first repetition, register the provider endpoint
  only on the consumer. In the second repetition, register each
  other's endpoint on both sides and start them at the same time.
  Call profile-lookup once each repetition, after exactly one ready
  peer.
- Verification: In both repetitions, the client receives a
  `ProfileLookupRes` of the same meaning, and the provider handler
  runs exactly once. Even in the bidirectional repetition, the
  consumer's public status shows exactly one ready peer of that RID.
- Detailed behavior: verifies the manual topology contract from
  [RouteMesh Topology §5.1](../spec/07-channel-topology.en.md#51-automatic-only-the-meshnode-with-the-smaller-rid-starts-the-connection)
  and
  [§6](../spec/07-channel-topology.en.md#6-how-to-find-a-peer-endpoint).

#### RM-A3 Whether An Object Client Pair Needs A Connection

Priority: `P0`

Two Object Clients host no Actor or Spot, so they don't need a peer
connection to receive object traffic. But if one of the two is a
RouteMesh Channel Server, that node must receive Channel messages, so
a connection is needed. Public status must distinguish this
difference as `not_required` versus `ready`.

**Verification question:** Does an Object Client pair correctly
distinguish `not_required` from `ready` depending on whether it has
Channel Server membership?

- Start condition: Run two Object Client processes using the same
  MeshName, separate from RM-A1.
- Procedure: Run each of the following configurations on both
  automatic and manual topology.
  1. Neither side registers RouteMesh Channel membership.
  2. Only a Channel Client membership is registered on one or both
     sides.
  3. A Channel Server membership at weight `100` is registered on one
     side.
  4. The same Server membership's weight is set to `0`.
- Verification: The first two configurations show public peer state as
  `not_required` and provide ready-peer count `0`. The latter two show
  peer state as `ready` and provide ready-peer count `1`. Weight `0` is
  excluded from new Channel target selection but doesn't remove the
  Server membership or the need for a connection. It's a failure if
  `not_required` is shown as `not_connected` or a topology fault.
- Detailed behavior: verifies
  [RouteMesh Topology §4.2](../spec/07-channel-topology.en.md#42-a-channel-call-can-start-without-a-local-server-role),
  [§5.1](../spec/07-channel-topology.en.md#51-automatic-only-the-meshnode-with-the-smaller-rid-starts-the-connection),
  and
  [Runtime Monitoring §2.2](../spec/24-runtime-monitoring.en.md#22-topology-state).

#### RM-A4 Use A New RID For A Provider Replacement

Priority: `P0`

Automatic topology's RID distinguishes process lifecycle. Even if a
provider of the same application role restarts, the previous RID and
connection mustn't be reused, so a late-arriving frame or state
doesn't get applied to the new process.

**Verification question:** After a provider gracefully stops and
restarts under the same role, does the consumer use a new RID's
connection?

- Start condition: Provider A v1 and the consumer are ready, and a
  baseline request has succeeded once.
- Procedure: The runner gracefully stops v1 and confirms via consumer
  status that v1 dropped out of the ready peer and Channel target. It
  starts provider A v2 with the same RID prefix and a new port. Right
  after v2 appears ready in consumer status, it calls profile-lookup
  with no application retry.
- Verification: The UUID suffix of the v2 RID shown in public status
  differs from v1 and is a lowercase canonical UUID v4 form. After the
  replacement, the request is recorded exactly once, only in v2's
  public application evidence. The consumer isn't restarted, and it's
  a failure if v1 remains a ready peer or target in public status.
- Detailed behavior: verifies
  [MeshNode §3.1](../spec/13-mesh-node.en.md#31-the-rid-used-by-automatic-discovery)
  and
  [Transport Liveness §6](../spec/29-transport-liveness.en.md#6-connection-loss-and-reconnect).

#### RM-A6 Isolate Different RouteMeshes

Priority: `P1`

Even if one process and Location Store use several RouteMeshes
together, each MeshName's peer and Channel target must not mix.
Otherwise, a scale change in one business area could change routing
in a different area.

**Verification question:** Even with `profile-mesh` and
`workflow-mesh` sharing the same Store, is each request processed only
by that Mesh's provider?

- Start condition: The providers of the two MeshNames and a consumer
  using both Channels are registered under the same Location Store key
  prefix.
- Procedure: The runner waits until both RouteMeshes are ready each.
  The client calls the profile and workflow business endpoints once
  each. Then it gracefully stops only the profile provider and calls
  both endpoints again.
- Verification: Each marker is recorded only in that Mesh's provider
  evidence. Removing the profile provider doesn't change
  `workflow-mesh`'s status, ready-target count, or handler evidence.
- Detailed behavior: verifies the MeshName isolation contract from
  [RouteMesh Topology §3](../spec/07-channel-topology.en.md#3-meshname-and-meshnode).

#### RM-A7 Global Actor/Spot Identity Conflict

Priority: `P0`

Actor ID and Spot ID are global identities used across the whole
Location Store namespace, not a per-Mesh address. Even creating the
same ID at the same time from different Meshes must not show the
application two objects.

**Verification question:** Does concurrent GetOrCreate from different
Meshes converge to exactly one current Actor and Spot?

- Start condition: The Object owners of `profile-mesh` and
  `workflow-mesh` provide the same stable Actor/User Spot type, and the
  target ID is Missing.
- Procedure: The two role servers call public GetOrCreate concurrently
  with the same Actor ID and Spot ID. After each terminal, run manager
  `Find` and a global-ID direct request.
- Verification: Exactly one current ref is returned per ID, and a
  direct request is processed only once at that ref's owner. An
  operation requesting a different type/kind ends in the contracted
  mismatch result and doesn't build a second object.
- Detailed behavior: verifies
  [Location Runtime](../spec/21-location-runtime.en.md) and
  [Spot Address Messaging](../spec/16-spot-address-messaging.en.md).

### Track B — Changing Provider Count And Lifecycle

#### RM-B1 Add A Provider While Traffic Is Being Processed

Priority: `P0`

If adding a provider required restarting the consumer, automatic
discovery's operational benefit would disappear. Framework must
include a new provider as a selection candidate for subsequent
requests once it's ready.

**Verification question:** While provider A is processing requests, if
B is added, does B also process subsequent requests without a consumer
restart?

- Start condition: Only provider A is a ready target, and all 10
  baseline requests were processed at A.
- Procedure: The runner starts provider B. Once B is a ready peer in
  consumer status and the `profile` Channel's ready-target count is 2,
  the client sends 40 requests with unique markers.
- Verification: Markers before the addition are only at A. After the
  addition, both A and B each process at least one, and the sum of
  both providers' handler counts is 40. All 40 client requests end
  with exactly one reply.
- Detailed behavior: verifies ready-target selection from
  [RouteMesh Topology §7](../spec/07-channel-topology.en.md#7-ready-state-and-channel-target-selection).

#### RM-B2 Exclude A Gracefully-Stopped Provider From The Target

Priority: `P0`

Once a provider gracefully stops, Framework must exclude that provider
from new Channel targets. Without confirming this state, the consumer
could keep sending requests to an already-stopped endpoint.

**Verification question:** After provider B's graceful stop completes,
is a new request processed only at the remaining A?

- Start condition: Providers A and B are ready targets, and both
  processed the baseline request.
- Procedure: The runner requests a host shutdown on B. It waits until
  B's terminal host status is `stopped` and the consumer status's
  ready-target count is 1. It then sends 20 requests.
- Verification: All 20 sent after the stop are recorded exactly once
  at A's evidence, and the client receives a normal reply for all. B
  has no marker after the stop, and B doesn't remain a ready peer or
  target in the consumer's public status.
- Detailed behavior: verifies
  [MeshNode §8](../spec/13-mesh-node.en.md#8-drain-and-shutdown)
  and
  [Transport Liveness §7](../spec/29-transport-liveness.en.md#7-location-store-and-host-termination).

#### RM-B3 Use The Remaining Provider After A Provider Crash

Priority: `P0`

Even if one provider terminates abnormally, another ready provider
must keep processing new requests. However, since automatically
re-running a request A may have accepted before the crash on B could
cause a duplicate side effect, Framework doesn't replay that operation
to B.

**Verification question:** After provider A crashes and is excluded
from the target, is a new request processed at B, without an in-flight
request being auto-rerun at B?

- Start condition: Only provider A is a ready target, and evidence that
  a controllable request has started at A's handler is used as a
  barrier.
- Procedure: With A's handler not yet finished, the runner starts
  provider B. Once both A and B are ready targets in consumer public
  status, it force-kills process A. It collects the in-flight
  request's terminal result. It waits until A is excluded from the
  ready peer and target in consumer status, then sends 20 new
  requests.
- Verification: The in-flight request ends exactly once in either
  `Unavailable` or `DeadlineExceeded` at the configured deadline, and
  B's evidence has no matching marker. All 20 sent after A's exclusion
  are each processed once at B and receive a normal reply. The
  consumer isn't restarted, and it's a failure if there's an infinite
  wait or automatic replay.
- Detailed behavior: verifies
  [Failure Response §3](../spec/31-failure-failover-policy.en.md#3-channel-target-and-connection-failure),
  [Transport Liveness §6](../spec/29-transport-liveness.en.md#6-connection-loss-and-reconnect),
  and
  [Error Model §5](../spec/32-framework-error-model.en.md#5-request-completion-and-failure).

### Track C — Process A Message On A Ready Connection

#### RM-C1 Distinguish Request And Send Completion Semantics

Priority: `P0`

A request only completes once it receives a reply, but a send
completes with no result once the source outbound queue accepts the
message. Interpreting send completion as remote handler completion
could make the application wrongly conclude a side effect that hasn't
happened yet.

**Verification question:** Does a request end with a typed reply, does
a send complete with no result, and does provider evidence record both
messages exactly once each?

- Start condition: Providers A and B are ready targets, with no
  existing evidence for the scenario marker.
- Procedure: The client calls the consumer's profile-lookup endpoint,
  then the command endpoint. After the send completes, it separately
  waits for command processing via the provider's bounded evidence
  wait.
- Verification: The request client receives a reply containing the
  marker and provider RID. The send client completes normally with no
  reply payload. The provider's request and command evidence each
  record their marker exactly once.
- Detailed behavior: verifies
  [Error Model §4](../spec/32-framework-error-model.en.md#4-send-completion-and-failure)
  and
  [§5](../spec/32-framework-error-model.en.md#5-request-completion-and-failure).

#### RM-C2 A Node Direct Request Specifying An RID

Priority: `P0`

An operational or infrastructure call can specify an exact MeshName
and RID instead of letting the Channel auto-select a provider.
Framework must not swap the specified target for a different RID.

**Verification question:** Is a Node direct request processed only at
the specified provider, and does a non-existent RID end in
`NotFound`?

- Start condition: Consumer status has confirmed providers A's and B's
  RID, both ready.
- Procedure: The client sends a `NodePingReq` specifying B's RID
  through the consumer endpoint. Then it sends the same request with
  `api-missing`, an RID absent from the member snapshot.
- Verification: The first request is recorded exactly once, only in
  B's evidence, and receives a reply containing B's RID. The second
  request ends exactly once in `NotFound`, running no provider
  handler.
- Detailed behavior: verifies
  [Channel Messaging §3.1](../spec/08-channel-messaging.en.md#31-node-direct)
  and
  [Error Model §5](../spec/32-framework-error-model.en.md#5-request-completion-and-failure).

#### RM-C3 Distribute Requests Across Providers Of The Same Weight

Priority: `P0`

If two ready providers offer the same Channel, Framework selects one
of them as each operation's target. Since the exact alternating order
per request isn't part of the public contract, this is judged by
overall processing count and both providers' participation.

**Verification question:** Sending requests to two providers of the
same weight, do both process them, with the total handler count
matching the request count?

- Start condition: A and B are ready Channel targets at weight `100`.
- Procedure: The client sends 400 profile-lookups with different
  markers.
- Verification: The sum of both providers' unique markers is 400, with
  no duplicate marker. Each provider processes 35–65% of the total,
  140–260 each. The client receives 400 replies. The exact per-request
  alternating order isn't used as an assertion.
- Detailed behavior: verifies the select-one contract from
  [Channel Messaging §3.2](../spec/08-channel-messaging.en.md#32-channelname-select-one).

#### RM-C4 Discard A Late Reply After Timeout

Priority: `P0`

Even if the caller's deadline ends first, the remote handler may
already be running. Framework must not use a late-arriving reply as
the result for a subsequent request, or complete a finished request a
second time.

**Verification question:** After a slow request ends in
`DeadlineExceeded`, do two normal requests each receive their own
reply?

- Start condition: The provider leaves handler start and completion
  per marker as evidence, and a delay-free baseline request has
  succeeded.
- Procedure: The client sends a slow request with a deadline shorter
  than the handler delay. Right after receiving the timeout terminal,
  it sends two normal requests with different markers in sequence. The
  provider's slow handler completion is also confirmed via a bounded
  evidence wait.
- Verification: The first request ends exactly once in
  `DeadlineExceeded`. The following two requests each receive a reply
  matching their marker. It's a failure if the late reply adds to the
  client result or links to a different correlation marker.
- Detailed behavior: verifies the timeout and late-reply contract from
  [Error Model §5](../spec/32-framework-error-model.en.md#5-request-completion-and-failure).

#### RM-C5 Handle A Message With No Handler

Priority: `P0`

If a provider has no handler for a packet, a request caller must
receive a result saying the target business can't be processed. A
send may already have completed normally once the source queue
accepted it, so the provider records the dispatch result it received
via the public message-flow observer callback into application
evidence.

**Verification question:** Does a request for an unregistered packet
end in `NotFound`, and does a send not run an application handler?

- Start condition: The provider has no handler for the unknown packet
  identity used in this scenario.
- Procedure: The client sends one unknown request through the consumer
  endpoint, then one unknown send with a different marker. Then it
  sends a normal profile request.
- Verification: The unknown request ends exactly once in `NotFound`,
  and observer evidence records `no_handler` and `reply_error`. The
  unknown send ends with no reply payload, and observer evidence
  records `no_handler` and `drop`. Both messages have `0` application
  handler executions. The normal request is unaffected and receives a
  reply.
- Detailed behavior: verifies
  [Channel Messaging §5](../spec/08-channel-messaging.en.md#5-how-to-find-and-run-a-handler),
  [Error Model §4](../spec/32-framework-error-model.en.md#4-send-completion-and-failure),
  and
  [Message Flow Tracing §3.1](../spec/26-message-flow-tracing.en.md#31-closed-values-shared-by-every-language).

#### RM-C7 Set The Provider Selection Ratio By Weight

Priority: `P1`

An application can set the selection ratio of providers offering the
same Channel via startup weight. This scenario starts two providers
with different weights from the start, verifying the long-run
selection ratio without separately waiting for runtime propagation.

**Verification question:** With A's weight `300` and B's weight `100`,
does A process about 75% of the total over enough requests?

- Start condition: A starts with startup weight `300`, and B with
  startup weight `100`; both are ready targets.
- Procedure: The client sends 800 profile-lookups with different
  markers.
- Verification: The sum of both providers' unique markers is 800, with
  no duplicate marker. A processes 65–85% of the total, 520–680. The
  client receives 800 replies. An exact per-request order or an exact
  3:1 result isn't required.
- Detailed behavior: verifies the positive-weight long-run selection
  ratio from
  [Channel Messaging §3.2](../spec/08-channel-messaging.en.md#32-channelname-select-one).
  Excluding/restoring weight at runtime is verified by
  [Config 5 RL-B4](config-5-resilience-lifecycle.en.md).

#### RM-C8 Verify RouteMesh SS Payload Integrity

Priority: `P1`

RouteMesh ServerServer (SS) doesn't provide a Framework-level
`MaxMessageSize` setting. This scenario leaves the SS transport without a
listener ceiling and verifies that payloads of several sizes round-trip
without corruption. The Core STREAM inbound ceiling for a StreamNode is a
separate contract and isn't tested by this scenario.

**Verification question:** Does RouteMesh SS process payloads of several
sizes without a separate Framework message-size setting?

- Start condition: The provider and consumer configure only the public
  RouteMesh topology and don't set a Framework `MaxMessageSize`. The
  provider records payload length and checksum in the reply and evidence.
- Procedure: The client sends 1 byte, 4 KiB, 256 KiB, and 1 MiB payloads as
  requests, followed by one more normal 1 byte request.
- Verification: Each payload request receives the same length and checksum,
  with the provider handler running once for each request. The final normal
  request also receives a reply, and no evidence shows a partial payload.
- Detailed behavior: verifies the SS boundary in
  [RouteMesh Topology §8](../spec/07-channel-topology.en.md#8-routemesh-ss-message-size),
  where Framework doesn't provide a message-size setting. The StreamNode
  ceiling is defined separately in
  [STREAM Session §4](../spec/19-stream-session.en.md#4-stream-socket-message-size).

#### RM-C9 Resume Receiving After Hitting Application HWM

Priority: `P2`

When the payload a provider's handler is processing reaches public
Application HWM, Framework stops receiving new application messages.
Once the handler finishes and pending payload drops below HWM,
receiving must resume on the same connection.

**Verification question:** When a provider's pending payload reaches
Application HWM, does public status show receive-stopped, and does it
show receive-resumed and normal request processing after the handler
finishes?

- Start condition: The provider's `ApplicationHwmBytes` is 1 MiB. The
  provider handler doesn't finish
  processing a 2 MiB command until released by the public application
  control endpoint. The baseline request has succeeded.
- Procedure: The client sends a 2 MiB command via the consumer once.
  After confirming handler start in the provider's public application
  evidence, it reads public host status. The client releases the
  handler via the provider's public control endpoint, and after
  confirming pending-payload decrease and receive-resume in public
  host status, sends a normal profile request.
- Verification: While the handler waits, public status's pending
  payload is at or above HWM and application receive paused is
  `true`. After handler completion, pending payload is `0` and
  application receive paused is `false`. The subsequent normal request
  receives a reply, the provider handler runs once, and public
  RouteMesh status stays ready. Outbound queue length or send-timeout
  occurrence count isn't judged.
- Detailed behavior: verifies
  [Framework API §2.1](../spec/06-framework-api.en.md#21-keeping-received-payload-from-growing-memory-indefinitely),
  [Runtime Monitoring §2.1](../spec/24-runtime-monitoring.en.md#21-host-state),
  and
  [Error Model §4](../spec/32-framework-error-model.en.md#4-send-completion-and-failure).

## 5. Completion Conditions

- The `P0` scenarios RM-A1, RM-A2, RM-A3, RM-A4, RM-A7, RM-B1, RM-B2,
  RM-B3, RM-C1, RM-C2, RM-C3, RM-C4, and RM-C5 all pass.
- Each scenario uses the client result together with role server
  evidence. A judgment needing public status uses an immutable
  snapshot or status stream the runtime-owning consumer or provider
  role server read.
- The client calls only the role server's public business/evidence/
  control endpoint. It doesn't directly call the Framework API, a
  Store provider, or a private record.
- Every wait ends based on public readiness, public status sequence, a
  role server's public application marker, or a bounded evidence wait.
  A fixed sleep to line up scenario order isn't used.
- Redis key and evidence marker are isolated per run, and after the
  run ends, its dedicated key is cleaned up or the disposable Redis
  instance is discarded.
- On failure, client result, consumer public status, and provider
  application evidence are preserved. Structured log is preserved
  only as diagnostic material investigating which process and stage
  failed.
