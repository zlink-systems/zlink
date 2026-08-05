<!-- framework-adapter-nav:start -->
[E2E Index](README.en.md) | [Previous: One-Way Submit Admission](config-13-submit-admission.en.md)
<!-- framework-adapter-nav:end -->

# Config 14 — Instance Spot Activation

An Instance Spot is created when the Application first sends a message to a global Spot ID that
needs it. The caller does not choose the owner node or create the Spot beforehand. The Framework
confirms the public Instance intent, prepares one Spot on a suitable node, and processes messages
from the first one in the same Spot's execution queue.

This config verifies that this public behavior holds even in a deployment with multiple processes
and a real Store. Location Store rows, activation barriers, claim tokens, recovery cursors, and Core
frames are not used in judgment. The results of application factory/handler/lifecycle callback
execution are confirmed through the role servers' public evidence endpoints.

## 1. Verification Scope

- The first request and send with an Instance intent specified against a missing global Spot ID
- Concurrent first calls converging on a single Instance Spot
- Processing order between the first message and subsequent messages
- Capacity, stable type, initial Mesh, and existing-owner routing
- The public result after a crash, Store failure, deadline, or relocation
- Actor and Logical Multicast features that Instance Spot does not allow
- The same payload and terminal meaning across different Framework languages

An ordinary Spot direct call is existing-only. A missing-Spot call with no Instance intent does not
create a Spot. Instance Spot creation is not exposed through the Spot manager's public Create/
GetOrCreate features.

## 2. Deployment Configuration And Judgment Method

| Role | Count | Purpose |
|---|---:|---|
| Instance caller | 2 | Starts public Spot requests/sends from separate processes. |
| Instance owner | 2 | Provides the Instance factory and packet/timer handler of the same stable type. |
| User Spot owner | 1 | Verifies the contention and existing-only regression of creating a different kind of Spot with the same ID. |
| Location Store | 1 | Provides global Spot location and node status. |
| Relocation Store | 1 | Preserves the Framework state needed for Instance relocation and activation recovery. |
| External state store | 1 | Preserves application domain state. |
| E2E runner | 1 | Controls only process, network, and Store failure; starts Framework operations through the role servers' public endpoints. |

Each factory and handler records the Spot ID, operation ID, execution order, process ID, and domain
state version in application state. The E2E uses only the client result, public Spot lookup/status,
and this application evidence. Readiness is confirmed by bounded polling of public startup evidence.
Lease or recovery completion is not inferred from a fixed sleep.

## 3. Scenarios

### Track A — The First Call And Basic Routing

#### IS-E2E-01 Cold Request

Priority: `P0`

The first request against a missing Spot with an Instance intent specified must provide Spot
preparation and processing of the first work as a single public operation.

**Verification question:** Even if the caller did not create the Spot beforehand, is the first
request processed exactly once and does it get a reply?

- Starting condition: Both owners provide the stable type, and the Spot ID is Missing in a public
  lookup.
- Procedure: Caller A sends one request with an Instance intent.
- Verification: The factory and request handler each run exactly once, and the reply's Spot ID and
  operation ID match the input. A subsequent public lookup returns a Ready Spot ref.
- Contract basis: [Spot Address Messaging](../spec/16-spot-address-messaging.en.md),
  [Location Runtime](../spec/21-location-runtime.en.md)

#### IS-E2E-02 Cold Send

Priority: `P0`

A one-way send's success means the Framework accepted the message into the transmission path. It
does not mean the remote handler has finished.

**Verification question:** Does a cold send complete with the public send meaning, and does the
accepted message get processed exactly once at the final owner?

- Starting condition: The target Spot is Missing, and the owner factory's progress can be delayed
  with an application gate.
- Procedure: The gate is closed, an Instance-intent send is called, the send result is confirmed, and
  the gate is opened.
- Verification: The send can succeed before the handler finishes, and one piece of handler evidence
  appears after the gate is released. A separate input that injects activation failure does not
  change the already-returned send result.
- Contract basis: [Async Execution Policy](../spec/05-async-execution-policy.en.md),
  [Spot Messaging](../spec/12-spot-messaging.en.md)

#### IS-E2E-03 Concurrent First Call

Priority: `P0`

Even if different callers call the same missing ID at the same time, the Application must see only
one Spot.

**Verification question:** Do concurrent requests converge on a single factory and a single serial
handler?

- Starting condition: Two owners providing the same stable type are Ready.
- Procedure: Callers A and B concurrently send a sufficient number of requests, each with a unique
  operation ID, to the same Spot ID.
- Verification: Every successful reply points to the same Spot identity, and the factory runs
  exactly once. Every operation ID is processed exactly once with no duplicates, and the handler
  active count never exceeds 1.
- Contract basis: [Spot Address Messaging](../spec/16-spot-address-messaging.en.md)

#### IS-E2E-04 Different Spot ID

Priority: `P1`

Since separate Instance Spots use their own execution queues, one Spot's handler must not block
another Spot's processing.

**Verification question:** Does a Spot B request complete while Spot A's handler is waiting?

- Starting condition: Instance Spots A and B, with different IDs, are Ready.
- Procedure: A's handler is made to wait on an application gate, and a request is sent to B.
- Verification: B's reply arrives before the gate opens, and the Spot IDs in A's and B's evidence
  are not mixed.
- Contract basis: [Spot Messaging](../spec/12-spot-messaging.en.md)

### Track B — Losing And Re-Activating An Owner

#### IS-E2E-05 A Ready-Owner Crash Does Not Lead To Automatic Takeover

Priority: `P0`

If a Ready owner terminates, the Framework does not automatically pick another node as the new owner
or create a new incarnation of the same ID. A new request cannot be used until the Application has
explicitly completed Close and re-create.

**Verification question:** After the previous owner terminates, does the new request end in a
bounded `Unavailable` without running on another owner?

- Starting condition: The Spot is Ready on owner A, and domain state is stored in the external
  store. Another node, B, provides the same type but is not this Spot's owner.
- Procedure: A is crashed, and after public liveness/owner-lease status becomes invalid, a request is
  sent. Whether the Location Store authority record is automatically deleted is not judged in this
  scenario.
- Verification: The request ends exactly once in `Unavailable`. Neither A nor B has handler/factory
  evidence for that operation ID, and the Framework does not automatically create a new generation.
  Explicit recreate and rebind are confirmed separately in `IS-E2E-08` and application recovery
  scenarios.
- Contract basis: [Failure And Failover](../spec/31-failure-failover-policy.en.md)

#### IS-E2E-06 A Creating-Owner Crash Respects The Same Generation's Recovery Boundary

Priority: `P0`

If the owner terminates before the Spot that will process the first request has become `Ready`, the
Framework can either keep using the same generation's creation record or cancel it exactly. This
recovery differs from Ready-owner failover.

**Verification question:** Does an owner crash mid-factory end the existing request exactly once,
with follow-up requests not mixing with a different generation or a stale owner?

- Starting condition: The factory waits on an application gate.
- Procedure: The first request is started, factory entry is confirmed, and the owner is crashed.
  After the previous call's terminal, a new request is sent.
- Verification: The previous request ends exactly once, in either success or a formal failure. If
  the same generation's recovery continues, the follow-up request joins that activation; if the
  creation is canceled and becomes `Missing`, a new activation starts then. There must be no stale
  owner's handler evidence or generation mixing.
- Contract basis: [Location Runtime](../spec/21-location-runtime.en.md) and
  [Failure And Failover](../spec/31-failure-failover-policy.en.md)

#### IS-E2E-07 Normal Relocate

Priority: `P0`

A normal relocation moves the same Instance identity and domain state to the target owner, without
duplicate-executing a message being processed.

**Verification question:** After public Relocate completion, does a follow-up request use the state
restored at the target?

- Starting condition: The Spot is Ready on A, and the state version can be queried.
- Procedure: A public host relocation to B is started, the terminal success is awaited, and a state
  request is sent.
- Verification: The follow-up handler runs only on B, and the Spot identity and state version are
  preserved. An operation ID accepted before the relocation is also processed exactly once across
  all evidence.
- Contract basis: [Location Runtime](../spec/21-location-runtime.en.md) and
  [Graceful Drain And Handoff](../spec/28-graceful-drain-handoff.en.md)

#### IS-E2E-08 Close And Reactivate

Priority: `P1`

After explicitly closing a Spot, sending an Instance-intent call to the same ID must prepare a new
instance instead of reusing the previous runtime object.

**Verification question:** After Close completes, is the first call processed on a new factory
instance?

- Starting condition: The Spot is Ready, and the factory instance ID can be queried.
- Procedure: The public close operation's completion is awaited, then an Instance request is sent to
  the same ID.
- Verification: The new factory instance ID differs from the previous value, and the handler runs
  exactly once on the new instance.
- Contract basis: [Location Runtime](../spec/21-location-runtime.en.md)

#### IS-E2E-09 Concurrent Requests After A Ready-Owner Crash Do Not Auto-Switch

Priority: `P0`

Even if multiple callers request at the same time after a Ready owner is invalidated, the Framework
does not automatically create a new owner and factory.

**Verification question:** After the crash, do concurrent requests each end in a bounded failure with
no automatic takeover?

- Starting condition: The previous owner crash has been invalidated in public liveness/owner-lease
  status, and no Application recreate has run.
- Procedure: Two callers concurrently send requests to the same ID.
- Verification: Both requests each end exactly once in `Unavailable`, with no handler/factory
  evidence on any owner.
- Contract basis: [Failure And Failover](../spec/31-failure-failover-policy.en.md)

#### IS-E2E-10 No Automatic Owner Even After A Stale Owner Resumes

Priority: `P0`

Even if a long-stopped previous owner runs again, it must not process messages while the Application
has not recreated it. This scenario has no automatically created current owner.

**Verification question:** After the previous owner resumes, does the request end in `Unavailable`
without the stale owner processing it?

- Starting condition: The Spot is Ready on A; A is paused, and public owner lease is invalidated. B
  provides the same type but does not automatically own this Spot.
- Procedure: A is resumed, and a request with a unique operation ID and a timer-observation request
  are sent.
- Verification: The request ends in `Unavailable`, with no new handler/timer evidence on either A or
  B.
- Contract basis: [Failure And Failover](../spec/31-failure-failover-policy.en.md)

### Track C — Failure Results And Resubmission Boundaries

#### IS-E2E-11 Confirmed Not Admitted

Priority: `P1`

A confirmed result that the target did not accept the request must be delivered to the caller as one
failure.

**Verification question:** Does a confirmed admission failure return exactly once, with no handler
run on another owner?

- Starting condition: Public capacity is configured so that the selectable target rejects request
  admission.
- Procedure: A request with a unique operation ID is sent once.
- Verification: The request ends in exactly one contracted failure, and no owner's handler evidence
  has that operation ID.
- Contract basis: [Framework Error Model](../spec/32-framework-error-model.en.md)

#### IS-E2E-12 Ambiguous Result

Priority: `P1`

If the connection drops right after the target accepts the request, the Framework must not secretly
re-run the same request on another owner.

**Verification question:** Even if a connection failure occurs, is the handler execution count for
the operation ID at most one?

- Starting condition: The handler records the operation ID in durable application state.
- Procedure: Right after the target accepts the request, the network proxy terminates the
  connection.
- Verification: The caller receives either a reply or exactly one terminal failure, and the combined
  handler execution count across every owner is at most one.
- Contract basis: [Framework Error Model](../spec/32-framework-error-model.en.md)

#### IS-E2E-13 Accepted Send Then Failure

Priority: `P1`

Even if the target terminates after a send succeeds, there is no guarantee the Framework replays the
same one-way message on another owner.

**Verification question:** Is an accepted send not duplicate-processed on another owner after a
target failure?

- Starting condition: The send operation ID can be confirmed through handler evidence.
- Procedure: Right after the send succeeds, the target is terminated, and a replacement owner is
  prepared.
- Verification: The operation ID's processed count across all owners is 0 or 1, never 2.
- Contract basis: [Async Execution Policy](../spec/05-async-execution-policy.en.md)

#### IS-E2E-14 Store Outage

Priority: `P0`

If the current owner cannot be confirmed from the Location Store, the Framework must not create a
missing Spot from a local guess, or send a new message to a stale route.

**Verification question:** During a Store outage, does a new request end in a bounded failure with
no handler run?

- Starting condition: The Spot is made Ready, then Location Store access is blocked.
- Procedure: After the cached location's validity period ends, a request is sent.
- Verification: The request ends in a contracted Store/route failure, and the handler execution
  count across every owner is 0.
- Contract basis: [Location Runtime](../spec/21-location-runtime.en.md),
  [Framework Error Model](../spec/32-framework-error-model.en.md)

#### IS-E2E-15 Kind/Type Atomic Conflict

Priority: `P0`

The same global Spot ID cannot simultaneously become different Spot kinds or stable types.

**Verification question:** In a contention between User Spot creation and an Instance cold request,
does only one kind succeed?

- Starting condition: The ID is Missing, and the User Spot owner and Instance owners are Ready.
- Procedure: A User Spot GetOrCreate and an Instance request of a different type are started
  concurrently.
- Verification: Only one operation succeeds, and the public lookup's kind/type matches the
  successful operation. The failed side's factory and handler do not run.
- Contract basis: [Spot Address Messaging](../spec/16-spot-address-messaging.en.md)

#### IS-E2E-16 No Eligible Node

Priority: `P0`

If no node provides the stable type, or all capacity is exhausted, the caller must receive a public
failure that lets it tell the cause apart.

**Verification question:** Do a missing type and exhausted capacity each end in their contracted
terminal?

- Starting condition: A no-type topology and a zero-capacity topology are each prepared as separate
  runs.
- Procedure: The same shape of cold request and send are called in each topology.
- Verification: The request and send each return the terminal matching their condition, with no
  factory/handler evidence.
- Contract basis: [Framework Error Model](../spec/32-framework-error-model.en.md)

#### IS-E2E-17 Activation Backpressure

Priority: `P0`

Even if many cold activations are started at once, the factory and initialization must not run beyond
the public activation-concurrency setting. This scenario does not assume internal waiter capacity as
public configuration.

**Verification question:** When public activation concurrency is 1, does the concurrent factory
execution count never exceed 1, with each request having a bounded terminal?

- Starting condition: Activation concurrency is set to `1` through a language-specific public
  configuration interface, and the factory waits on an application gate.
- Procedure: A finite number of cold requests to different Spot IDs are started concurrently, and the
  gate is opened one at a time.
- Verification: The concurrently running factory/initialize count in application evidence is always
  at most 1, and each request ends in either a reply or a formal failure. The requests' admission
  order or internal waiter count is not judged.
- Contract basis: [Framework API §5](../spec/06-framework-api.en.md) and
  [Async Execution Policy](../spec/05-async-execution-policy.en.md)

#### IS-E2E-18 Cross-Language

Priority: `P1`

Even if the caller's and owner's implementation languages differ, the typed payload, reply, and
failure meaning must be the same.

**Verification question:** Do supported language combinations interpret the same cold request and
failure case with the same result?

- Starting condition: Callers and owners of different Framework languages register the same public
  contract and stable type.
- Procedure: A successful request and a no-type request are run in each direction combination.
- Verification: The successful payload and reply match, and the failure category also matches. No
  separate raw frame or test adapter is used.
- Contract basis: [Public Contract Governance](../spec/00-public-contract-governance.en.md)

### Track D — Ordering And Concurrency

#### IS-E2E-19 Ready Ordering

Priority: `P0`

The cold first message is the work input that caused the Spot to be created. A follow-up message that
arrives while the Spot is being prepared must not overtake it.

**Verification question:** Is the first request processed by the handler before follow-up requests?

- Starting condition: The factory can be delayed on an application gate.
- Procedure: The first request is started, factory entry is confirmed, follow-up messages are sent,
  and the gate is opened.
- Verification: The first operation ID in handler evidence is the first request, and the rest keep
  their acceptance order.
- Contract basis: [Spot Messaging](../spec/12-spot-messaging.en.md)

#### IS-E2E-20 Closing-Owner Crash

Priority: `P1`

If the owner terminates before Close completes authority release, the previous owner must not release
again or process new work. In this case too, the Framework does not automatically create another
owner.

**Verification question:** After a closing-owner crash, does the next call end in `Unavailable`
without a new factory running?

- Starting condition: The Close callback is delayed on an application gate so the owner can be
  crashed before authority release.
- Procedure: Once Close starts, the owner is crashed, and after public liveness/owner-lease status
  becomes invalid, an Instance request is sent.
- Verification: The new request ends exactly once in `Unavailable`, with no new factory/handler
  evidence on any owner. The flow that starts a new activation after an explicit Close completion is
  confirmed in `IS-E2E-08`.
- Contract basis: [Failure And Failover](../spec/31-failure-failover-policy.en.md)

#### IS-E2E-21 Multi-Mesh Initial Placement

Priority: `P0`

The initial Mesh selection decides where a Spot is first created when it doesn't exist yet. It is not
a request to move an already-Ready Spot to a different Mesh.

**Verification question:** Does a cold call's Mesh selection take effect, and does a call specifying
a different Mesh after Ready go to the current owner?

- Starting condition: Meshes A and B provide the same type, and the Spot is Missing.
- Procedure: A cold request specifying Mesh A is sent, then a follow-up request specifying Mesh B is
  sent.
- Verification: Both handlers run on the initial owner, and the factory runs exactly once.
- Contract basis: [Spot Address Messaging](../spec/16-spot-address-messaging.en.md)

#### IS-E2E-22 Monotonic Owner Deadline

Priority: `P0`

If the owner process is stopped for a long time and then resumed, it must not process new work using
a previously computed local valid state.

**Verification question:** After resuming past the deadline, does the owner not run message/timer
handlers, and does the request end in `Unavailable`?

- Starting condition: The Spot is Ready on A, and A can be process-paused. Another node does not
  automatically own this Spot.
- Procedure: A is paused, and after the lease is invalidated in public liveness/owner-lease status, A
  is resumed. A message and a timer-observation request are then sent.
- Verification: After resuming, there is zero new message/timer evidence on A and other nodes, and
  the caller's operation ends in `Unavailable`.
- Contract basis: [Failure And Failover](../spec/31-failure-failover-policy.en.md)

#### IS-E2E-23 Handler Capability

Priority: `P1`

Instance Spot does not provide Actor membership or Logical Multicast subscription. A misconfigured
factory must surface before it receives any work message.

**Verification question:** Does a cold request of a type that registered a forbidden handler
capability fail without running the application handler?

- Starting condition: A negative-type factory configures a forbidden capability through the public
  registration API.
- Procedure: An Instance request of that type is sent.
- Verification: The request ends in a configuration failure, and the packet handler and Actor
  lifecycle callback do not run.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### IS-E2E-24 Late Store Response

Priority: `P0`

Even if a Store response arrives after the operation deadline, it must not belatedly run the handler
for an expired request.

**Verification question:** Even after a late location response, does the request end in exactly one
timeout without the handler running?

- Starting condition: A network proxy delays the Location Store response longer than the request
  deadline.
- Procedure: A short-deadline request is sent, and after the timeout, the proxy is restored.
- Verification: The caller receives exactly one timeout, with no factory/handler evidence for that
  operation ID.
- Contract basis: [Framework Error Model](../spec/32-framework-error-model.en.md)

#### IS-E2E-25 Activation Completion Failure

Priority: `P1`

If the factory finishes but the final step that makes the Spot usable fails, it must not appear to
the caller as a Ready Spot.

**Verification question:** After an activation completion failure, does the public lookup not return
Ready, and does the next call converge normally?

- Starting condition: The owner's application initialization callback is configured to fail exactly
  once.
- Procedure: The first request's terminal is confirmed, the failure setting is removed, and the same
  ID is requested again.
- Verification: The first handler does not run, and the first request fails exactly once. The next
  request succeeds with exactly one factory and handler run.
- Contract basis: [Location Runtime](../spec/21-location-runtime.en.md)

#### IS-E2E-26 Concurrent Claim

Priority: `P0`

Even if the same cold call arrives at different targets, the Application object must be created on
only one target.

**Verification question:** Even under network contention, do the factory and handler run on only one
owner?

- Starting condition: Two owners provide the same type and capacity.
- Procedure: Two callers concurrently send first requests for the same ID.
- Verification: There is exactly one piece of factory evidence, on one owner, and every successful
  handler evidence is also only on that owner.
- Contract basis: [Location Runtime](../spec/21-location-runtime.en.md)

#### IS-E2E-27 Deadline Isolation

Priority: `P0`

Even operations waiting on the same activation must independently respect each caller's deadline.

**Verification question:** Does only the short request time out, while the long request and send
keep being processed?

- Starting condition: The factory gate delay is longer than the short deadline and shorter than the
  long deadline.
- Procedure: A short request, a long request, and a send are started in order on the same Spot, then
  the gate is opened.
- Verification: The short request times out, and the long request receives a reply. The send's and
  the long request's operation IDs are each processed exactly once.
- Contract basis: [Framework Error Model](../spec/32-framework-error-model.en.md)

#### IS-E2E-28 Close/Admission Contention

Priority: `P1`

A Spot where Close has started must not accept new work into the existing instance's queue.

**Verification question:** Is a request sent concurrently with Close not processed by the previous
handler?

- Starting condition: Close-callback entry can be confirmed through public application evidence.
- Procedure: Right after Close entry, a request with a unique operation ID is sent.
- Verification: The previous instance's handler has no such operation ID, and the request ends in
  either one failure or exactly one processing on a new instance after Close.
- Contract basis: [Location Runtime](../spec/21-location-runtime.en.md)

### Track E — Relocation Contention And Recovery

#### IS-E2E-29 Cross-Mesh In-Flight Relocate

Priority: `P1`

A message arriving at a Spot mid-relocation to a different Mesh must also be processed exactly once
in one owner's queue.

**Verification question:** Do Relocate and a concurrent request each end in exactly one terminal, with
no duplication?

- Starting condition: The Spot is Ready on Mesh A, and Mesh B provides a compatible target.
- Procedure: A Relocate to B is started, and a request with a unique operation ID is sent
  concurrently.
- Verification: Relocate and the request each end in exactly one terminal, and the request handler
  runs exactly once, on either A or B alone.
- Contract basis: [Location Runtime](../spec/21-location-runtime.en.md) and
  [Graceful Drain And Handoff](../spec/28-graceful-drain-handoff.en.md)

#### IS-E2E-30 Multi-Mesh Concurrent Relocate

Priority: `P1`

Requests trying to move the same Spot to different targets at the same time must not produce two
owners.

**Verification question:** After concurrent Relocate operations, does the public lookup return
exactly one owner?

- Starting condition: The source and two compatible targets are Ready.
- Procedure: Relocate operations specifying different targets are started concurrently.
- Verification: Each operation receives exactly one terminal, and the final public lookup shows one
  Ready owner. The follow-up request handler also runs only on that owner.
- Contract basis: [Location Runtime](../spec/21-location-runtime.en.md)

#### IS-E2E-31 Remote Selection Loser

Priority: `P1`

The target not selected in a cold-activation target contention must not create a separate application
instance or process requests.

**Verification question:** After the contention, does factory/handler evidence exist only on the
final owner?

- Starting condition: Two targets are configured with the same type and weight, and the same Spot ID
  is left `Missing`.
- Procedure: Multiple callers concurrently start cold requests with distinct operation IDs against
  the same Spot ID.
- Verification: The public lookup's owner matches the factory/handler evidence, and the
  non-selected target's factory count is 0.
- Contract basis: [Spot Address Messaging](../spec/16-spot-address-messaging.en.md)

#### IS-E2E-32 Activation Crash Boundary

Priority: `P0`

Even if the process terminates during a cold activation, the next request must not wait forever or
duplicate-process the same first operation. Before creation becomes `Ready`, the same generation's
recovery can either continue or be canceled exactly.

**Verification question:** After a source or target crash, does the existing call end in exactly one
terminal, with a follow-up call handled within the same-generation-recovery or explicit-new-activation
boundary?

- Starting condition: The pre-source-call and target-factory-entry points can be crashed separately.
- Procedure: The two boundaries are crashed in separate runs, and after the existing call's terminal,
  a follow-up request is sent.
- Verification: Each existing call ends in exactly one reply or failure. If the same generation's
  recovery continues, the follow-up request joins that activation; if the creation is canceled and
  becomes `Missing`, the next call starts a new activation. A Ready-owner crash is not interpreted as
  automatic handling by another owner, and every operation ID's processed count is at most one.
- Contract basis: [Failure And Failover](../spec/31-failure-failover-policy.en.md)

#### IS-E2E-33 Cold Activation Failure Release

Priority: `P0`

A Spot ID where the factory or initialize failed must not be pinned to an invisible failed instance.

**Verification question:** After the failure cause is removed, does the next call succeed on a new
factory?

- Starting condition: Factory failure and initialize failure can be reproduced with separate types
  or inputs.
- Procedure: Each failing request's terminal is confirmed, the failure is removed, and the same ID
  is requested again.
- Verification: The failed operation's handler execution count is 0, and it has exactly one
  terminal. The follow-up request succeeds with exactly one factory and handler run.
- Contract basis: [Framework Error Model](../spec/32-framework-error-model.en.md)

#### IS-E2E-34 Unpublished Activation Cleanup

Priority: `P1`

An activation that terminated before the owner finished the factory must not mix with the next call's
payload or result.

**Verification question:** After a crash, does new payload not mix with the previous activation's
payload, respecting the recovery boundary?

- Starting condition: The factory leaves entry evidence with payload A, then waits on an application
  gate.
- Procedure: After confirming factory entry, the target is crashed and the request terminal is
  confirmed. The target role is restarted, and a public Instance-intent request with payload B is
  sent to the same Spot ID. Public resolve and factory/handler application evidence are collected
  together.
- Verification: If the same generation's recovery continues, A is processed exactly once at the
  recovery root, followed by B; if the first activation is canceled and public resolve does not
  return a Ready object, only B is processed in a new activation. A and B payloads must not merge,
  and stale A must not be used in the new operation's reply instead of B.
- Contract basis: [Location Runtime](../spec/21-location-runtime.en.md)

#### IS-E2E-35 The Queue Is Not Automatically Recovered After A Ready-Owner Crash

Priority: `P0`

If the owner terminates after the Spot is already `Ready`, the Framework does not automatically
replay the first request and follow-ups on another owner.

**Verification question:** After the crash, does each operation end in exactly one terminal without
another owner's handler running?

- Starting condition: The gap between the public lookup returning the Spot as `Ready` and the first
  handler starting is widened with an application gate. The owner is already Ready.
- Procedure: First and follow-up requests are sent, then the owner is crashed and restarted within
  the gate window. No application recreate is run.
- Verification: Each caller receives exactly one terminal, with no other owner's handler/factory
  evidence after the crash. Subsequent messaging ends in `Unavailable` until an explicit Close and
  recreate.
- Contract basis: [Failure And Failover](../spec/31-failure-failover-policy.en.md)

#### IS-E2E-36 First-Handler Terminal Recovery

Priority: `P0`

If the owner terminates while the first handler has started, it cannot be assumed the Framework will
necessarily re-run the same operation. However, it must not duplicate-run it on another owner or
leave the call hanging indefinitely.

**Verification question:** Across a crash before and after the handler starts, does the caller get
exactly one terminal with no automatic replay on another owner?

- Starting condition: Handler entry and the domain commit are recorded as separate application
  evidence.
- Procedure: Crashes are run in separate runs, right before and right after handler entry, and the
  caller's and follow-up request results are confirmed.
- Verification: Each caller ends in exactly one reply or failure. Each operation ID's domain commit
  happens at most once, and no other owner's handler has that ID. Follow-up requests end in
  `Unavailable` until an explicit recreate/rebind.
- Contract basis: [Framework Error Model](../spec/32-framework-error-model.en.md)

## 4. Reference Sample Verification

### 4.1 GameQuest {#71-gamequest}

GameQuest uses the Player ID as the global Spot ID and specifies a public Instance intent on quest
messages. The sample code must not choose the owner node or create the Instance Spot beforehand
through the Spot manager.

- The first message arriving concurrently at different Quest nodes converges on a single factory and
  a single serial handler.
- The same Player ID's gameplay send and progress request keep first-message ordering.
- After a Ready-owner crash, the Framework does not automatically process follow-up calls on the
  current owner. The Application must recreate a new generation using external state and rebind the
  Session.
- The sample E2E judges only the client reply, handler operation ID, and domain state, without
  reading internal activation state.

### 4.2 ShoppingMall {#72-shoppingmall}

ShoppingMall uses the Order ID as the global Spot ID and specifies a public Instance intent on
workflow messages. The caller does not deal with the Instance address, owner node, or activation
phase.

- The first start request creates exactly one runtime Instance and one domain workflow.
- An order with a domain workflow but no runtime Spot recovers from external state before processing
  the command.
- Continue/rebuild for an ID with neither a runtime nor a domain workflow does not turn an empty
  workflow into a success.
- After Close, the next valid command is processed on a new factory instance, without
  duplicate-running a previous operation.

## 5. Completion Criteria

- All 36 scenarios start operations through the role servers' public Framework API.
- Pass/fail judgment uses only the client result, public Spot lookup/status, and application
  factory/handler/callback evidence.
- Fixed sleep, Store records, private activation phase, raw frames, and internal counters are not
  used as pass conditions.
- If a supported language cannot implement a scenario, it is not completed as a skip — the public
  contract gap is recorded in the feature map instead.
- Caller/owner combinations of at least two Framework languages interpret the same successful
  payload and terminal failure with the same meaning.
