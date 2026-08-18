<!-- framework-adapter-nav:start -->
[E2E Index](README.en.md) | [Previous: Resilience](config-5-resilience-lifecycle.en.md) | [Next: Monitoring](config-7-monitoring.en.md)
<!-- framework-adapter-nav:end -->

# Config 6 — Location Store And Relocation Store Failure

The Location Store provides the current location of services and stateful objects, and the
Relocation Store preserves the Instance Spot cold-activation record and the terminal record of a
pending request that completes after relocation. An Actor/Spot state-handoff payload is transferred
directly from source to target and does not pass through the Relocation Store. If the Store stops
responding briefly, an already-ready transport connection is not immediately dropped. On the other
hand, a stateful owner must not keep accepting new messages once it passes the last-confirmed owner
lease deadline.

This config builds Store outages, delays, recovery, and provider crashes as real processes, and
confirms public status, operation results, and application handler evidence. The E2E client does not
directly read or interpret descriptors, owner leases, authority rows, or relocation chunks.

## 1. Verification Scope

- Normal baseline and polling-based automatic discovery
- Retaining existing connections during a Store outage and holding off new topology changes
- Excluding a stale provider and replacing it after the owner lease expires
- Recovery from short and long Store outages
- Isolation of application processing that has nothing to do with a slow Store response
- The Relocation Store's remaining responsibilities and failure boundary, long-running relocation, and owner replacement
- Public operational query pagination and capacity results

## 2. Deployment Configuration

| Role | Count | Purpose and reason for separation |
|---|---:|---|
| Location Store | 1 | Provides automatic topology, object location, and owner leases. The harness can stop or delay only this Store. |
| Relocation Store | 1 | Provides the Instance Spot cold-activation record and the post-relocation pending-request terminal record. It does not store the state-handoff payload. Its failures are controlled from a separate instance/namespace from the Location Store. |
| Provider | 2 | Provides Channel handlers, Actor/User Spot/Instance Spot factories, and the relocation adapter. |
| Consumer | 1 | Is the Object Client and Channel caller. Provides the public RouteMesh/Host status and object operation endpoints. |
| E2E client | 1 | Calls only the role servers' public application endpoints. |

The owner lease renew interval, TTL, polling interval, and Store failure grace are specified in the
runner configuration. Time boundaries are computed from these values plus tolerance, with no
arbitrary settle sleep added. The runner stops and restarts the Store process and provider process
from the outside.

## 3. Common Run And Judgment Method

Each scenario uses a fresh Store namespace and object IDs. Store failure is confirmed by public
status and operation result, and after recovery, both status-ready and actual request success are
confirmed. The status observer is not assumed to deliver every intermediate state — the final
`GetStatus` is cross-checked instead.

When confirming that an application handler processes a request unrelated to the Store, the Store
proxy's response gate is controlled by an application signal. Small differences in latency
percentiles are not compared against a flaky threshold.

## 4. Scenarios

### Track A — Verify The Normal State And Polling

#### SF-A1 Store Normal-State Baseline

Priority: `P0`

To judge Store-failure scenarios, the same topology must first be confirmed ready and processing
requests in the normal state.

**Verification question:** Do the two providers appear as ready targets, with Channel requests
processed by either?

- Starting condition: The Location Store, providers A and B, and the consumer are running normally.
- Procedure: The consumer's public RouteMesh status is read, and 20 distinct requests are sent.
- Verification: Status reports a ready-target count of 2, and each of the 20 requests receives one
  reply. The combined handler count is 20.
- Detailed behavior: verifies [Normal Processing Order](../spec/server/21-location-runtime.en.md#14-normal-processing-order)
  and [Read And CAS](../spec/server/21-location-runtime.en.md#51-read-and-cas).

#### SF-A2 Reflect Provider Changes Through Polling Without A Watch

Priority: `P1`

Even a Location Store with no provider-specific watch must be able to refresh topology through
bounded snapshot polling.

**Verification question:** Are provider additions and removals reflected in public status and
request targets within the configured polling interval?

- Starting condition: A Store extension that provides only polling, and provider A, are ready.
- Procedure: Provider B is started, and the increase in ready targets is awaited. B shuts down
  normally, and the decrease in targets is awaited.
- Verification: After B is added, both providers can process requests; after removal, only A does.
  All waits are computed from the polling interval plus common tolerance.
- Detailed behavior: verifies [Location Runtime §3](../spec/server/21-location-runtime.en.md#3-finding-running-nodes-and-their-capabilities).

### Track B — Verify The Fail-Static Boundary During A Location Store Outage

#### SF-B1 Keep An Existing Connection During A Store Outage

Priority: `P0`

A Store polling failure alone does not drop a transport that is already ready. As long as peer
liveness is normal, messages can keep being processed over the existing connection.

**Verification question:** Do requests to the existing provider still succeed while the Location
Store is down?

- Starting condition: The SF-A1 baseline has passed, and both peers are ready.
- Procedure: While continuous requests are being sent, the runner stops the Location Store. Request
  results and status are read within a verification window shorter than the owner lease TTL.
- Verification: Requests over the existing connection keep receiving replies. Status may provide a
  formal degraded reason, but it does not immediately remove a ready transport for a Store error
  alone.
- Detailed behavior: verifies the fail-static boundary in [Location Runtime §4](../spec/server/21-location-runtime.en.md#4-blocking-a-previous-owners-new-work-when-the-store-connection-drops).

#### SF-B2 Distinguish Existing Connections From New Discovery Past The Failure Grace

Priority: `P1`

Once the failure grace has passed, a new provider that cannot be verified against the Store is not
added as a connection target. An already-ready transport can be kept as long as its own liveness is
normal.

**Verification question:** During a long Store outage, do existing requests keep succeeding while a
new provider is not added to targets?

- Starting condition: Only provider A is ready, and a failure grace is specified.
- Procedure: The Store is stopped for longer than the grace, and provider B is started. Requests keep
  going to A.
- Verification: A's requests succeed, and B is not added as a ready target in public status. B enters
  the current target set only after the Store recovers.
- Detailed behavior: verifies [Location Runtime §4](../spec/server/21-location-runtime.en.md#4-blocking-a-previous-owners-new-work-when-the-store-connection-drops).

#### SF-B3 The Discovery Grace Does Not Extend A Stateful Owner's Lease

Priority: `P0`

The grace that keeps a discovery connection alive and the lease that lets an Actor/Spot owner accept
new work are separate policies.

**Verification question:** Are new Instance Spot requests rejected past the owner lease deadline even
while the transport is kept?

- Starting condition: The consumer-provider connection is ready, the provider has an active Instance
  Spot and a periodic timer, and the failure grace is longer than the owner lease TTL.
- Procedure: The owner lease deadline is passed while the Store stays stopped. A request is sent to
  the same Instance Spot, and the timer's application evidence is read.
- Verification: The RouteMesh peer can have normal transport liveness, but the new stateful request
  ends in a formal unavailable result. The timer callback evidence also does not increase past the
  lease deadline.
- Detailed behavior: verifies [Object Routing And Creation Recovery](../spec/server/31-failure-failover-policy.en.md#4-object-routing-and-creation-recovery)
  and [Store Failure](../spec/server/31-failure-failover-policy.en.md#7-store-failure).

### Track C — Distinguish A Stale Provider From Its Lifecycle

#### SF-C1 Exclude A Crashed Provider After Its Lease Expires

Priority: `P0`

Even if a provider terminates without clearing its descriptor, once the owner lease expires it can no
longer be used as a current ready target.

**Verification question:** After provider B crashes, is B dropped from ready targets and are
follow-up requests handled only by A?

- Starting condition: A and B are ready and have each processed a baseline request.
- Procedure: The runner force-terminates B and waits for the configured owner lease to expire and
  public status to converge on the current target set. 20 follow-up requests are sent.
- Verification: B does not appear in status's ready peers/targets, and A processes all 20. Repeated
  timeouts against B's endpoint are not treated as success.
- Detailed behavior: verifies [Finding Running Nodes And Their Capabilities](../spec/server/21-location-runtime.en.md#3-finding-running-nodes-and-their-capabilities).

#### SF-C2 An Orderly Shutdown Does Not Wait For Lease Expiry

Priority: `P1`

An orderly shutdown blocks new selection first and clears owner information, so unlike a crash it does
not need to wait for TTL expiry.

**Verification question:** Is B excluded from status immediately after its `Stopped/None`, with A
continuing to serve?

- Starting condition: A and B are ready.
- Procedure: Public Shutdown is called on B, and the Host terminal is awaited. Consumer status is
  polled immediately, and follow-up requests are sent.
- Verification: B drops out of new targets and only finishes already-accepted work within a bounded
  time. After the shutdown terminal, A processes every follow-up request.
- Detailed behavior: verifies [Host Maintenance §10](../spec/server/30-host-relocation-flow.en.md#10-relocate-completion-and-failure).

#### SF-C3 A Previous Owner's Lifecycle Cannot Change The Replacement

Priority: `P0`

Even if the same role restarts quickly, a late lease action or message from the previous process must
not remove the new lifecycle from being current.

**Verification question:** Does the replacement keep its ready state and request handling even after
the paused old process resumes?

- Starting condition: Provider A is process-paused so its lease expires, and replacement A2 is
  started and made ready.
- Procedure: A2 processes a marker request, then old A resumes. Requests with different markers keep
  being sent.
- Verification: Public status keeps A2 as the current ready peer, and requests are each processed
  once by A2. Old A's handler evidence does not increase.
- Detailed behavior: verifies [Failover Policy §3](../spec/server/31-failure-failover-policy.en.md#3-channel-target-and-connection-failure).

#### SF-C4 Resolve A Host With Multiple Service Roles Under One Lifecycle

Priority: `P0`

Even if one process provides multiple MeshNodes, ClientServer Servers, and a fanout publisher, after a
host crash/restart, every role must converge on the same current lifecycle.

**Verification question:** After a multi-role host replacement, do all roles become ready on the new
process, with the old roles not selected?

- Starting condition: One host provides two RouteMesh Channels, a ClientServer Channel, and a fanout
  publisher, all ready.
- Procedure: The host is force-terminated, and a replacement is started. Once each public status is
  ready, one marker per role is sent.
- Verification: The replacement handler processes every marker exactly once, and old-process evidence
  does not increase.
- Detailed behavior: verifies [Transport Liveness §6](../spec/server/29-transport-liveness.en.md#6-connection-loss-and-reconnect).

#### SF-C5 Read A Public Operational Query In Bounded Pages

Priority: `P0`

Returning a namespace with many objects all at once makes response size and memory use unbounded.
Public operational queries must use a page size and a continuation token.

**Verification question:** Reading 1,001 objects with page sizes 1/100/1000 yields all of them with
no duplicates or gaps?

- Starting condition: 1,001 ready objects are created through public manager operations.
- Procedure: For each page-size variant, the public query is repeated from the first page until
  continuation ends.
- Verification: Each page's item count never exceeds the requested cap, and the total logical IDs are
  exactly 1,001. The continuation token is not interpreted or modified by the client.
- Detailed behavior: verifies [Location Runtime — Querying The Current Location From Operational Tools](../spec/server/21-location-runtime.en.md#64-querying-the-current-location-from-operational-tools).

#### SF-C5A Distinguish Object States In ID And Page Queries

Priority: `P0`

For public object queries, a missing exact lookup is empty and no page item is returned. Only existing
objects return `Creating`, `Ready`, or `Unavailable` entries. A Store query failure must not return a
partial page as normal.

**Verification question:** Is Missing empty/absent while the three object states agree as entries,
with Store failure returning a whole-page error?

- Starting condition: Prepare a missing ID, an object held at a creation gate, a Ready object, and an
  Unavailable object after owner loss.
- Procedure: Query every ID and a bounded page containing the same objects. On a fresh fixture, fail
  the Store request and repeat the page query.
- Verification: The missing exact lookup is empty and no page item exists for its ID. `Creating`,
  `Ready`, and `Unavailable` objects return matching state entries in ID and page queries. The
  Store-failure variant is a whole-page error with no items or continuation token.
- Detailed behavior: verifies [Location Runtime — Querying The Current Location From Operational Tools](../spec/server/21-location-runtime.en.md#64-querying-the-current-location-from-operational-tools).

### Track D — Converge On Current Topology After Store Recovery

#### SF-D1 A Short Outage Does Not Unnecessarily Change Existing Connections

Priority: `P0`

If the Store recovers within the failure grace, the last stable target set and current connections
can be kept while reconciling against the fresh snapshot.

**Verification question:** Do the same providers keep processing requests across a short Store
outage?

- Starting condition: A and B are ready, and a continuous request workload is running.
- Procedure: The Store is stopped for less than the owner lease TTL, then restarted. The workload is
  kept running until status recovers to ready.
- Verification: Every request receives one terminal result each, and the current provider set stays
  A and B. There is no unnecessary re-registration call in application evidence.
- Detailed behavior: verifies [Blocking A Previous Owner's New Work When The Store Connection Drops](../spec/server/21-location-runtime.en.md#4-blocking-a-previous-owners-new-work-when-the-store-connection-drops).

#### SF-D2 Keep Only The Re-Registered Provider After A Long Outage

Priority: `P0`

If the Store outage is longer than the lease TTL, every previous lease expires. The recovered runtime
re-publishes its own current lifecycle, and a provider that crashed during the outage must be
excluded from the target set.

**Verification question:** After a long-outage recovery, does the still-running A stay while only the
crashed B is excluded?

- Starting condition: A and B are ready.
- Procedure: The Store is stopped for longer than the TTL, and B is force-terminated during that
  time. The Store is restarted, and A's requests are sent until consumer status converges.
- Verification: A's requests keep succeeding in the periods it can, and after recovery there is one
  ready target, A. B is not auto-created as a replacement or sent to along the old route.
- Detailed behavior: verifies [The Store-Connection Fence](../spec/server/21-location-runtime.en.md#4-blocking-a-previous-owners-new-work-when-the-store-connection-drops)
  and [Normal Processing Order](../spec/server/21-location-runtime.en.md#14-normal-processing-order).

#### SF-D3 Public Status Converges As Ready → Degraded → Ready

Priority: `P1`

The Application must be able to confirm Store failure and recovery from public topology status.

**Verification question:** Does one outage cycle's latest status reflect the normal, degraded, and
normal states in order?

- Starting condition: The consumer observer has received the initial Ready status.
- Procedure: The Store is stopped, and a degraded status is awaited; it is started again, and a Ready
  status is awaited.
- Verification: The sequence observed from the same source increases, and each stage's final
  `GetStatus` matches the actual Store/target state. The presence of every intermediate event is not
  required.
- Detailed behavior: verifies [Runtime Monitoring §3](../spec/server/24-runtime-monitoring.en.md#3-querying-current-state-and-observing-changes).

### Track E — Isolate A Slow Store Response From Application Dispatch

#### SF-E1 Unrelated Requests Are Processed While A Store Response Is Pending

Priority: `P1`

Slow Store I/O must not stall the entire event loop or application execution lane of the same
process.

**Verification question:** Does a Channel request unrelated to the Store complete while a Store query
is waiting on the response gate?

- Starting condition: The Store proxy can hold a specific query's response on an application signal,
  and the normal Channel handler does not use the Store.
- Procedure: A Store query is started through a public operation, confirming it is proxy-held. In
  that state, 100 Channel requests are sent and all complete, after which the Store response is
  released.
- Verification: The Channel requests each receive a reply before the Store gate is released. The
  Store operation also returns a formal terminal once released.
- Detailed behavior: verifies I/O-wait isolation in [Worker Offload](../spec/server/05-async-execution-policy.en.md#12-worker-offload).

### Track F — Verify Public Results Of Relocation And Owner Recovery

#### SF-F1 Interpret Cross-Language Object Location And State

Priority: `P0`

An Actor/Instance Spot created by one language's runtime must be usable by a caller in another
language and a replacement runtime, with the same global identity and state.

**Verification question:** Across directional language combinations, is the same application state
obtained after create/request/relocation?

- Starting condition: The source and target languages provide the same stable type and public packet
  contract.
- Procedure: An object is created and its state changed in the source language. A caller in a
  different language sends a request, the object is relocated to the target language, and it is
  requested again.
- Verification: The public ID and ObjectGeneration are preserved, and the payload/reply and state
  values match in every direction.
- Detailed behavior: verifies interop in [Public Contract Governance](../spec/server/00-public-contract-governance.en.md).

#### SF-F2 A Long-Running Relocation Completes Within The Operation Deadline, And A New Call Is Allowed After A Failure

Priority: `P0`

Even if capture/restore takes a long time, it must be able to finish within the current relocation
operation's absolute deadline. A mid-way-failed operation is not auto-resumed — it is retried with a
new call the Application starts.

**Verification question:** Does a long-running relocation succeed, and does only a new relocation
succeed after a failed variant?

- Starting condition: Adapter capture can be held for a long time on an application signal.
- Procedure: The first relocation is kept running past several owner-lease renew intervals, but for
  less than the operation deadline, then released. A fresh object's second relocation is made to fail
  through an explicit target failure before the relay-ready reply, and a new call is started.
- Verification: The first operation succeeds, preserving state. The failed operation keeps its source
  location and state, and only the new operation ID's call completes at the target.
- Detailed behavior: verifies [The Complete Actor And Spot Relocation Flow](../spec/server/28-relocation-flow.en.md).

#### SF-F3 Distinguish What A Relocation Store Failure Blocks And Does Not Block

Priority: `P1`

The state-handoff payload is transferred directly from source to target, so a Relocation Store
failure does not block the Actor/Spot move itself. What the Store still blocks are its remaining
responsibilities — the terminal record of a pending request that completes after relocation and the
Instance Spot cold-activation record require the store, so results that depend on those records
cannot be reconstructed during the outage.

**Verification question:** Does Relocate still succeed through direct transfer during a Store
outage, with only results that need the store's remaining responsibilities — like the
pending-request terminal record — reconstructed after recovery?

- Starting condition: A stateful object is ready on the source, and the Location Store is normal. A
  pending request that completes after relocation can be produced with an application gate.
- Procedure: Only the Relocation Store is stopped, and public Relocate is called. After the terminal,
  a target request is sent. In a separate variant, a pending request that completes across the
  relocation is produced, and the caller results before and after the Store outage/recovery are
  collected.
- Verification: Relocate succeeds regardless of the Store outage, state is restored at the target
  through direct transfer, and the target request succeeds. The pending-request variant's caller
  receives exactly one terminal, and result reconstruction that depends on the store record holds
  only after Store recovery. Neither variant auto-resumes an operation or produces a duplicate
  terminal.
- Detailed behavior: verifies [Relocation Store Cancellation, Errors, And Result Reconstruction](../spec/server/23-relocation-store-redis.en.md#5-cancellation-errors-and-result-reconstruction).

#### SF-F4 Distinguish ObjectGeneration And Owner Replacement Through The Public Ref

Priority: `P0`

Relocation keeps the same logical incarnation, and an explicit close followed by re-create makes a new
incarnation.

**Verification question:** Does relocation keep the ObjectGeneration, while close/recreate returns a
new, higher value?

- Starting condition: The initial public refs of an Actor and Instance Spot are saved.
- Procedure: Each object is relocated to a different node, and the ref is re-queried. It is then
  publicly closed/destroyed and recreated with the same ID.
- Verification: After relocation, generation is the same as the initial one, with only the location
  changed to target. The recreate ref has a different, nonzero generation, and the previous exact
  ref's lifecycle call does not change the current object.
- Detailed behavior: verifies [Failover Policy §4.1](../spec/server/31-failure-failover-policy.en.md#41-logical-id-messaging-and-objectgeneration).

#### SF-F5 A Public Request Gets A Bounded Recovery Result After The Creating Owner Crashes

Priority: `P0`

Even if the owner crashes during Instance Spot cold activation, a stale owner must not accept new
work. Before creation becomes `Ready`, the creation record of the same generation can either keep
being used or be exactly canceled.

**Verification question:** After the creating owner crashes, does each of the pending request and the
follow-up request end in exactly one terminal?

- Starting condition: Instance factory initialization is held on an application gate.
- Procedure: A cold request is confirmed factory-held, and the owner process is force-terminated.
  Pending terminals are collected, and once recovery conditions are met, a new request is sent.
- Verification: The pending request ends exactly once, in either success or a formal failure. If
  recovery continues the same generation, the follow-up request joins that result; if it is canceled
  and public resolve does not return a Ready object, the next call starts a new activation. In either
  case, old-owner evidence does not increase.
- Detailed behavior: verifies [Location Runtime §6.1](../spec/server/21-location-runtime.en.md#61-first-creating-an-instance-spot-on-the-node-that-received-the-message)
  and [Failure And Failover §4.4](../spec/server/31-failure-failover-policy.en.md#44-distinguishing-instance-spot-cold-activation-from-owner-failure).

#### SF-F6 Reflect Concurrent Changes During An Operational Query On The Next Page Cycle

Priority: `P0`

Even if objects are added/removed during a paged query, a single scan must not return duplicate IDs or
end with a continuation interpretation error.

**Verification question:** During concurrent create/delete, does each completed scan return bounded
pages and unique IDs?

- Starting condition: 1,001 ready objects and a page-size-100 query are prepared.
- Procedure: After receiving the first page, some objects are created/deleted, and continuation is
  read to the end. A new scan is then started.
- Verification: IDs within a single scan have no duplicates and respect the page cap. The second scan
  reflects the mutations completed by that point. The client does not modify the continuation token.
- Detailed behavior: verifies [Location Runtime — Querying The Current Location From Operational Tools](../spec/server/21-location-runtime.en.md#64-querying-the-current-location-from-operational-tools).

#### SF-F7 Large-State Relocation Restores Across The Chunk Boundary

Priority: `P0`

The Framework does not impose a relocation-adapter-specific size cap. It splits participant state
into chunks no larger than `RelocationPayloadChunkLimit`, transfers them directly from source to
target, and restores after checksum verification. A payload larger than
`RelocationInFlightPayloadBudget` still starts and completes as chunks flow in order.

**Verification question:** Are the chunk boundary, a state one byte larger, and a state larger than
the in-flight budget all preserved and restored?

- Starting condition: Separate fixtures contain deterministic participant state exactly matching the
  configured chunk limit, one byte over, and larger than the in-flight payload budget, built through
  the public application API.
- Procedure: Each object is relocated to a target node, and its state checksum/length is queried
  through a public request.
- Verification: Every state has the same checksum/logical length at the target and processes
  requests. The one-byte-larger state and the larger-than-budget state do not end in
  `Blocked/StateIncompatible` because of a size cap.
- Detailed behavior: verifies the chunk split and in-flight budget boundary in
  [The Complete Actor And Spot Relocation Flow](../spec/server/28-relocation-flow.en.md).

#### SF-F8 The Source Is Kept If The Target Owner's Lease Expires

Priority: `P0`

If the target owner stops being current during target preparation, a stale completion must not be
committed.

**Verification question:** If a target process pause makes the lease expire, does Relocate fail while
the source handler keeps running?

- Starting condition: Target adapter restore is waiting on an application gate.
- Procedure: After confirming restore-held, the target process is paused past the owner lease
  deadline. The process is resumed, and the gate is released.
- Verification: Relocate ends in a target-unavailable result, and the public current location is the
  source. The source's follow-up request succeeds, and the target handler receives no new workload.
- Detailed behavior: verifies [Failover Policy §5](../spec/server/31-failure-failover-policy.en.md#5-host-relocation-failure).

#### SF-F9 Old-Lifecycle Cleanup Does Not Remove Replacement Service Roles

Priority: `P0`

On a fast restart, a late cleanup by the old process removing the replacement service descriptor would
make a stateless service unavailable again. This scenario is kept separate from the policy of not
automatically replacing a stateful Actor/Spot owner.

**Verification question:** Does the replacement Channel keep being selected even after the old process
resumes?

- Starting condition: An old Channel provider is paused so its lease expires, and a same-role
  replacement provider is made ready.
- Procedure: A replacement Channel request is confirmed, then the old provider resumes, and the same
  request is repeated.
- Verification: Public status keeps the replacement provider as the current ready target, and every
  follow-up marker is processed exactly once by the replacement. This scenario does not judge Actor/
  Spot object location.
- Detailed behavior: verifies [Failover Policy §3](../spec/server/31-failure-failover-policy.en.md#3-channel-target-and-connection-failure).

#### SF-F10 Handle Many Accepted Requests Together With Relocation Completion

Priority: `P0`

Even if an object with many accepted requests moves, each request's reply and the relocation terminal
must not be duplicated or lost.

**Verification question:** During in-flight requests and large replies with a relocation, does each
operation get exactly one terminal?

- Starting condition: A stateful object's handler holds each request's reply on a per-request
  application signal.
- Procedure: Many requests with distinct IDs are accepted, then Relocate is started, and the signals
  are released. After completion, a follow-up request is sent.
- Verification: Each accepted request ends exactly once, in reply, timeout, or relocation failure.
  The Relocate terminal is also single, and the follow-up is processed once at the current target.
- Detailed behavior: verifies [Host Maintenance §7](../spec/server/30-host-relocation-flow.en.md#7-relocation-units-and-concurrency-limits).

#### SF-F11 Preserve Payload Values After Waiter Termination And Transfer Failure

Priority: `P0`

Even if the direct transfer fails explicitly before the relay-ready reply or a supported language
cancels its relocation waiter, a mutable application payload must not be reused for another
operation. The cancellation variant runs only where the exact interface supports it.

**Verification question:** After a common transfer failure and supported-language cancellation, does
a new relocation restore only its own payload checksum at the target?

- Starting condition: Two fresh objects with distinct deterministic payloads A and B are prepared.
- Procedure: The common variant makes A's direct transfer fail explicitly before the relay-ready
  reply; supported languages additionally cancel a pending waiter. B's relocation is then run.
- Verification: A's awaitable keeps its transfer-failure or supported cancellation result, and A is
  restored from source memory. B's target-state checksum exactly matches B, with no mixing of A's
  bytes. Each operation has exactly one terminal.
- Detailed behavior: verifies the payload identity binding and failure rules in
  [The Complete Actor And Spot Relocation Flow](../spec/server/28-relocation-flow.en.md).

### Track G — Verify Capacity Results Through Public Create/Relocation

#### SF-G1 Apply Actor/Spot/Stable-Type Limits Atomically

Priority: `P0`

If one creation uses several capacity limits together, it must not fail after consuming only some of
them.

**Verification question:** Does the number of successful concurrent creates respect every public
limit, and does capacity recover after a failed create?

- Starting condition: The Actor total, Spot total, and stable-type limits are set to small distinct
  positive numbers.
- Procedure: Multiple callers concurrently start more creates than the remaining slots, and some
  factories return an application error. A new create is attempted after the failure.
- Verification: The successful active counts never exceed any limit. Calls with no capacity are
  `CapacityExceeded`, and factory-failed calls do not remain in the active count. After cleanup, a new
  create can use the available slot.
- Detailed behavior: verifies [MeshNode §5](../spec/server/13-mesh-node.en.md#5-object-placement-capability).

#### SF-G2 Distinguish Unlimited Population From Activation Concurrency

Priority: `P0`

A population limit of 0 is unlimited, and activation concurrency limits only how many factories run at
the same time.

**Verification question:** Do active objects hold under unlimited population, while only factory
concurrency respects the configured limit?

- Starting condition: The population limits are 0, and activation concurrency is a small positive
  number. The factory records the active count on an application gate.
- Procedure: Many creates are started concurrently, and the factory gate is released in sequence.
- Verification: The factory's active count never exceeds the concurrency limit, but every valid
  create eventually succeeds. The Entry Spot is not counted in Spot population, and member Actors are
  counted in the Actor count.
- Detailed behavior: verifies [MeshNode §5](../spec/server/13-mesh-node.en.md#5-object-placement-capability).

## 5. Completion Criteria

- Every judgment uses public status, operation terminal, object lookup, and application
  handler/callback evidence.
- Descriptors, lease tokens, authority rows, Store versions, relocation manifests/chunks, and provider
  buffers are not E2E assertions.
- Store failure and recovery are confirmed together with the latest public status and an actual
  follow-up request.
- Time boundaries are computed from the configured lease/grace/poll interval, with no arbitrary settle
  sleep.
- Store response delay is controlled by an application signal, and pass/fail is not decided by a small
  latency ratio.
