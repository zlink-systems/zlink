<!-- framework-adapter-nav:start -->
[E2E Index](README.en.md) | [Previous: Registration And Codec](config-4-registration-codec.en.md) | [Next: Store Failure/Recovery](config-6-store-failure-recovery.en.md)
<!-- framework-adapter-nav:end -->

# Config 5 — Process Restart And Lifecycle Recovery

A service process goes through normal shutdown, crashes, replacement, and network disconnection. The
Framework must reflect the current ready target in public status, must not automatically resubmit an
already-finished operation to a new target, and must distinguish accepted work from new admission
during Host maintenance.

This config has the runner manipulate processes and the network from the outside, and confirms
results through public request/send, Host operations, status, and application evidence. Raw protocol
commands, private connection IDs, Store records, and internal relocation queues are not used.

## 1. Verification Scope

- Server restart, endpoint replacement, reconnect bursts, and rolling update
- Cancellation, in-flight crash, graceful shutdown, and runtime weight changes
- Repeated lifecycle, Store independence, fanout load, and observer failure
- Orderly disconnect, half-open connections, and terminal-once completion
- Relocation preflight, Session-binding fencing, in-move handoff ordering, and abort restoration

## 2. Deployment Configuration

| Role | Count | Purpose and reason for separation |
|---|---:|---|
| Location Store | 1 | Provides automatic provider discovery and object location. |
| Relocation Store | 1 | Preserves `PreserveStateWith` Actor/User Spot relocation state. |
| Channel provider | 2–3 | Provides a weighted Channel handler and runtime status. Can start a replacement with a different version and endpoint. |
| Object Server | 2–3 | Provides an Actor, Entry/User/Instance Spot factory, and relocation adapter. |
| Session gateway | 1 | Provides Stream Session and Actor binding. |
| E2E client | per scenario | Sends continuous traffic to the public application endpoint and Stream endpoint. |

The runner provides `SIGTERM`, `SIGKILL`, process restart, and network pause/blackhole. Each scenario
uses a fresh process and Store namespace. Application handler/adapter gates provide public evidence
and a release endpoint, without directly changing Framework state.

## 3. Common Run And Judgment Method

Recovery is confirmed together with the current ready targets in public status and the success of a
follow-up request. Each request uses a unique operation ID, allowing exactly one terminal — reply,
error, timeout, or cancellation. A send terminal is not treated as remote handler completion.

Readiness and lease boundaries use bounded polling computed from the configured timeout. Whether a
provider is reselected is not left to chance. When a specific replacement needs confirmation, another
provider's weight is set to 0, public status propagation is confirmed, then a directed verification
window is run.

## 4. Scenarios

### Track A — Handle Process Restart And Replacement

#### RL-A1 Restart A Server At The Same Endpoint

Priority: `P1`

Even if the server restarts with a new lifecycle at the same endpoint after a normal shutdown, the
consumer does not need to restart.

**Verification question:** Is a down-window request a bounded failure, while a request right after
the new server is ready succeeds?

- Starting condition: The sole provider is ready, and a baseline request succeeded.
- Procedure: Public Shutdown is called on the provider, and the absence of a current target is
  confirmed. A down-window request is sent once, and a replacement is started at the same endpoint.
  A follow-up request is sent right after it's ready.
- Verification: The down-window request ends exactly once, in `NotFound` or a formal route terminal,
  with no automatic resubmission. The follow-up request is processed exactly once by the replacement
  handler, and the consumer process is kept.
- Detailed behavior: verifies [Transport Liveness §6](../spec/29-transport-liveness.en.md).

#### RL-A2 Switch To A Replacement At A Different Endpoint

Priority: `P2`

Pod replacement can change both the endpoint and the Node RID. The consumer must follow the current
automatic descriptor.

**Verification question:** After the old provider crashes, does only the new-endpoint replacement
process follow-up requests?

- Starting condition: The old provider is the sole target and has entered a slow request handler.
- Procedure: The old provider is force-terminated, and the pending request's terminal is collected. A
  replacement is started at a different endpoint, readiness is confirmed, and 20 requests are sent.
- Verification: The pending request ends exactly once, in either `Unavailable` or
  `DeadlineExceeded`. All 20 follow-ups are processed by the replacement, with no automatic
  resubmission to the old endpoint.
- Detailed behavior: verifies [Failover Policy §3](../spec/31-failure-failover-policy.en.md).

#### RL-A3 Many Clients Reconnect After A Server Restart

Priority: `P1`

Even with many concurrently connected clients, recovery after a server restart must work through the
Framework connector's reconnect.

**Verification question:** Do 100 clients each complete a request exactly once after the replacement
is ready, with no separate reconnect loop?

- Starting condition: 100 clients each succeeded with a baseline request.
- Procedure: The server shuts down normally, and a replacement is started. Each connector's public
  ready state is confirmed with bounded polling, then a unique request is sent from each.
- Verification: Every connector is ready within the common reconnect timeout, and 100 replies arrive,
  one per operation ID. The Application does not repeatedly call reconnect.
- Detailed behavior: verifies [Transport Liveness §6](../spec/29-transport-liveness.en.md).

#### RL-A4 Keep A Serving Target During A Rolling Or Blue-Green Update

Priority: `P2`

Old providers must be excluded one at a time only after the new-version targets are confirmed ready,
so the target count never drops to 0 mid-switch.

**Verification question:** Even while swapping the old set for the new set under continuous requests,
does completion happen with no lost terminal, with only the new version processing afterward?

- Starting condition: Version-N providers are ready, and a continuous request workload is running.
- Procedure: N+1 providers are started, and readiness is confirmed with public status and direct
  requests. The rolling variant Relocates/Shuts down one old provider at a time; the blue-green
  variant terminates the entire blue set in order once the entire green set is ready.
- Verification: Each request receives exactly one terminal, and the serving-target count never
  reaches 0. After completion, new requests are recorded only in N+1's handler evidence.
  Descriptor discovery alone is not treated as ready.
- Detailed behavior: verifies [Host Maintenance §5](../spec/28-graceful-drain-handoff.en.md).

#### RL-A5 Converge On The Current Target Even With Repeated Provider Lifecycle

Priority: `P2`

Even if a provider repeatedly terminates and restarts, previous lifecycles must not accumulate in the
ready list.

**Verification question:** Even after restarting provider B five times, do only A and the current B
process requests?

- Starting condition: A and B are ready.
- Procedure: B Shutdown, status removal, and B-replacement-ready are repeated five times. During each
  down window, requests go to A; once B is ready, A's weight is set to 0 to confirm a B-directed
  request, then restored.
- Verification: Down-window requests are processed by A, and each replacement verification is
  processed by the current B. Public status never keeps a previous B RID as ready.
- Detailed behavior: verifies [Runtime Monitoring §3](../spec/24-runtime-monitoring.en.md).

### Track B — Distinguish In-Flight Operations From New Admission

#### RL-B1 A Follow-Up Request Is Processed After A Client Cancellation

Priority: `P1`

Even if the caller cancels a pending request's await, a late server reply must not complete the next
request.

**Verification question:** After a canceled request, does the same client's new request receive only
its own reply?

- Starting condition: The provider handler holds the first request's reply on an application gate.
- Procedure: After the first request reaches the handler, the caller's await is canceled. A request
  with a new operation ID is sent and its reply received, then the first gate is released.
- Verification: The first awaitable is a cancellation, and the second request receives its own
  payload reply exactly once. The late first reply does not change the second's completion.
- Detailed behavior: verifies [Error Model §5](../spec/32-framework-error-model.en.md).

#### RL-B2 Handle A Provider Crash During An In-Flight Handler

Priority: `P1`

If a provider crashes after accepting a request, whether it was processed can be ambiguous. The
Framework does not automatically resubmit the same operation to a different provider.

**Verification question:** Does the crashed request end in exactly one terminal, with only the
follow-up processed by a different provider?

- Starting condition: A and B are ready, and a slow request has entered B's handler.
- Procedure: B is force-terminated, and the request's terminal is awaited. The client sends a
  follow-up request with a new operation ID.
- Verification: The old request ends exactly once, in either `Unavailable` or `DeadlineExceeded`. The
  same ID is not processed by A, and only the follow-up is processed by A exactly once.
- Detailed behavior: verifies [Failover Policy §2](../spec/31-failure-failover-policy.en.md).

#### RL-B3 Remove From Topology After A Graceful Shutdown

Priority: `P1`

A normal shutdown must stop new target selection, finish accepted requests within a bound, and then
leave the current topology.

**Verification question:** After provider B shuts down, does B drop out of ready targets while the
accepted reply is preserved?

- Starting condition: A slow request has been accepted by B's handler, and A is also ready.
- Procedure: B's Shutdown is started, new requests are sent, then the slow handler's gate is
  released.
- Verification: The accepted request completes exactly once with B's reply. New requests after the
  seal go to A, and after B's terminal, public status no longer keeps B as a ready target.
- Detailed behavior: verifies [Host Maintenance §10](../spec/28-graceful-drain-handoff.en.md).

#### RL-B4 Exclude From New Selection With Runtime Weight 0, Then Restore

Priority: `P0`

A Channel weight of 0 excludes only that membership from new select-one, without terminating the
process or connection.

**Verification question:** After the weight-0 change propagates, does B not receive new requests, and
does it process them again after restoring to 100?

- Starting condition: A and B are ready at weight 100.
- Procedure: B's weight is changed to 0, and after status propagation, 50 requests are sent. B's
  weight is restored to 100, A's weight is set to 0 for a directed verification request, then A is
  also restored.
- Verification: The first window is processed only by A, and the directed verification is processed
  by B. B's process and connection are kept throughout.
- Detailed behavior: verifies [Channel Topology §4.3](../spec/07-channel-topology.en.md).

#### RL-B5 Finish A Request Accepted Before A Weight Change

Priority: `P0`

A weight update changes only new target selection — it does not cancel a request B's handler has
already accepted.

**Verification question:** Even after B's weight changes to 0, does the existing slow request finish
with B's reply?

- Starting condition: A slow request has entered B's handler and waits on the reply gate.
- Procedure: B's weight is changed to 0, new requests are confirmed processed by A, then the gate is
  released.
- Verification: The slow request receives B's reply exactly once, and new requests are processed by
  A.
- Detailed behavior: verifies connection retention and weight update in [Channel Topology §5.1](../spec/07-channel-topology.en.md).

#### RL-B6 Isolate One Provider's Gray Failure From Other Replies

Priority: `P1`

Even if one provider returns an error or timeout for some inputs, another provider's correlation and
payload must not mix in.

**Verification question:** Do A's normal replies and B's deterministic error/timeout each correspond
exactly to their own requests?

- Starting condition: A is normal, and B returns `InternalFailure` or a delayed timeout depending on
  marker parity. The weights are the same.
- Procedure: 200 requests with unique markers are sent with bounded concurrency.
- Verification: IDs processed by A end in exactly one normal reply, and IDs processed by B end in
  exactly one configured error or timeout. There is no automatic resubmission to a target with no
  handler evidence.
- Detailed behavior: verifies request correlation and provider-failure isolation in
  [Channel Messaging](../spec/08-channel-messaging.en.md) and [Error Model](../spec/32-framework-error-model.en.md).

### Track C — Clean Up Public Resources After Shutdown And Store Lifecycle

#### RL-C1 Shut Down Normally After Many Connections

Priority: `P1`

Internal resource-cleanup counts are not an E2E public contract. The E2E confirms that every public
operation reaches a terminal, the process shuts down within a bound, and a replacement can use the
same ports.

**Verification question:** Does Shutdown complete after load, with a replacement starting normally on
the same listeners?

- Starting condition: Many Stream connections and requests have completed.
- Procedure: Clients and the server are shut down through public close/Shutdown. After process exit,
  a replacement is started on the same ports.
- Verification: There are no pending public operations, the old process exits, and the replacement's
  listeners become ready.
- Detailed behavior: verifies [Host Maintenance §10](../spec/28-graceful-drain-handoff.en.md).

#### RL-C2 Exclude A Crashed Provider After Its Owner Lease Expires

Priority: `P2`

Even a crash that couldn't remove its descriptor must drop out of ready targets once the current
lease expires.

**Verification question:** After provider B crashes, does only A process follow-up requests?

- Starting condition: A and B are ready.
- Procedure: B is force-terminated, the configured lease/status convergence is awaited, then 20
  requests are sent.
- Verification: Public status does not show B as ready, and A processes all 20.
- Detailed behavior: verifies [Location Runtime §5](../spec/21-location-runtime.en.md).

#### RL-C3 Converge On The New Lifecycle After A Normal Restart

Priority: `P2`

A normal process restart removes the old ready identity and makes the replacement identity current.

**Verification question:** After the restart, does the old RID drop out of the ready list, with the
replacement processing requests?

- Starting condition: The provider is ready.
- Procedure: The provider's Shutdown terminal is confirmed, and a replacement is started at the same
  endpoint.
- Verification: Public status shows only the replacement RID as ready, and follow-up requests are all
  recorded in the replacement's evidence.
- Detailed behavior: verifies [Runtime Monitoring §2.2](../spec/24-runtime-monitoring.en.md).

#### RL-C4 Keep Existing Messaging During A Location Store Restart

Priority: `P1`

An established connection's liveness is independent of Store polling.

**Verification question:** Do existing Channel requests keep being processed during a Store restart,
with status returning to Ready after recovery?

- Starting condition: A and the consumer connection are ready.
- Procedure: The Location Store is restarted during continuous requests. Results are collected while
  public status converges through degraded and ready.
- Verification: Existing-route requests each receive a terminal, and the follow-up succeeds after
  recovery. A Store failure is not turned into target-missing.
- Detailed behavior: verifies [Transport Liveness §7](../spec/29-transport-liveness.en.md).

### Track D — Isolate Load And Observability Failures

#### RL-D1 Isolate Subscribers From Each Other Under High Fanout

Priority: `P2`

Instead of a vague "stable under high load," the subscriber count and application marker are fixed.
Cross-subscriber ordering and lossless delivery in Classic fanout are not this scenario's contract.

**Verification question:** Do 20 ready subscribers independently observe the same marker, with one
subscriber's processing not blocking another?

- Starting condition: 20 subscribers see the publisher as ready. Only one subscriber's
  `fanout-start` handler waits on an application gate, with the rest left open. The start marker is
  kept small, with no network block introduced.
- Procedure: The publisher sends the `fanout-start` marker, the selected subscriber's gate is
  confirmed waiting, and load events are sent at a bounded rate. The other subscribers' marker
  evidence is confirmed, then the gate is opened.
- Verification: Each subscriber's evidence records the `fanout-start` marker, and one subscriber's
  processing delay does not block another subscriber's public ready state or marker processing.
  Event-sequence completeness/order and drop counts are not judged.
- Detailed behavior: verifies [Framework API §11](../spec/06-framework-api.en.md).

#### RL-D2 Isolate An Observer Failure From Messaging

Priority: `P1`

Even if the public message-flow observer throws an exception, handler dispatch must keep going, and
the runtime error sink must report the observer failure.

**Verification question:** After a failing observer, do normal requests succeed with exactly one
error-sink event?

- Starting condition: An observer and the public runtime error sink are registered.
- Procedure: The first observer callback throws an exception, and 20 normal requests are sent.
- Verification: All 20 replies succeed, and the error sink provides exactly one formal
  `observer_failed` event. The event does not include the payload or exception object.
- Detailed behavior: verifies [Message Flow Tracing §5](../spec/26-message-flow-tracing.en.md).

#### RL-D3 Confirm A Dispatch Error In A Public Logging Sink

Priority: `P1`

If the dispatch-error log is an observation contract, it must be judged by formal fields, not
implementation-specific strings.

**Verification question:** Does a request with no handler leave formal error fields in the public
logging sink?

- Starting condition: A logging sink and a normal handler are registered.
- Procedure: A missing-handler request and a normal request are each sent once.
- Verification: The sink provides `dispatch_error`, `no_handler`, and `reply_error` fields for the
  negative operation, and the normal request succeeds.
- Detailed behavior: verifies [Runtime Monitoring §5](../spec/24-runtime-monitoring.en.md).

#### RL-D4 Same-Version Peers Preserve The Public Error Kind

Priority: `P2`

The E2E does not read raw reply frames or JSON keys directly. The public exception kind and message
the caller received are cross-checked against the error the server made.

**Verification question:** Does each server failure resolve to the same public ErrorKind and
application-safe message at the caller?

- Starting condition: A same-version provider can produce `NotFound`, `Rejected`, `Unavailable`, and
  `InternalFailure` variants.
- Procedure: The caller sends each variant's request once.
- Verification: The caller's public error kind and message match the expected variant, and the
  success request receives a normal reply. The raw envelope is verified by the contract test.
- Detailed behavior: verifies [Error Model §2](../spec/32-framework-error-model.en.md).

#### RL-D5 Pass Lifecycle Checkpoints During A Fixed Soak

Priority: `P2`

Soak does not turn an environment-specific performance bar into a new contract — it confirms
functional correctness is kept across repeated lifecycle.

**Verification question:** After each scale/weight/shutdown checkpoint of a 5-minute mixed workload,
do requests keep having a terminal?

- Starting condition: 20 clients each start requests and sends at 5 per second.
- Procedure: 1 minute of scale-out, 2 minutes of provider weight 0, 3 minutes of weight restoration,
  and 4 minutes of graceful scale-in are run.
- Verification: Each checkpoint's public status and directed request confirm the expected target set.
  Requests have exactly one terminal, with no functional errors or pending accumulation. Throughput
  and latency are recorded only, not used as a common PASS threshold.
- Detailed behavior: verifies repeated-lifecycle convergence in [Transport Liveness](../spec/29-transport-liveness.en.md).

### Track E — Verify Service Connection Liveness

#### RL-E1 Reflect An Orderly Disconnect Before The Peer Deadline

Priority: `P0`

FIN, RST, and a normal close must be excluded from ready selection without waiting for the 15-second
liveness deadline.

**Verification question:** After an orderly close and RST, does only the affected target become
not-ready within the common observation budget?

- Starting condition: RouteMesh and ClientServer targets are each ready.
- Procedure: The normal-close and RST variants are run on fresh connections, and public status is
  observed.
- Verification: The affected connection drops out of ready targets before the fixed peer deadline,
  and the other target keeps processing requests.
- Detailed behavior: verifies [Transport Liveness §5](../spec/29-transport-liveness.en.md).

#### RL-E2 Judge A Half-Open Connection Independently Of Application Traffic

Priority: `P0`

Even if one-direction traffic keeps showing, if the Framework's liveness round trip fails, the
connection must become not-ready.

**Verification question:** After a packet blackhole, does only the affected connection become
not-ready at the 15-second deadline?

- Starting condition: Two targets are ready, and a fault proxy can block one connection direction.
- Procedure: A→B packets are blocked, keeping B→A application traffic going. The fixed liveness
  deadline plus tolerance is waited for public status to change.
- Verification: Only the blocked connection is not-ready, and reverse application traffic does not
  extend the deadline. The other target's requests succeed.
- Detailed behavior: verifies [Transport Liveness §3](../spec/29-transport-liveness.en.md).

#### RL-E3 An Old Reply Before Reconnect Does Not Complete A New Request

Priority: `P0`

A delayed reply from the old physical connection has different correlation from a replacement
connection's operation.

**Verification question:** Even with the old request's reply delayed through reconnect, does the new
request receive only its own reply?

- Starting condition: The old provider handler holds the request's reply on an application gate.
- Procedure: After the request enters, the connection drops and a replacement becomes ready. The new
  request completes, then the old reply gate is released.
- Verification: The old request keeps exactly one failure or timeout terminal. The new request
  receives the replacement's reply exactly once, unaffected by the old payload.
- Detailed behavior: verifies [Transport Liveness §6](../spec/29-transport-liveness.en.md).

#### RL-E4 Produce Exactly One Terminal Even In A Connection-Loss Race

Priority: `P0`

Even if admission, cancellation, disconnect, and reply happen close together, request completion must
be exactly once.

**Verification question:** In three race variants, is there exactly one operation terminal, with the
handler running at most once?

- Starting condition: Handler-entered and reply-release are controlled by application signals.
- Procedure: Connection loss/cancellation are each raced before admission, right after handler entry,
  and right before the reply.
- Verification: Each request ends exactly once, in reply, cancellation, `Unavailable`, or timeout. The
  same operation ID is not processed by a different provider.
- Detailed behavior: verifies [Error Model §5](../spec/32-framework-error-model.en.md).

#### RL-E5 Handle A Store Failure Independently Of Transport Liveness

Priority: `P1`

A Store fail-static does not interrupt an established connection's liveness deadline.

**Verification question:** Even with a Store-response outage and a packet blackhole together, does
the connection become not-ready without reconnecting after Shutdown?

- Starting condition: A ready connection and a current provider exist.
- Procedure: The Store response is interrupted, and connection packets are also blackholed. A
  liveness failure is confirmed in public status, then Host Shutdown completes.
- Verification: The connection is not-ready at the peer deadline, unblocked by the Store error. After
  the Host terminal, public status does not switch back to Connecting, and no new handler evidence
  appears.
- Detailed behavior: verifies [Transport Liveness §7](../spec/29-transport-liveness.en.md).

### Track F — Confirm Relocation Lifecycle And Fencing Through Public Results

#### RL-F1 Preserve The Source In A Preflight With Changing Capacity

Priority: `P0`

If target capacity becomes insufficient after preflight, or the target becomes unavailable, source
admission must be restored.

**Verification question:** Does the source request succeed after a blocked Relocate in a
target-capacity/availability race?

- Starting condition: The target capacity's last slot can be raced against another create operation
  using an application gate.
- Procedure: A capacity variant that starts Relocate together with a competing create, and an
  availability variant that terminates the target process precommit, are each run.
- Verification: When Relocate succeeds, state exists exactly once at the target. The blocked variant
  keeps the source location and state, its follow-up request succeeds, and there is no automatic
  switch to a different target.
- Detailed behavior: verifies [Host Maintenance §6](../spec/28-graceful-drain-handoff.en.md).

#### RL-F2 A Previous Session's Message Does Not Apply To The New Binding After Rebind

Priority: `P0`

Even if the Actor owner changes A→B→A, the previous Session-binding token has a different identity
from the new binding.

**Verification question:** Does an old Session's delayed relay/unbind not change the new binding and
Actor state?

- Starting condition: The Actor, bound to Session S1, moves to a target owner and rebinds to a new
  Session S2.
- Procedure: S1's connection relay and unbind are delayed with a network gate and delivered after S2
  binding completes. A normal relay and push are then run on S2.
- Verification: The old operations end in a stale-binding result with no Actor handler evidence. S2's
  relay and push each succeed exactly once, and the current binding is S2.
- Detailed behavior: verifies [Session Actor Dispatch §4](../spec/20-session-actor-dispatch.en.md).

#### RL-F3 Interpret Cross-Language Terminal Failures The Same Way

Priority: `P0`

Even with different source and target languages, the formal public ErrorKind and typed `Rejected`
meaning must be the same.

**Verification question:** Does the same failure scenario return the same public terminal across
directional language combinations?

- Starting condition: Each target language provides the spec's error variants through a public
  handler.
- Procedure: A normal reply, `Rejected`, `NotFound`, `Unavailable`, and timeout are run in every
  supported language direction.
- Verification: The caller's received terminal kind and application payload match the scenario. Raw
  unknown-code injection is the protocol contract test's responsibility.
- Detailed behavior: verifies [Error Model §8](../spec/32-framework-error-model.en.md).

#### RL-F4 A ClientServer Process With No Client Role Cannot Call Outbound

Priority: `P0`

A process that registers only the ClientServer Server role has no Client egress for the same
ChannelName.

**Verification question:** Does a server-only process's request return `NotFound`, while a normal
Client request succeeds?

- Starting condition: A server-only process and a separate Client-role process are ready.
- Procedure: Both processes each start a request for the same ChannelName.
- Verification: The server-only call is `NotFound`, and its handler does not run. The Client call is
  processed exactly once by the server handler.
- Detailed behavior: verifies [ClientServer Channel §3](../spec/09-client-server-channel.en.md).

#### RL-F5 Process Messages Received During Relocation In Order At The Target

Priority: `P0`

While target restore is in progress, incoming messages must not be delivered directly to the
application handler — they must be processed after the handoff finishes, following the preserved
work.

**Verification question:** Are messages sent while restore-held processed once each, in order, at the
target after relocation completes?

- Starting condition: The source Actor/Instance Spot has queued markers Q1/Q2, and the target
  adapter's restore is held on an application gate.
- Procedure: Relocate is started and restore-held is confirmed, then H1/H2 are sent with the same
  logical IDs. The gate is released.
- Verification: The target handler evidence is in the order `Q1, Q2, H1, H2`, and each marker appears
  exactly once. There is no application handler evidence during the restore-held window.
- Detailed behavior: verifies [Location Runtime §8](../spec/21-location-runtime.en.md) and
  [Graceful Drain And Handoff §8](../spec/28-graceful-drain-handoff.en.md).

#### RL-F6 Distinguish A Runtime Mutable Update From An Invalid Mutation

Priority: `P0`

A spec-allowed value like weight can be changed while keeping the connection, but identity/capability
is not a runtime-update target.

**Verification question:** Does a public weight update change selection, while an unsupported
mutation is a local validation error?

- Starting condition: Two providers are ready.
- Procedure: B's weight is changed to 0 and 100, confirming directed requests. A separate invalid
  public configuration update is attempted.
- Verification: The weight update produces the RL-B4 result and keeps the connection. The unsupported
  mutation is a public validation error before starting the operation, and current target selection
  does not change.
- Detailed behavior: verifies [Channel Topology §4.3](../spec/07-channel-topology.en.md).

#### RL-F7 A Relocated Accepted Request Returns Exactly One Terminal

Priority: `P0`

Even if a request accepted before relocation overlaps with connection loss for its reply, the caller's
completion must be exactly once.

**Verification question:** Even if the Actor moves and the source connection drops during an accepted
request, does the request have exactly one terminal?

- Starting condition: The Actor handler has accepted the request and waits on the reply gate.
- Procedure: The Actor Relocate is started, and after handler state is restored at the target, the
  caller route is briefly blocked. The reply gate and route are restored.
- Verification: The caller ends exactly once, in reply, timeout, or an unavailable result. The same
  operation ID does not duplicate-run in the application handler, and the follow-up request succeeds
  at the current target.
- Detailed behavior: verifies [Spot Actor §8](../spec/15-spot-actor.en.md).

#### RL-F8 Host Relocate Does Not Start On A Manual Topology

Priority: `P0`

A Host with a manual RouteMesh peer, ClientServer client endpoint, or manual fanout endpoint
registered cannot automatically confirm replacement readiness, so it is blocked in Host Relocate's
preflight. In this case, source's accepted work and Host admission are not changed.

**Verification question:** Does Relocate on a manual-only topology end in `ManualTopologyUnsupported`
while keeping the source?

- Starting condition: A fresh Host with only a manual RouteMesh or ClientServer endpoint has a
  stateful source object and an accepted request waiting on an application gate.
- Procedure: Public Host Relocate is called and the terminal is confirmed. The request gate is then
  released, and a follow-up request is sent to the source object.
- Verification: Relocate is `Blocked/ManualTopologyUnsupported`, and the Host stays `Serving`. The
  accepted request and follow-up request are each processed once at the source, with no target
  restore/factory evidence.
- Detailed behavior: verifies [Host Maintenance §4](../spec/28-graceful-drain-handoff.en.md) and
  [§10](../spec/28-graceful-drain-handoff.en.md).

#### RL-F9 Distinguish A Preflight Timeout From A Post-Seal Deadline

Priority: `P0`

A timeout that ends before the admission seal does not change the source. A post-seal teardown
deadline can force a terminal.

**Verification question:** Do preflight-held and closing-held variants return different public
outcomes/Host states?

- Starting condition: The target-readiness gate and the source closing-callback gate are provided
  separately.
- Procedure: The first Relocate blocks readiness past its deadline; the second, fresh Host blocks the
  closing callback after the seal.
- Verification: The first is `Blocked/DeadlineExceeded`, and the Host is Serving. The second is
  `ForceStopped/DeadlineExceeded` or the spec's post-seal forced outcome, and the source is not
  mistaken as Serving again.
- Detailed behavior: verifies [Host Maintenance §12](../spec/28-graceful-drain-handoff.en.md).

#### RL-F10 Host-Relocate An Entry Actor And SpotWide Aggregate

Priority: `P0`

Host maintenance is not an Application Join. It restores an Entry Actor and User Spot aggregate at
the target but does not call the Actor Join/Leave callbacks.

**Verification question:** After the Relocate, are state and membership preserved without running
Join/Leave callbacks?

- Starting condition: An Entry Actor and a `SpotWide` User Spot's member Actors are at the source, and
  callback counters are 0.
- Procedure: Host Relocate completes, and current refs, state, and callbacks are queried.
- Verification: The objects preserve generation and state, processing requests at the target. The
  Join/Leave callback counters are 0, and the source Spot's closing reason is RelocationOut.
- Detailed behavior: verifies [Host Maintenance §8](../spec/28-graceful-drain-handoff.en.md).

#### RL-F11 Ready Relocation Units Finish Before Slow Units

Priority: `P0`

If one object's current turn is slow, making all the Host's other ready objects wait too would
enlarge the maintenance interruption.

**Verification question:** Do ready units' relocation completions progress while slow units are
handler-held?

- Starting condition: Some of 80 objects' handlers are held on application gates, with the rest idle.
- Procedure: Host Relocate is started, ready objects' public locations are observed, then the slow
  gates are released.
- Verification: At least one ready object has a target location and a normal handler result before
  the slow gates are released. The slow objects also reach a terminal after release, and aggregate
  members move together.
- Detailed behavior: verifies [Host Maintenance §7](../spec/28-graceful-drain-handoff.en.md).

#### RL-F12 Restore A User Spot's Queue And Timer After Relocation

Priority: `P0`

User Spot relocation must continue queued messages and the logical timer schedule at the target. The
Application does not re-register the timer.

**Verification question:** Are frozen messages and a pending timer each processed exactly once, in
original order, at the target?

- Starting condition: The source User Spot handler R0 waits on a gate, and Q1/Q2, Actors A1/A2, and a
  one-shot timer have been accepted.
- Procedure: Relocate is started, and R0 is released. After completion, target evidence and the timer
  callback are awaited.
- Verification: After R0, Q1/Q2 and A1/A2 keep their per-lane order and are each processed once. The
  timer callback also runs exactly once at the target, and the Application does not repeat timer
  registration.
- Detailed behavior: verifies [Spot Actor §8](../spec/15-spot-actor.en.md).

#### RL-F13 Finish Relocation Of Many Large-State Units With A Bounded Terminal

Priority: `P0`

Internal permit counts are not an E2E contract. The public E2E confirms that Host operations finish
within a bound across many units and a size boundary, and do not block source admission too early.

**Verification question:** Do 80 units and large-state variants each have exactly one success or
`StateIncompatible` terminal?

- Starting condition: 80 units of 1 MiB state, units at the 64 MiB boundary, and an oversize unit are
  built as separate fixtures.
- Procedure: Host Relocate is run on each fixture, collecting current locations and operation
  results.
- Verification: Valid units preserve their checksum at the target, and the oversize unit is
  `StateIncompatible` while keeping the source. Every unit and Host operation has a bounded terminal.
- Detailed behavior: verifies [Host Maintenance §7](../spec/28-graceful-drain-handoff.en.md).

#### RL-F14 Restore The Source Queue Order After A Precommit Abort

Priority: `P0`

If a target reservation or restore fails before commit, frozen work and work received during the seal
must be re-processed at the source in original order.

**Verification question:** After a failed Relocate, does the source process Q1, Q2, H1, H2 once each,
in order?

- Starting condition: The source User Spot has frozen Q1/Q2, and a target adapter failure can be
  selected with an application marker.
- Procedure: Relocate is started, and H1/H2 are sent during the seal window. Target-reservation
  failure and restore-failure variants are each run on fresh objects.
- Verification: Relocate has a blocked or failed terminal, and the public current location is the
  source. Source handler evidence is in the order `Q1, Q2, H1, H2` with no duplicates. A follow-up
  timer also runs normally at the source.
- Detailed behavior: verifies [Host Maintenance §9](../spec/28-graceful-drain-handoff.en.md).

## 5. Completion Criteria

- Every scenario uses only public operations, status, object lookup, and application
  handler/callback evidence.
- Raw liveness commands/ACKs, invalid protocol frames, wire headers, the Core peer table, and
  internal relocation permits are not E2E assertions.
- Restart and recovery are confirmed together with the latest public status and a directed follow-up
  request.
- Process/network races are controlled with application gates and public readiness, not fixed settle
  sleeps or probabilistic target selection.
- Each operation has exactly one terminal result, and connection recovery does not auto-resubmit a
  completed operation.
