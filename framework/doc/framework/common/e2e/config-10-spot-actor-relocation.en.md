<!-- framework-adapter-nav:start -->
[E2E Index](README.en.md) | [Previous: To-Actor Messaging](config-9-to-actor-messaging.en.md) | [Next: Observability/Ops Deployment](config-11-observability-ops.en.md)
<!-- framework-adapter-nav:end -->

# Config 10 — Spot Actor Join And Relocation

When an Actor Joins a different Spot, the Framework changes the Actor's membership and current
location, and if it moves to a different node, also delivers state and not-yet-processed messages to
the target runtime. The Application does not need to know the internal switchover moment between
source and target, or resend messages.

This config verifies this behavior through the public Join/Relocate/message/binding API. It does not
directly read Location Store rows, relocation payloads, temporary queues, or internal update packets.
Instead, it confirms the Join result, public Actor/Spot ref, application lifecycle callback/handler
evidence, and the push a bound client received.

## 1. Verification Scope

- Accept/reject of an Actor Join on the same node and a different node
- `PreserveStateWith` and `RecreateOnRelocation` state handling
- The order and at-most-once processing of requests/sends accepted while moving
- The conditions under which the source Actor and binding are kept when a failure occurs
- A bound Session's route update after Actor relocation
- Old-route messages during the Message Follow period, and the result after it expires
- User Spot `PerActor`/`SpotWide` relocation and the execution-turn boundary
- Deferred Join completion, timeout, and handler context

This version does not automatically restart a relocation operation on a different target after a
source/target node or Store failure. Each operation ends in exactly one public success or terminal
failure, and the Application decides the next operation.

## 2. Deployment Configuration And Common Evidence

| Role | Count | Purpose |
|---|---:|---|
| Actor node | 2, 3 for multi-hop | Provides an Entry Spot, `PerActor`/`SpotWide` User Spots, an Actor factory/handler, and lifecycle callbacks. |
| Session gateway | 2 | Opens a Stream Session and binds an Actor, providing relay and push. |
| Relocation caller | 1 | Calls Join/Relocate/Actor messages through the role servers' public endpoints. |
| Location Store | 1 | Provides public Actor/Spot location lookup and routing. |
| Relocation Store | 1 | Used by the Framework to preserve relocation state. The E2E does not read internal records. |
| Network proxy | 1 when needed | Creates connection delay/blocking; does not generate Framework messages. |
| E2E client | per scenario | Uses only role server endpoints and the Stream connector. |

An Actor node records the callback name, Actor/Spot ID, operation ID, handler order, node ID, and
domain state version in application state. The Session gateway records the bind result and the Actor
ID/sequence of the push the client received. Ordering contention is reproduced by making a callback
or handler wait on a public application gate. The switchover moment is not inferred from a fixed
sleep or an internal queue length.

## 3. Scenarios

### Track A — Join On The Same Node

#### ST-A1 Local Join Accept

Priority: `P0`

A Join on the same node changes membership to the target Spot without re-creating the Actor
instance.

**Verification question:** After an accepted Join, is a follow-up Actor request processed under the
target Spot's membership?

- Starting condition: The Actor is in an Entry Spot, and a User Spot on the same node is Ready.
- Procedure: Join is called so the target `OnActorJoin` accepts it, and after success, an Actor
  request is sent.
- Verification: `OnActorJoin`, source `OnLeaveActor`, and target `OnJoinedActor` each run exactly
  once. The follow-up handler sees the target membership, and the Actor identity and state are
  preserved.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-A2 Local Join Reject

Priority: `P0`

If the target Spot rejects the Join, the Actor's existing membership and message route must not
change.

**Verification question:** After a rejected Join, does the Actor keep processing requests at the
source?

- Starting condition: The target `OnActorJoin` returns a typed rejection.
- Procedure: The Join result is received, then a state request is sent to the same Actor ID.
- Verification: Join returns a Rejected result and reply. The Leave/Joined callbacks do not run, and
  the state request is processed under the source membership.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-A3 Local Join Callback Boundary

Priority: `P1`

Before Join completion, source and target must not process the same Actor message at the same time.

**Verification question:** While the target lifecycle callback is waiting, is a message not
duplicate-processed under both memberships?

- Starting condition: The target `OnJoinedActor` waits on an application gate.
- Procedure: Join is started, and after callback entry, a request with a unique operation ID is
  sent, then the gate is opened.
- Verification: The operation ID's handler execution happens exactly once, on either source or
  target, and Join and the request each end in exactly one terminal.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

### Track B — Moving An Actor To A Different Node

#### ST-B1 PreserveState Relocation

Priority: `P0`

An Actor moving to a different node restores the application state saved by the adapter into the
target instance.

**Verification question:** After a remote Join completes, does the target Actor keep the source's
state version?

- Starting condition: A `PreserveStateWith` Actor is on node A, with a state version set.
- Procedure: It Joins a User Spot on node B, and after success, a state request is sent.
- Verification: The target factory/restore/`OnJoinedActor` and source `OnLeaveActor` each run once.
  The reply's state version and Actor identity are the same as before the move.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-B2 Moving Message Ordering

Priority: `P0`

Once target restoration starts, the Framework connects Actor messages that arrived during the move
into the queue the target Actor will use. The Application does not select this queue or compute the
relay moment.

**Verification question:** Are messages accepted before the move and messages received during the
move processed exactly once, in order, at the target?

- Starting condition: One source handler waits on an application gate and records the subsequent
  operation ID.
- Procedure: `before`, Join, `during-1`, and `during-2` are started in order, then the gate is
  opened, and `after` is sent after Join completes.
- Verification: The successfully processed IDs keep the order `before`, `during-1`, `during-2`,
  `after`, and each ID is processed exactly once. The same ID does not appear twice, across source
  and target.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md), [Spot Messaging](../spec/12-spot-messaging.en.md)

#### ST-B3 RecreateOnRelocation

Priority: `P0`

A `RecreateOnRelocation` Actor has the target factory create a new runtime instance with no
relocation adapter, keeping identity.

**Verification question:** Does an Actor with no registered adapter process messages at the target
after a remote Join?

- Starting condition: The Actor type registers only the `RecreateOnRelocation` policy and a factory.
- Procedure: It Joins a User Spot on a different node, and instance evidence and a follow-up reply
  are confirmed.
- Verification: Join succeeds, and the target factory runs exactly once. There is no Capture/Restore
  application callback, and a follow-up specifying the Actor ID is processed at the target.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-B4 Empty Relocation State

Priority: `P1`

A state adapter returning an empty payload is not a relocation failure. The target Actor can be
created by the factory and read the domain state it needs from a separate store.

**Verification question:** Does an empty-state Actor process the remote Join and a follow-up request
normally?

- Starting condition: The adapter returns empty state, and the target factory reads external state.
- Procedure: After the remote Join, a domain state request is sent.
- Verification: Join succeeds, and the restore callback runs exactly once with empty input. The reply
  returns the expected value from external state.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

### Track C — Relocation Failure

#### ST-C1 Location Store Response Loss

Priority: `P0`

If the location-update result cannot be confirmed during a Join, the Framework must not return
success and failure at the same time.

**Verification question:** Does a Store response loss end in exactly one Join terminal, with the
Actor message processed by only one owner?

- Starting condition: A network proxy can block the Location Store response during relocation.
- Procedure: The response is blocked during a remote Join, and after the public terminal, an Actor
  request is sent.
- Verification: Join ends in success or exactly one Store-related failure. The follow-up handler runs
  exactly once, on the same node as the public current location.
- Contract basis: [Framework Error Model](../spec/32-framework-error-model.en.md)

#### ST-C2 Target Connection Failure

Priority: `P0`

If the relocation request cannot reach the target, the source Actor must not be removed and the
route must not switch to the target.

**Verification question:** After a target connection failure, does the source Actor process requests
with its existing state?

- Starting condition: The Actor is on A, and the network path to B is blocked.
- Procedure: It Joins B, and after the failure, an Actor state request is sent.
- Verification: Join ends in exactly one connection-related failure. There is no Leave/target-Joined
  callback, and the state request returns the existing state at A.
- Contract basis: [Framework Error Model](../spec/32-framework-error-model.en.md)

#### ST-C3 Application Callback Failure

Priority: `P1`

Admission reject, a callback exception, and a timeout are different public results and must not be
represented as the same Accepted result.

**Verification question:** Do reject/exception/timeout inputs each return exactly one terminal
without duplicating the Actor?

- Starting condition: Actors are prepared separately with target callbacks configured for reject,
  exception, and bounded wait.
- Procedure: Each Actor's Join is called once, and after the terminal, a public Actor request is
  sent.
- Verification: Reject returns a Rejected result, and exception and timeout return the contracted
  failure. Each Actor request is processed at most once, on one owner.
- Contract basis: [Framework Error Model](../spec/32-framework-error-model.en.md)

### Track D — Current Location And Stale Route

#### ST-D1 Join Completion And Current Location

Priority: `P0`

When the Join completion callback is called, the Application must be able to use the target Actor as
the current Actor.

**Verification question:** Right after receiving Join completion, does an Actor request reach the
target with no separate wait?

- Starting condition: The Actor is on A, and the target Spot is Ready on B.
- Procedure: A state request is started right in the Join completion callback.
- Verification: Completion is called exactly once, and the request is processed exactly once at B.
  The caller does not recompute the owner RID or rebind.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-D2 Stale Source Message Fencing

Priority: `P1`

After the move completes, a message that arrives late on the source route must not re-run the source
Actor or duplicate-process against the target.

**Verification question:** Does the late source message's operation ID appear at most once across
every handler?

- Starting condition: A network proxy delays a message on the pre-move route.
- Procedure: Join is completed while the message is delayed, then the proxy is restored.
- Verification: The caller receives a result matching the public contract, and the operation ID's
  handler count across all nodes is at most one. Source state is unchanged.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

### Track E — Session Binding

#### ST-E1 Bound Session Push After Relocation

Priority: `P0`

After an Actor moves, the Framework updates the Actor location referenced by a bound Session. The
user does not need to detect the owner change and rebind.

**Verification question:** After a remote Join completes, does the existing bound client receive the
target Actor's push?

- Starting condition: A client Session is bound to A's Actor and has received push sequence 1.
- Procedure: The Actor Joins B, and after completion, push sequence 2 is sent.
- Verification: The same client receives sequence 2 exactly once, with no rebind. The push evidence's
  sender node is B.
- Contract basis: [Session Actor Dispatch](../spec/20-session-actor-dispatch.en.md)

#### ST-E1B Binding Route Per Relocation Mode

Priority: `P0`

Standalone Actor movement, `PerActor` Spot relocation, and `SpotWide` relocation must all update the
bound Session's current route.

**Verification question:** Do all three move methods deliver the target push through the existing
binding?

- Starting condition: An Actor and bound client for each move method are prepared with separate IDs.
- Procedure: After each method's relocation completes, a unique push sequence is sent from the
  target.
- Verification: Each client receives its sequence exactly once with no rebind, and the previous node
  does not send the same sequence.
- Contract basis: [Session Actor Dispatch](../spec/20-session-actor-dispatch.en.md)

#### ST-E1C Session Location Update Retry

Priority: `P0`

Join completion does not wait for the Session route-update response. If the moved runtime doesn't
get a first response, it retries after 1 second. If there is still no response, it retries at 1, 2,
4, and 5 second intervals, then keeps a 5-second interval after that.

**Verification question:** Even if the network path to the Session owner is temporarily cut, does
Join still complete, with the existing binding eventually receiving the target push?

- Starting condition: A proxy can block the network path between the target runtime and the Session
  owner.
- Procedure: With the path blocked, remote Join completion is confirmed, then the path is restored,
  and target push delivery is awaited with bounded polling.
- Verification: Join completion does not time out because of the Session-owner path being cut. After
  the path is restored, the target push is received with no rebind, with no duplicate push. The
  actual retry timing is diagnosed only when a public trace contract exists for it.
- Contract basis: [Session Actor Dispatch](../spec/20-session-actor-dispatch.en.md)

#### ST-E1A A New Actor Incarnation Requires A Bind

Priority: `P0`

If an Actor is explicitly removed and re-created with the same Actor ID, the previous Actor's binding
to the Session ends.

**Verification question:** Is the re-created Actor's push not delivered to the previous Session
before an explicit bind?

- Starting condition: A Session is bound to the Actor and has received a previous push.
- Procedure: The Actor is removed and re-created with the same ID, a push is sent, then it is
  explicitly bound, and another push is sent.
- Verification: The first push is not delivered to the previous binding, and only the second push,
  after a successful bind, is delivered.
- Contract basis: [Session Actor Dispatch](../spec/20-session-actor-dispatch.en.md)

#### ST-E2 A Failed Relocation Keeps The Binding

Priority: `P0`

If relocation fails, the Session binding must keep pointing at the source Actor.

**Verification question:** After a failed Join, does the source Actor's push reach the existing
bound client?

- Starting condition: The Session is bound to A's Actor, and the connection to target B is blocked.
- Procedure: A Join failure to B is confirmed, then a push is sent from A.
- Verification: The client receives the push exactly once with no rebind, and the sender evidence is
  A.
- Contract basis: [Session Actor Dispatch](../spec/20-session-actor-dispatch.en.md)

### Track F — Messages During A Move And The Previous Route

#### ST-F1 In-Flight Handoff Order

Priority: `P0`

Work the source accepted before the move must be processed before work accepted during and after the
move.

**Verification question:** Is the order preserved between work waiting at the source and work headed
to the target?

- Starting condition: The `old-1` handler waits on a gate, and `old-2`'s acceptance is confirmed by
  reply.
- Procedure: Join, `moving-1`, and `moving-2` are started, then the gate is opened, and `new-1` is
  sent.
- Verification: The successful-handler order is `old-1`, `old-2`, `moving-1`, `moving-2`, `new-1`,
  and each ID appears exactly once.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-F2 A Direct Message Cannot Overtake

Priority: `P0`

Even a direct message sent right after Join completion must not overtake messages the Framework
preserved during the move.

**Verification question:** Is the message sent from the completion callback processed after the
moving messages?

- Starting condition: The target restore callback can wait on a gate.
- Procedure: Moving messages are sent during Join, the gate is opened, then a direct request is sent
  from the completion callback.
- Verification: In target handler evidence, the moving IDs precede the direct ID, with no
  duplicates.
- Contract basis: [Spot Messaging](../spec/12-spot-messaging.en.md)

#### ST-F3 Bound Session Cross-Move Order

Priority: `P0`

Session relay messages and direct Actor messages must not break the Actor's serial execution even at
a relocation boundary.

**Verification question:** Are the bound relay and direct operations processed at the target Actor
with no duplication?

- Starting condition: The Session is bound to the Actor, and each input carries a sequence number.
- Procedure: Session relay and direct messages are submitted in a set order during Join.
- Verification: Every successful sequence appears exactly once in Actor handler evidence, and the
  concurrently running handler count is 1.
- Contract basis: [Session Actor Dispatch](../spec/20-session-actor-dispatch.en.md)

#### ST-F3A Late Session Route Update

Priority: `P0`

In consecutive relocations, a late route update from the first move must not overwrite the second
move's current location. This scenario runs by delaying the public route boundary between the target
runtime and the Session owner, without identifying internal packets.

**Verification question:** After A→B→C, does the bound push still arrive at C even with a late
update for B?

- Starting condition: A network proxy can delay only one direction of the connection from the B
  runtime to the Session owner. The connection to the C runtime is not delayed. The proxy does not
  read or generate packet names, frames, or payloads.
- Procedure: The A→B Join is completed, then, while keeping the B→Session-owner direction held, the
  B→C Join is completed. A bounded wait confirms the bound Session's public ActorRef location
  snapshot points to C, the B-direction delay is released, and a push is sent from C.
- Verification: The client receives the C push exactly once and does not receive the same sequence
  from B.
- Contract basis: [Session Actor Dispatch](../spec/20-session-actor-dispatch.en.md)

#### ST-F4 Message Follow Before And After Expiry

Priority: `P1`

A message that arrives on the previous route right after a move can be delivered to the target during
the configured Message Follow period. After the period ends, the old route must not keep being used.

**Verification question:** Is a message within the Follow period processed at most once, while a
message after expiry does not reach the target handler?

- Starting condition: A short but sufficient Message Follow duration relative to the test deadline is
  publicly configured, and the proxy holds old-route messages.
- Procedure: After Join completes, the first old-route message is delivered within the duration. The
  second message is delivered after the duration plus the scheduler tolerance has passed.
- Verification: The first operation ID is processed at most once at the target. The second ID is not
  processed, and the caller receives `Unavailable`.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-F5 Message Follow Route Cleanup

Priority: `P1`

Message Follow is not a feature that keeps the previous route forever. Current-route messages must
keep being processed normally after it expires.

**Verification question:** After Follow expires, is the old route unused while current Actor requests
succeed?

- Starting condition: Actor relocation is completed, and the proxy holds an old-route message
  received before the move until the duration plus tolerance has passed.
- Procedure: The proxy delivers the old-route message, then another operation is sent through the
  global Actor ID route.
- Verification: The old-route operation is absent from the target handler, and the global-ID
  operation is processed exactly once at the target.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-F6 Request Terminal Across Relocation

Priority: `P1`

A request during relocation must not return both a reply and a timeout, or swap reply correlation
with another request.

**Verification question:** Do requests with different deadlines each receive exactly one terminal for
their own correlation?

- Starting condition: The handler waits on a gate longer than the short request deadline and shorter
  than the long request deadline.
- Procedure: The short and long requests are sent during Join, then the gate is opened.
- Verification: The short request receives either a timeout or exactly one reply, and the long
  request receives the expected reply. Reply operation IDs are not swapped, and the handler count for
  each ID is at most one.
- Contract basis: [Framework Error Model](../spec/32-framework-error-model.en.md)

### Track G — Spot Relocation And The Execution Turn

#### ST-G1 Yielded Continuation Barrier

Priority: `P0`

A continuation an Actor handler `Yield`ed is still part of the same Actor turn. Relocation must not
run it concurrently on a different node.

**Verification question:** Are the state changes before and after Yield, and the request after
relocation, observed serially?

- Starting condition: The handler changes state, Yields, then performs a second change.
- Procedure: Relocation and a follow-up request are started after Yield evidence.
- Verification: The final state includes both defined changes and the follow-up, and the handler
  active count across all nodes is 1.
- Contract basis: [Async Execution Policy](../spec/05-async-execution-policy.en.md)

#### ST-G2 User Spot Aggregate Capacity

Priority: `P0`

A `SpotWide` relocation must fit the Spot and all its Actors, as a single unit, into target capacity.

**Verification question:** If capacity is short, is the source kept instead of moving only some
Actors?

- Starting condition: The Spot has several Actors, and target capacity is insufficient for the full
  move.
- Procedure: SpotWide Relocate is called, and after the terminal, a state request is sent to each
  Actor.
- Verification: Relocate ends in a capacity failure, and every Actor request returns existing state
  at the source.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-G3 PerActor Spot Relocation

Priority: `P0`

In `PerActor` mode, the Spot and its Actors can have different current locations, and each Actor's
route must update independently.

**Verification question:** Does only the selected Actor move to the target, with the rest processed
at the source?

- Starting condition: Actors A and B are in the same PerActor Spot.
- Procedure: Only A is relocated to the target, then requests are sent to A and B.
- Verification: A's handler runs at the target and B's at the source, and both Actors' identity and
  state are preserved.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-G4 A ToActor Message During A Spot Move

Priority: `P0`

A message sent by Actor ID during a SpotWide relocation must also follow that Actor's move order.

**Verification question:** Are ToActor messages processed serially at the target with no duplication
during a Spot move?

- Starting condition: A SpotWide Spot has several Actors and a sequence handler.
- Procedure: Unique sequence messages are sent to each Actor during the Spot Relocate.
- Verification: The successful sequence order is preserved per Actor, and the same operation ID does
  not duplicate across source and target.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-G5 Relocation Interruption Measurement

Priority: `P1`

Relocation performance is measured separately from correctness conditions. A fixed sleep or a small
sample is not used to fail a slow environment.

**Verification question:** Can a relocation-interruption distribution be recorded under a fixed
workload with no message loss or duplication?

- Starting condition: A sufficient Actor count and a steady request rate are used after warm-up.
- Procedure: The same profile is run several times, recording Join start/completion and client
  latency on a monotonic clock.
- Verification: Every accepted operation has exactly one terminal and at most one handler run. P50,
  P95, and P99 are reported as results, used for pass/fail only when a spec-defined SLO exists.
- Contract basis: [Runtime Metrics](../spec/25-runtime-metrics.en.md)

#### ST-G6 SpotWide Application Boundary

Priority: `P1`

SpotWide relocation must start after the current application turn ends, and process a new turn only
after target preparation finishes.

**Verification question:** Does a Spot handler overlapping relocation not run concurrently at source
and target?

- Starting condition: The Spot handler waits on an application gate.
- Procedure: After handler entry, SpotWide Relocate and a follow-up request are started, then the
  gate is opened.
- Verification: The existing handler finishes at the source, and the follow-up runs exactly once at
  the target. The combined active count never exceeds 1.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

### Track H — Deferred Join And Handler Context

#### ST-H1 Deferred Join Registration

Priority: `P0`

Deferring a Join inside a handler lets the current handler finish without blocking to wait for
completion.

**Verification question:** After calling defer, does the handler finish, and is the target
`OnActorJoin` called?

- Starting condition: The Actor handler uses the public Join-defer API with an immutable request.
- Procedure: The handler registers a defer and leaves completion evidence.
- Verification: The evidence order is `defer called`, `handler returned`, `OnActorJoin`. The Join
  request payload matches the value at call time.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-H2 Completion Outcome

Priority: `P0`

Deferred Join completion delivers exactly one Accepted, Rejected, or failure result for that
operation.

**Verification question:** Does the completion callback receive the operation ID and terminal result
exactly once?

- Starting condition: Accept and reject targets are prepared separately.
- Procedure: A deferred Join is called with a unique operation ID.
- Verification: Each completion is called exactly once, and the ID and result match that Join. A
  follow-up request is processed at the target after accept, or at the source after reject.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-H3 Context Identity

Priority: `P1`

The Join admission callback receives the Actor ID and join request, and the Join completion callback
receives the operation ID and terminal result. The Application does not mix these two callbacks'
values or require an internal owner token.

**Verification question:** Does the callback context's public identity match the actual Join input?

- Starting condition: The `OnActorJoin` admission callback and the Join completion callback each
  record public evidence.
- Procedure: Joins are run for different Actors and target Spots, and the admission and completion
  callback evidence are collected separately.
- Verification: The Actor ID and request in admission evidence match the input. The operation ID,
  result, and Accepted ActorRef in completion evidence match that Join. The target Spot is confirmed
  through public Actor context or ref lookup, not forced from a value the callback doesn't have.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-H4 Invalid Context And Duplicate Registration

Priority: `P1`

A callback that does not allow Join defer, or a duplicate registration within the same handler turn,
must clearly surface as a startup or operation failure.

**Verification question:** Does misuse return a contracted failure with no hidden Join?

- Starting condition: An unsupported lifecycle callback and a duplicate-defer input are prepared as
  separate cases.
- Procedure: Each case is run once.
- Verification: The public call or host startup ends in exactly one configuration/operation failure,
  and the target Join callback does not run.
- Contract basis: [Framework Error Model](../spec/32-framework-error-model.en.md)

#### ST-H4A Completion And Timeout Race

Priority: `P0`

Even if a deferred Join's timeout and accept race, completion must happen exactly once per operation.
This scenario does not assume a public pending limit.

**Verification question:** At the timeout boundary, does every deferred Join end exactly once in
Accepted, Rejected, or failure?

- Starting condition: A small number of deferred Joins and each operation's bounded timeout are
  prepared, with the target admission callback controlled by an application gate.
- Procedure: One operation's accept is made to overlap the timeout boundary, while another operation
  is explicitly rejected or accepted normally.
- Verification: Every operation ends in exactly one Accepted, Rejected, or failure. The completion
  count is 1 per operation, and no new target callback runs once a timeout is confirmed. The moment a
  callback started before the timeout finishes is not used as a separate success condition.
- Contract basis: [Async Execution Policy](../spec/05-async-execution-policy.en.md) and
  [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-H4B Yield And Reply Terminal

Priority: `P1`

Even if a handler Yields or waits on an asynchronous operation, the correlation between Join
completion and the original request's reply must not change.

**Verification question:** Does a handler that includes a Yield deliver the reply and Join completion
each exactly once?

- Starting condition: The handler registers a deferred Join after a Yield and returns a typed reply.
- Procedure: It is called with a unique request and Join operation ID.
- Verification: The caller reply and Join completion each happen exactly once, and the two IDs
  correspond exactly. The concurrent active count of the handler and callback does not exceed the
  contracted execution lane.
- Contract basis: [Async Execution Policy](../spec/05-async-execution-policy.en.md)

#### ST-H5 MessageContext Parity

Priority: `P1`

Each language's Framework must provide the same public identity and cancellation/deadline meaning for
the same handler kind.

**Verification question:** Do Actor handlers of supported languages observe the same MessageContext
values?

- Starting condition: Owners of different languages implement the same typed message and evidence
  schema.
- Procedure: Send, request, and deferred Join are run across each language combination.
- Verification: The Actor ID, operation ID, request correlation, and deadline presence carry the same
  meaning. No reflection or private adapter is used.
- Contract basis: [Public Contract Governance](../spec/00-public-contract-governance.en.md)

### Track I — Load And Multi-Hop

#### ST-I1 Payload Size Profile

Priority: `P1`

Even if the relocation payload and in-move message size differ, the correctness judgment must be the
same.

**Verification question:** Within the defined payload-size ranges, are accepted operations processed
with no loss or duplication?

- Starting condition: Small, medium, and large state and message fixtures are fixed.
- Procedure: Remote Join is repeated with the same Actor count and message count in each range.
- Verification: The state checksum and handler operation ID match the input, and each ID appears
  exactly once. Latency and memory are recorded separately as measurement results.
- Contract basis: [Runtime Metrics](../spec/25-runtime-metrics.en.md)

#### ST-I2 Many Actor Relocations

Priority: `P1`

Even while moving many Actors at once, public requests to a control Actor not part of the relocation
must keep being processed.

**Verification question:** Is control traffic not lost during bulk Actor relocation?

- Starting condition: A sufficient number of relocation Actors and a separate control Actor are
  prepared.
- Procedure: Actor relocations are run with bounded concurrency while control requests are sent at a
  steady rate.
- Verification: Both relocation and control operations each have exactly one terminal, and
  successful operation IDs are processed exactly once. Latency distribution is reported.
- Contract basis: [Runtime Metrics](../spec/25-runtime-metrics.en.md)

#### ST-I3 Many Spot Relocations

Priority: `P1`

Multiple SpotWide moves must also not interrupt the execution of a Spot that isn't part of them.

**Verification question:** Do control Spot requests keep completing during bulk Spot relocation?

- Starting condition: Several SpotWide Spots and a separate control Spot are prepared.
- Procedure: Relocations and control requests are run together with bounded concurrency.
- Verification: There is no control-request loss or duplication, and each Spot's Actor state checksum
  is preserved after relocation.
- Contract basis: [Runtime Metrics](../spec/25-runtime-metrics.en.md)

#### ST-I4 Message Follow Authority Boundaries

Priority: `P1`

Message Follow handles old-route messages right before and after move completion, and must not
change normal processing of the current global route.

**Verification question:** Is an old-route message at each boundary processed at most once, while a
current-route message always reaches the target?

- Starting condition: The proxy holds old-route messages separately for before the move, right after
  completion, and after expiry.
- Procedure: Each message is delivered at its boundary, together with a global Actor ID request.
- Verification: The old-route operation is processed at most once, only within the contracted period.
  The global-route operation is processed exactly once at the target.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

#### ST-I5 Message Follow Error Bounds

Priority: `P1`

The Follow route must not treat an expired-deadline request, a duplicate, or a forwarding loop as new
work.

**Verification question:** Do expired/duplicate/loop inputs end in a bounded terminal with no
duplicate handler run?

- Starting condition: The proxy reproduces the same operation with an expired deadline and an A↔B
  old route.
- Procedure: Each input is delivered as a separate operation.
- Verification: Each caller receives exactly one terminal, and the operation ID's handler count is at
  most one. A current-route follow-up succeeds normally.
- Contract basis: [Framework Error Model](../spec/32-framework-error-model.en.md)

#### ST-I6 Multi-Hop Relocation

Priority: `P1`

Even after consecutive moves like A→B→C, the current route must point at the last owner, and the
stale route chain must not repeat message delivery.

**Verification question:** After a multi-hop completes, are the old A/B route messages not
duplicated, and is the global request processed at C?

- Starting condition: Actor nodes A, B, and C, and a short Message Follow duration, are prepared.
- Procedure: The A→B and B→C Joins are completed, then the A/B old-route messages and a global-ID
  request are sent.
- Verification: The global request is processed exactly once at C. Each old-route operation is
  processed at most once, and not at all after Follow expires.
- Contract basis: [Spot Actor](../spec/15-spot-actor.en.md)

## 4. Completion Criteria

- Every Join/Relocate/message/bind operation is started by a role server through the public
  Framework API.
- Pass/fail judgment uses only the client result, public Actor/Spot ref, lifecycle
  callback/handler evidence, and client push.
- Temporary queues, Location Store rows, relocation payloads, and Session route-update packets are
  not inspected directly.
- An accepted operation has exactly one terminal, and a handler operation ID appears at most once.
- Fixed sleep, exact scheduler timing, internal retry counts, and small-sample latency are not used
  as pass conditions.
- If a supported language's public API is lacking, the scenario is not bypassed — the contract gap
  is recorded in the feature map instead.
