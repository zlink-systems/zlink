<!-- framework-adapter-nav:start -->
[E2E Index](README.en.md) | [Previous: Location Messaging](config-1-location-messaging.en.md) | [Next: Pub/Sub](config-3-pubsub.en.md)
<!-- framework-adapter-nav:end -->

# Config 2 — A Service Using Spot, Actor, And Session Together

This config deploys a stateful User Spot, an Actor that lives in that Spot, and a Stream Session
bound to the Actor, across several real processes. The Application uses the global SpotId and
ActorId, without computing an owner RID or endpoint. The Framework connects request/send/push through
the public object manager, direct messaging, binding, and Stream API.

The E2E client calls only the role servers' application endpoint and Stream endpoint. Location Store
rows, private mailboxes, raw frames, and internal creation barriers are not used. Evidence needed for
factory/handler/lifecycle callbacks is recorded as application state.

## 1. Verification Scope

- Entry/User Spot creation, state, timers, and global Spot routing
- Actor create, local/remote Join, lifecycle, and direct messaging
- Message flow by Channel/Spot/Logical Multicast direction
- Session bind/rebind, relay, push, disconnect, and Stream lifecycle
- Separating Channel/Node/Spot routes on the same MeshNode transport
- Play node crash, scale-out, lifecycle contention, and placement weight

## 2. Deployment Configuration

| Role | Count | Purpose and reason for separation |
|---|---:|---|
| Location Store | 1 | Provides global Spot/Actor current location and automatic topology. |
| Relocation Store | 1 | Preserves the `PreserveStateWith` payload for cross-node Actor Join. |
| Play node | 2 | The Object Server. Provides an Entry Spot, `SpotWide`/`PerActor` User Spots, and an Actor factory/handler. |
| Session gateway | 2 | The Object Client. Provides Stream Session, Actor binding, and relay, but not an Actor/Spot factory. |
| E2E client | per scenario | Uses the role servers' public endpoint and Stream connector. |

The runner creates new object IDs, Sessions, and evidence markers for each scenario. Operations start
after confirming role health, public topology status, and object readiness. When ordering control is
needed, an application factory or handler waits on a public signal. State is not inferred from a
fixed sleep or handler-selection probability.

## 3. Scenarios

### Track A — Create A Spot And Use It As A Global SpotId

#### SM-A1 An Entry Spot Request Creates A User Spot

Priority: `P0`

The Entry Spot is the application entry point prepared when the Host starts. If a Join request
creates a User Spot, the caller receives the created global SpotId and uses it for subsequent direct
messaging.

**Verification question:** Does an Entry Spot request return a Ready User Spot's ID, with that Spot
processing requests?

- Starting condition: The Entry Spot ID is obtained from the play node's public startup evidence, and
  the Entry Spot is ready.
- Procedure: The caller sends a Join request to the Entry Spot, then sends a state request once using
  the SpotId from the reply.
- Verification: The public manager `Find` returns the reply's SpotId as a Ready ref, and the state
  handler runs exactly once.
- Detailed behavior: verifies [Spot Actor §2](../spec/15-spot-actor.en.md).

#### SM-A2 A User Spot's State Changes Serially

Priority: `P0`

Callbacks of the same Spot are processed in a single execution lane, preserving the order of shared
state changes.

**Verification question:** Do N counter-increment requests converge on a final state of N, regardless
of reply order?

- Starting condition: A User Spot with counter 0 is ready.
- Procedure: N increment requests with unique operation IDs are sent with bounded concurrency.
- Verification: Every request receives exactly one reply, and the final state is N. The handler
  active count never exceeds 1.
- Detailed behavior: verifies [Spot Messaging §7](../spec/12-spot-messaging.en.md).

#### SM-A3 A Global SpotId Reaches The Correct Spot

Priority: `P1`

Even if Spots of the same stable type exist on several nodes, a direct request must be processed
only by the specified global SpotId's current owner.

**Verification question:** Does a Spot A ID's request appear only in A's evidence?

- Starting condition: Spots A and B are ready on different nodes.
- Procedure: A marker request is sent once with Spot A's ID.
- Verification: The marker appears once in A's evidence and not in B's.
- Detailed behavior: verifies [Spot Messaging §3](../spec/12-spot-messaging.en.md).

#### SM-A4 Calling The Current Spot With No Owner Input

Priority: `P0`

The Application decides only the SpotId from a domain key, and the Framework finds the current owner.

**Verification question:** Is a request with the same SpotId processed at the current node, before
and after owner relocation?

- Starting condition: The Spot is ready on play-a, and the caller endpoint takes only the SpotId as
  input.
- Procedure: A request is sent once, the Spot owner is switched to play-b through Host Relocate, then
  the same ID is requested again.
- Verification: The first marker is processed by A, and the second by B. The caller input has no
  MeshName, RID, or endpoint.
- Detailed behavior: verifies [Spot Messaging §3](../spec/12-spot-messaging.en.md).

#### SM-A5 An Application Stage Wrapper Does Not Change The Spot Contract

Priority: `P2`

Stage is a wrapper the Application uses to bundle Spot/Actor/timer API calls; it is not a separate
scheduler or routing layer.

**Verification question:** Do SpotWide and PerActor Stage variants keep each execution mode's public
ordering?

- Starting condition: The same domain behavior is configured in two User Spot execution modes.
- Procedure: A Spot request, a member Actor request, and a timer are run through the application
  wrapper.
- Verification: SpotWide keeps the shared-gate order, and PerActor keeps per-Actor/per-timer lane
  order. The public replies and state match a control flow that doesn't use the wrapper.
- Detailed behavior: verifies [Async Execution Policy](../spec/05-async-execution-policy.en.md).

#### SM-A6 Run A User Spot's Initialize And Close Lifecycle

Priority: `P1`

A User Spot with member Actors is not closed arbitrarily; once all members have left, an exact ref
close calls `OnClosing(ExplicitClose)` exactly once.

**Verification question:** Is close false while an Actor is present, succeeding only after it leaves?

- Starting condition: A User Spot whose initialize callback has finished once has an Actor joined.
- Procedure: It is closed with the current SpotRef, the Actor leaves, then it is closed again with
  the same current ref.
- Verification: The first close is false, and the callback/membership are kept. The second is true,
  and the closing callback runs exactly once.
- Detailed behavior: verifies [Spot Actor §7](../spec/15-spot-actor.en.md).

#### SM-A7 Reject A Stable-Type Conflict For The Same SpotId

Priority: `P1`

A single global SpotId can have only one object kind and stable type in its current incarnation.

**Verification question:** Does GetOrCreate-ing a type-A SpotId with type B return `TypeMismatch`?

- Starting condition: A stable-type-A Spot is ready with a state marker.
- Procedure: A `GetOrCreate` of stable type B is called on the same ID.
- Verification: The call is `TypeMismatch`, and the original Spot's state and handler availability
  are preserved.
- Detailed behavior: verifies [Spot Actor §2](../spec/15-spot-actor.en.md).

#### SM-A8 Reflect A CPU Worker's Result In Spot State

Priority: `P2`

CPU computation runs on a bounded worker pool, and the continuation returns to the Spot execution
context to change state.

**Verification question:** Does a probe progress during a CPU worker Yield, and is the computation
result reflected in Spot state exactly once?

- Starting condition: A Spot is ready that can hold worker completion on an application signal.
- Procedure: A CPU worker call is waited on with Yield, a probe request is sent to the same Spot, then
  the worker is released.
- Verification: The probe finishes before the continuation, and the final state reflects the worker
  result exactly once.
- Detailed behavior: verifies [Async Execution Policy §6](../spec/05-async-execution-policy.en.md).

#### SM-A9 A User Spot Is Published As Ready Only After Initialize Completes

Priority: `P0`

While the factory is creating and initializing an instance, a remote caller must not use the
incomplete Spot as an existing object.

**Verification question:** Do Find/request fail to use the Spot while initialize-held, succeeding
after release?

- Starting condition: A User Spot factory's initialize waits on an application signal.
- Procedure: Create is started, initialize-held is confirmed, and Find/request are attempted from
  another process. The gate is released, and it is called again.
- Verification: During the held window, Find does not return a Ready ref, and there is no handler
  evidence. After Create succeeds, Find and request succeed with the same current ref.
- Detailed behavior: verifies the publication boundary in [Spot Actor §2](../spec/15-spot-actor.en.md).

#### SM-A10 The Entry Spot ID Is A Lifecycle Identity Independent Of The MeshNode RID

Priority: `P0`

Computing the Entry Spot ID as a string combination of the Node RID cannot safely distinguish restart
from an identity conflict.

**Verification question:** Does the Entry Spot ID stay the same within the same Host lifecycle, and
change independently from the RID on a replacement?

- Starting condition: An Object Server with diagnostic prefix `play` is ready.
- Procedure: The Node RID and Entry Spot ID are read from public startup evidence, and a normal
  request is sent. The Host is restarted with a replacement lifecycle, and both IDs are read again.
- Verification: The two IDs are different, valid identities and are stable within the same lifecycle.
  In the replacement, the old Entry ID does not remain current, and a new Entry request succeeds.
- Detailed behavior: verifies [Network Listener Identity §7.3](../spec/10-network-listener-identity.en.md).

#### SM-A11 Reject Entry Spot's Reserved Format As A User/Instance ID

Priority: `P0`

Using the Entry Spot namespace the Framework issues as an Application object ID conflicts with the
current Entry identity.

**Verification question:** Are User create and Instance intent with a reserved Entry-style ID
`InvalidOperation`?

- Starting condition: A valid reserved-format string is prepared.
- Procedure: A User Spot GetOrCreate and an Instance Spot request are each attempted with the same
  ID.
- Verification: Both calls are `InvalidOperation`, with no factory callback or application handler
  evidence.
- Detailed behavior: verifies [Entry Spot in the Glossary](../spec/01-glossary.en.md).

#### SM-A12 Automatic User Spot IDs Differ Across Concurrent Creates

Priority: `P0`

Automatic create must return a globally unique Spot identity when the Application doesn't provide an
ID. Injecting an internal UUID-generator collision is the contract test's responsibility, not the
public E2E's.

**Verification question:** Do concurrent automatic creates all return different SpotIds with
independent state?

- Starting condition: The same stable-type factory and sufficient capacity are ready.
- Procedure: Different callers concurrently run 200 automatic Creates.
- Verification: The successful refs' SpotIds are all different, and each ID's marker request is
  processed exactly once at its own Spot.
- Detailed behavior: verifies [Spot Actor §2](../spec/15-spot-actor.en.md).

#### SM-A13 SpotId Keeps UTF-8 Length And Exact Equality

Priority: `P0`

A SpotId is a case-sensitive exact string of 1–255 UTF-8 bytes. The public E2E does not build an
invalid raw binary frame — it verifies only the public string boundary.

**Verification question:** Do 1-byte and 255-byte IDs succeed, while a 256-byte ID is rejected with
no side effect?

- Starting condition: Public strings of exact byte length, plus `Room/room` and NFC/NFD variants, are
  prepared.
- Procedure: Each valid ID is created, found, and requested, and a 256-byte ID create is attempted.
- Verification: The valid IDs point to distinct objects by exact value. The 256-byte call is a local
  validation error, with no factory evidence.
- Detailed behavior: verifies the same-global-ID rule and Spot ID contract in [Actor Model §2.1](../spec/14-actor-model.en.md).

### Track B — Actor Creation And Spot Membership Change

#### SM-B0 Distinguish Explicit-Type Create From Existing-Only Find

Priority: `P0`

Find does not create a missing Actor — only Create/GetOrCreate run the factory.

**Verification question:** Is a missing Find empty, while concurrent create calls converge on a
single current Actor?

- Starting condition: Two play nodes provide the same stable Actor type and capacity.
- Procedure: A missing ID is Found, then Create and GetOrCreate of the same ID/type are called
  concurrently, then Found again.
- Verification: The first Find is empty, with no factory evidence. The creation results point to a
  single current Actor, and the final Find returns the same generation ref.
- Detailed behavior: verifies [Actor Model §3](../spec/14-actor-model.en.md).

#### SM-B0A Return Actor-Creation Accept And Reject Per Operation

Priority: `P0`

A creation callback's reject for one operation must not be shared with the next caller's reply.

**Verification question:** Do the first rejected call and second accepted call each receive their own
request/terminal?

- Starting condition: The creation callback is configured to reject the first marker and accept the
  second.
- Procedure: Two GetOrCreate calls for the same ActorId are run in an order-controlled concurrent
  flow, then a final Find is called.
- Verification: The first receives a typed Rejected with its own payload, and the second receives
  Created and the current ActorRef. The final Find returns only the accepted Actor, with no
  handler/destroy evidence for the rejected operation.
- Detailed behavior: verifies [Actor Model §3](../spec/14-actor-model.en.md).

#### SM-B1 Join A User Spot On The Same Node

Priority: `P0`

A same-node Join changes the current Spot through membership callbacks, without Actor state
relocation.

**Verification question:** After a local Join, do the Actor's current Spot and follow-up handler
point to the target User Spot?

- Starting condition: The Actor is in play-a's Entry Spot, and the target User Spot is also ready on
  play-a.
- Procedure: The Actor handler starts a Join to the target SpotId, awaits completion, and sends a
  request.
- Verification: Target `OnActorJoin`, `OnJoinedActor`, and source `OnLeaveActor` each run exactly
  once, and the current Spot is the target. The follow-up Actor request is also processed exactly
  once on play-a.
- Detailed behavior: verifies [Spot Actor §4](../spec/15-spot-actor.en.md).

#### SM-B2 Join A User Spot On A Different Node

Priority: `P0`

A cross-node Join restores the same ActorId/ObjectGeneration and application state to the target
Actor instance.

**Verification question:** After a remote Join, does the Actor, having kept state and generation,
process requests at the target?

- Starting condition: The Actor is in play-a's Entry Spot, and the target User Spot is on play-b.
- Procedure: The counter state is changed, then it Joins the target SpotId, and after completion the
  current ref and state are queried.
- Verification: Join is Accepted, the current location is play-b, and generation and counter are the
  same as before. The public lifecycle/adapter callbacks each run exactly once in the formal order,
  and the follow-up request is processed at the target.
- Detailed behavior: verifies [Spot Actor §5](../spec/15-spot-actor.en.md).

#### SM-B3 Preserve A Typed Actor Request Payload

Priority: `P0`

A typed payload with nested objects, collections, and nullable fields must keep the same application
values across process boundaries.

**Verification question:** Do all application fields of a complex request and reply match the input?

- Starting condition: The Actor handler reflects the received DTO as-is into the reply and evidence.
- Procedure: A request with nested objects, ordered tags, and nullable values is sent to the remote
  Actor.
- Verification: The handler evidence's and reply's field values/collection order match the input.
- Detailed behavior: verifies [Message Model](../spec/04-message-model.en.md).

#### SM-B4 Send A Remote Actor Request To The Current Owner

Priority: `P1`

Even if the caller and Actor owner are different processes, a global ActorId request connects to the
target mailbox and reply route.

**Verification question:** Does the remote Actor process the request exactly once, with the caller
receiving the reply?

- Starting condition: The Actor is on play-b, and the caller server is ready on play-a.
- Procedure: The caller sends a request once using only the ActorId.
- Verification: Only the play-b handler records the marker exactly once, and the caller receives a
  matching reply.
- Detailed behavior: verifies [Actor Model §5](../spec/14-actor-model.en.md).

#### SM-B5 Observe An Actor Request With No Handler

Priority: `P0`

If the Actor exists but has no packet handler, that's a dispatch failure distinct from a missing
target.

**Verification question:** Does a missing-handler request leave a public error and
`no_handler/reply_error` flow evidence?

- Starting condition: The Actor is ready, and a public message-flow observer is registered.
- Procedure: An unregistered packet name's request is sent, then a normal request is sent.
- Verification: The first is a formal error terminal with observer evidence exactly once. The normal
  request succeeds.
- Detailed behavior: verifies [Message Flow Tracing §2.2](../spec/26-message-flow-tracing.en.md).

#### SM-B6 Distinguish Explicit Leave From A Session Disconnect Callback

Priority: `P0`

A Spot-membership leave and a physical Session disconnect are different lifecycle events.

**Verification question:** Does leave run only the leave callback, and disconnect only the disconnect
callback?

- Starting condition: Fresh Actors are placed in a User Spot and bound to a Session, respectively.
- Procedure: Variant A calls public leave, and B abnormally terminates the Stream connection.
- Verification: A runs only `OnLeaveActor` exactly once, with membership changing. B runs only
  `OnDisconnectActor` exactly once for the current binding, with Actor and Spot membership preserved.
- Detailed behavior: verifies [Session Actor Dispatch §6](../spec/20-session-actor-dispatch.en.md).

#### SM-B7 Dispatch Actor Packets After The Membership Callback

Priority: `P1`

If a packet handler starts before the Actor is Ready or the Join commit finishes, incomplete state
could be observed.

**Verification question:** Are packets sent after Join completion processed FIFO at the target Actor?

- Starting condition: The Actor and target Spot are ready.
- Procedure: Sequence-1-through-20 requests are sent after the Join-completion callback.
- Verification: The public callback evidence ends before the Join terminal, the target handler
  sequence is 1 through 20, and the active count is 1.
- Detailed behavior: verifies [Spot Actor §4](../spec/15-spot-actor.en.md).

#### SM-B8 Destroy The Current Incarnation With An Exact ActorRef

Priority: `P1`

Destroy ends only the incarnation of the exact ActorRef.

**Verification question:** Is the current-ref destroy true, a repeat false, and does the old ref
after recreate become `InvalidOperation`?

- Starting condition: The current ActorRef is saved.
- Procedure: Destroy is called twice with the same ref, the same ActorId is recreated, then destroy is
  called again with the old ref.
- Verification: The results are true, false, `InvalidOperation` in order, and the recreated Actor
  processes requests.
- Detailed behavior: verifies [Failover Policy §4.1](../spec/31-failure-failover-policy.en.md).

#### SM-B9 Distinguish A Target Spot's Join Accept From Reject

Priority: `P1`

The target `OnActorJoin` approves or rejects an existing Actor's membership proposal. Reject does not
change the source.

**Verification question:** Does accept switch to target membership while reject keeps source
membership?

- Starting condition: An accept target and a reject target are prepared as separate User Spots.
- Procedure: Local/remote accept and reject variants are run on fresh Actors.
- Verification: Accept returns completion Accepted and the target current Spot. Reject is typed
  Rejected, with no target-joined/source-leave callback, and the source follow-up request succeeds.
- Detailed behavior: verifies [Spot Actor §4](../spec/15-spot-actor.en.md).

#### SM-B10 Verify Object Role And Location Store Prerequisites

Priority: `P0`

Object Client/Server and Actor dispatch require the Location Store. A role-None manual Host provides
only Node/Channel.

**Verification question:** Do object hosts missing the Store fail at startup, while a role-None manual
Channel works?

- Starting condition: An object role without a Store, Actor dispatch without a Store, and a
  role-None manual configuration are each built.
- Procedure: The negative hosts and the manual host are started, and a manual Node/Channel request is
  sent.
- Verification: The negative hosts are a configuration error before listener readiness. The manual
  request succeeds, and object managers/factory operations are not provided.
- Detailed behavior: verifies [MeshNode §4](../spec/13-mesh-node.en.md).

#### SM-B11 An Actor Is Published As Ready Only After Initial Membership Completes

Priority: `P0`

A remote caller must not use an Actor that is mid-factory or mid-initial-Entry-membership as an
existing Actor.

**Verification question:** Do Find/request fail to use the Actor while factory-held, succeeding after
release?

- Starting condition: The Actor factory waits on an application signal.
- Procedure: Create is started, factory-held is confirmed, and Find/request are attempted from
  another process. The gate is released, and it is called again.
- Verification: During the held window, there is no Ready ref or handler evidence. After Create
  completes, Find and request succeed against the current Actor.
- Detailed behavior: verifies [Actor Model §3](../spec/14-actor-model.en.md).

### Track C — Confirm Message Direction Between Channel And Spot

#### SM-C1 Send A Spot Request From A Channel Handler

Priority: `P0`

The role server handling a Channel request can call a stateful Spot with a global SpotId and fold its
reply into the original Channel reply.

**Verification question:** Does the Channel caller receive exactly one reply that includes the final
Spot state?

- Starting condition: The Channel handler and target Spot are ready.
- Procedure: The caller sends the operation ID as a Channel request, and the handler runs a Spot
  request with the same ID.
- Verification: The Spot handler runs exactly once, and the caller receives exactly one reply that
  includes the Spot result.
- Detailed behavior: verifies [Spot Messaging §3](../spec/12-spot-messaging.en.md).

#### SM-C2 Send A Channel Request From A Spot Handler

Priority: `P0`

A Spot can wait on a ChannelName request from its own callback and reflect the result in Spot state.

**Verification question:** Is the downstream Channel reply reflected exactly once in the original Spot
request and state?

- Starting condition: The Spot and remote Channel handler are ready.
- Procedure: The Spot request waits on the Channel request with Async or allowed Yield.
- Verification: The Channel and Spot handlers each record the operation ID exactly once, and the
  final reply/state matches the downstream result.
- Detailed behavior: verifies [Channel Messaging §3.2](../spec/08-channel-messaging.en.md).

#### SM-C3 Send A Request From One Spot To Another Spot

Priority: `P1`

A source Spot can call a remote stateful service using only the target SpotId.

**Verification question:** Does the source Spot's request receive the target Spot's reply, reflected
in its own state?

- Starting condition: Source and target User Spots are ready on different nodes.
- Procedure: The source handler sends a request to the target SpotId once.
- Verification: The target marker and the source's final state carry a matching operation ID, and
  the caller's reply arrives exactly once.
- Detailed behavior: verifies [Spot Messaging §3](../spec/12-spot-messaging.en.md).

#### SM-C4 A MeshNode With No Local Spot Publishes A Logical Multicast

Priority: `P1`

A Logical Multicast origin doesn't need to host a local Spot. An Object Client node can also publish
to remote subscriptions by ChannelName and topic.

**Verification question:** Do only matching remote Spots receive the marker from an origin with no
Spot?

- Starting condition: The origin node has no local Spot, and two remote nodes have matching and
  nonmatching subscriptions.
- Procedure: The origin's application endpoint publishes a Logical Multicast once.
- Verification: The matching Spots each receive the marker exactly once, and the nonmatching Spot
  does not.
- Detailed behavior: verifies [Spot Messaging §4](../spec/12-spot-messaging.en.md).

#### SM-C5 Judge Logical Multicast Remote Delivery Through Subscriber Evidence

Priority: `P0`

The publish terminal is not remote-receipt confirmation. The E2E must separately confirm target
handler evidence.

**Verification question:** Do subscribed Spots at positive-weight remote nodes each receive the same
marker exactly once?

- Starting condition: Remote node weights 1, 10000, and 0 are set up as variants, with matching Spots
  at the positive nodes.
- Procedure: The source Spot publishes a unique marker.
- Verification: The Spots at positive nodes each receive it exactly once, and the weight-0 node is
  excluded as a new target. It does not pass on publish terminal alone.
- Detailed behavior: verifies [Spot Messaging §4](../spec/12-spot-messaging.en.md).

#### SM-C6 Isolate Logical Multicast Partial Backpressure From Other Targets

Priority: `P0`

Even if one target cannot accept a message, delivery to a target that is already acceptable must not
be rolled back or the same publish auto-re-run.

**Verification question:** When a blocked target and a ready target coexist, does only the ready
target process the marker exactly once?

- Starting condition: One remote target prepares a handler gate and a deterministic payload larger
  than the public HWM. A blocker payload is sent first to confirm handler entry and an Application
  receive-paused state. The other target is ready.
- Procedure: A marker is published once.
- Verification: The public terminal ends in a formal meaning with no per-target result, and the ready
  target processes the marker exactly once. Private snapshots/attempt counts are not read.
- Detailed behavior: verifies [Spot Messaging §4](../spec/12-spot-messaging.en.md).

### Track D — Confirm Session Binding, Relay, And Stream Lifecycle

#### SM-D1 Bind A Local Actor To A Session And Relay

Priority: `P0`

Once the Session gateway and Actor-owner route are ready, a client request is relayed to the bound
Actor, and the Actor's push returns to the same client.

**Verification question:** Do a local-owner Actor request reply and push both reach the bound Stream
client?

- Starting condition: The Session and Actor are ready, and an exact ActorRef is bound.
- Procedure: The client sends a request with Actor ID metadata, and the Actor sends a push once.
- Verification: The Actor handler processes the request exactly once, and the client receives a
  matching reply and push exactly once each.
- Detailed behavior: verifies [Session Actor Dispatch §5](../spec/20-session-actor-dispatch.en.md).

#### SM-D2 Bind A Remote Actor To A Session And Relay

Priority: `P0`

Even if the Actor owner is a different process from the gateway, the binding route connects request
and push.

**Verification question:** Do the remote Actor's relay and push both reach the same client through
the gateway?

- Starting condition: The Actor is on play-b, the Session is on session-a, and an exact-ref bind is
  complete.
- Procedure: The SM-D1 request and push are repeated.
- Verification: The play-b handler processes the request, and the client receives the reply/push.
  The caller provides no RID or endpoint.
- Detailed behavior: verifies [Session Actor Dispatch §5](../spec/20-session-actor-dispatch.en.md).

#### SM-D3 Entry And User Spot Actor Binding Have The Same Meaning

Priority: `P1`

Session binding is independent of the Actor's current Spot kind.

**Verification question:** Do an Entry Actor and a User Spot Actor produce the same bind/relay/push
result?

- Starting condition: Fresh Actors are prepared in Entry and User Spots, respectively.
- Procedure: Each binds to a separate Session, and request/push are run once each.
- Verification: Both variants provide a matching reply/push exactly once, with membership unchanged.
- Detailed behavior: verifies [Actor Model §2.3](../spec/14-actor-model.en.md).

#### SM-D4 Bind Several Actors To One Session

Priority: `P0`

A single Session can hold several Actor bindings, and the Application selects the target binding by
inbound metadata.

**Verification question:** Are packets and pushes with an Actor ID delivered only to the specified
Actor?

- Starting condition: Actors X and Y are bound to the same Session.
- Procedure: X/Y metadata requests and per-Actor pushes are sent. A missing-metadata request is also
  sent once.
- Verification: Each Actor processes only its own marker, and the client receives distinct
  replies/pushes. The missing target is a public dispatch error with neither Actor processing it.
- Detailed behavior: verifies [Session Actor Dispatch §3](../spec/20-session-actor-dispatch.en.md).

#### SM-D4A Isolate A Stale Session After Rebind

Priority: `P0`

Once an Actor is rebound to Session B, Session A's old binding identity is no longer current.

**Verification question:** Does Session A's late relay/disconnect not change Session B's binding and
Actor state?

- Starting condition: Actor X is bound to A, then explicitly rebound to B.
- Procedure: A relay and disconnect held on A's network gate are delivered after B's bind completes,
  then a normal relay/push is run on B.
- Verification: The old operations end in a stale result with no handler evidence. B's relay/push
  each succeed exactly once, and the current binding is B.
- Detailed behavior: verifies [Session Actor Dispatch §4](../spec/20-session-actor-dispatch.en.md).

#### SM-D4B Use Message Follow On The Stored Binding Route After Relocation

Priority: `P0`

Session binding stores a validated route. After Actor relocation, if there's an active Message
Follow, a relay on the old route is delivered to the current owner exactly once; if there's no
mapping, it's `Unavailable`.

**Verification question:** Does the active-follow-route variant succeed, and does the expired variant
return `Unavailable`?

- Starting condition: The Actor is bound, then relocated to a remote owner.
- Procedure: A relay is sent within the active follow window, and on a fresh fixture, a relay is
  sent after the window expires.
- Verification: The active marker is processed exactly once at the target Actor. The expired request
  is `Unavailable`, with no handler evidence. This applies only to a same-incarnation relocation the
  Application did not rebind.
- Detailed behavior: verifies [Session Actor Dispatch §5](../spec/20-session-actor-dispatch.en.md).

#### SM-D5 Notify All Current Bindings Of A Physical Disconnect

Priority: `P0`

If the Stream connection drops, the Framework automatically submits a disconnect callback to each
Actor in the current binding snapshot.

**Verification question:** Do multiple bound Actors each receive the disconnect callback at most
once, keeping membership?

- Starting condition: Several local/remote Actors are bound to one Session.
- Procedure: The Stream connection is abnormally terminated. One Actor callback returns an
  application error.
- Verification: Every current Actor's callback is attempted exactly once, and one failure does not
  block the rest. The public current Spot and ObjectGeneration are preserved.
- Detailed behavior: verifies [Session Actor Dispatch §6](../spec/20-session-actor-dispatch.en.md).

#### SM-D5A Notify A Selected Actor Of A Logical Disconnect

Priority: `P0`

An Application logical disconnect applies to one selected current binding only, not the entire
physical connection.

**Verification question:** Does only the selected Actor's callback run, with other bindings and the
connection preserved?

- Starting condition: Actors X and Y are bound to the same active Session.
- Procedure: A public logical-disconnect operation is called on X, and a relay is sent to Y.
- Verification: Only X's callback runs exactly once, and Y's relay succeeds. The connection and both
  Actors' membership are preserved.
- Detailed behavior: verifies [Session Actor Dispatch §6](../spec/20-session-actor-dispatch.en.md).

#### SM-D6 A Push Is Received Only By The Currently Bound Session

Priority: `P0`

An Actor push targets exactly one current binding — it is not broadcast to unbound clients.

**Verification question:** Does only the bound client receive the state-change push?

- Starting condition: Client A is bound to the Actor, and B is only connected.
- Procedure: A backend request changes Actor state, triggering a push.
- Verification: A receives the marker exactly once, and B does not.
- Detailed behavior: verifies [Session Actor Dispatch §5](../spec/20-session-actor-dispatch.en.md).

#### SM-D7 Allow Packet Dispatch After Stream Auth

Priority: `P0`

An unauthenticated connection does not dispatch business packets — the Session handler runs only
after a successful auth.

**Verification question:** Does a request succeed after valid auth, while an invalid-auth connection
gets a formal close/error?

- Starting condition: Valid and invalid credentials are prepared.
- Procedure: Separate connectors authenticate, then send the same packet.
- Verification: Only the valid connector receives a reply, with handler evidence. The invalid
  connector receives a public auth error or close reason, with the handler not running.
- Detailed behavior: verifies [Stream Session §3](../spec/19-stream-session.en.md).

#### SM-D8 A Stream Reconnect Requires A New Auth/Bind

Priority: `P1`

Since a reconnect is a new physical Session, previous pending requests and bindings are not
automatically restored.

**Verification question:** Does the disconnect-time pending request fail, with a new request
succeeding only after explicit auth/rebind on reconnect?

- Starting condition: An authenticated, bound Session and a slow pending request exist.
- Procedure: The connection is dropped and the pending terminal confirmed. It reconnects,
  auth/rebinds, and a new request is sent.
- Verification: The old request is a disconnected failure and is not replayed. Only the new request
  is processed exactly once at the Actor.
- Detailed behavior: verifies [Failover Policy §6](../spec/31-failure-failover-policy.en.md).

#### SM-D9 A Public Inbound Observer Records A Stream Packet

Priority: `P1`

Inbound observability provides packet kind/name/sequence as formal fields, without duplicating the
payload as a success condition.

**Verification question:** Do the inbound observer evidence and handler evidence carry the same
packet identity?

- Starting condition: A public inbound observer and a handler are registered.
- Procedure: A request and a one-way packet are each sent once.
- Verification: The observer records both identities exactly once, matching the handler results.
- Detailed behavior: verifies [Stream Session §8](../spec/19-stream-session.en.md).

#### SM-D10 Isolate Stream Backpressure Per Session

Priority: `P1`

One Session's slow consumer must not block another Session's send/reply.

**Verification question:** Do B's request and push complete even while Session A is pending on the
public HWM?

- Starting condition: A and B are placed on separate Session gateway processes. Only A's gateway uses
  a small public `ApplicationHwmBytes` and an application receive gate, while B's gateway operates
  normally with a separate HWM boundary. A's client receive is blocked with the gate, and A's public
  status confirms receive-paused.
- Procedure: Sends are started to A, confirming the source awaitable is pending, then B's
  request/push are run. A's gate is released.
- Verification: B's results complete before A's gate is released. A's operations each have exactly
  one success or deadline terminal, with Session state not corrupted.
- Detailed behavior: verifies [Stream Session §7](../spec/19-stream-session.en.md).

#### SM-D11 Separate Stream And Channel Requests On The Same Client

Priority: `P1`

Even when the same Application uses both transport surfaces together, reply correlation is kept per
operation.

**Verification question:** Do interleaved Stream/Channel requests each receive only their own
payload's reply?

- Starting condition: The Stream Session and Channel target are ready.
- Procedure: 50 requests with distinct markers are interleaved on each surface.
- Verification: All 100 replies exactly correspond to their operation ID and input surface, with no
  cross-delivery.
- Detailed behavior: verifies [Interaction Model](../spec/03-interaction-model.en.md).

#### SM-D12 Rebind Actor State After Reconnecting To A Different Gateway

Priority: `P0`

Since the Session-owner process and the Actor owner are separate, Actor state is preserved even when
the gateway changes. Binding is re-created on the new Session.

**Verification question:** Does messaging continue with the same Actor state after
Session-b auth/rebind?

- Starting condition: The Actor is bound to session-a, with state counter 10.
- Procedure: The connection is dropped, it reconnects/authenticates to session-b, binds with the
  current ActorRef, and sends a state request.
- Verification: The counter keeps increasing from 10, and reply/push arrive at session-b. The old
  binding is not reused.
- Detailed behavior: verifies [Failover Policy §6](../spec/31-failure-failover-policy.en.md).

#### SM-D13 Handle Stream Heartbeat Loss As A Disconnect

Priority: `P1`

A connection with normal heartbeat is kept; a Session whose heartbeat stops is treated as disconnected
after the configured deadline.

**Verification question:** After a heartbeat blackhole, are the connector disconnect and the bound
Actors' callbacks observed?

- Starting condition: A bound Session is exchanging heartbeats normally.
- Procedure: The runner blocks the heartbeat direction and awaits disconnect using the public
  deadline plus tolerance.
- Verification: The connector is Disconnected, and the current bound Actors each receive the
  disconnect callback at most once.
- Detailed behavior: verifies [Stream Session §6](../spec/19-stream-session.en.md).

#### SM-D14 Perform Auth/Relay/Push On A TLS Stream

Priority: `P2`

TLS changes transport security but not the application meaning of Session/binding.

**Verification question:** Does a valid certificate succeed at the SM-D2 flow, while an invalid
certificate is rejected before auth?

- Starting condition: Server variants with a valid trust chain and an invalid certificate exist.
- Procedure: TLS connectors connect to both endpoints.
- Verification: The valid connection completes auth/bind/relay/push. The invalid connection ends in a
  public TLS error, with the Session handler not running.
- Detailed behavior: verifies [Stream Session §10](../spec/19-stream-session.en.md).

#### SM-D15 Complete A Channel→Actor→Bound-Session Push Chain

Priority: `P0`

A state change started by a different backend role must reach the final Stream client through
Channel, direct Actor, and bound push.

**Verification question:** Does the bound client actually receive the push for one operation marker?

- Starting condition: The backend Channel, bound Actor, and Stream client are ready.
- Procedure: A backend request starts an Actor send, and the Actor handler sends a bound push.
- Verification: The client receives the marker push exactly once. The public flow trace connects
  each hop as the same flow.
- Detailed behavior: verifies [Flow Correlation §5](../spec/27-flow-correlation.en.md).

### Track E — Confirm Negative Dispatch And Timer

#### SM-E1 Observe A Spot Request With No Handler

Priority: `P0`

If a ready Spot has no packet handler, the caller and observer must be able to confirm the dispatch
error.

**Verification question:** Does a missing Spot handler produce an error reply and
`no_handler/reply_error` evidence?

- Starting condition: The Spot and a public message-flow observer are ready.
- Procedure: A missing-packet request and a normal-packet request are sent.
- Verification: The first returns a formal error with observer evidence; the second returns a normal
  reply exactly once.
- Detailed behavior: verifies [Message Flow Tracing §2.2](../spec/26-message-flow-tracing.en.md).

#### SM-E2 A Spot One-Shot Timer Changes State

Priority: `P1`

A timer callback changes application state in the Spot execution lane and leaves public evidence.

**Verification question:** Does a one-shot timer run exactly once, changing the counter and push
exactly once?

- Starting condition: A Spot with counter 0 and a bound notification target exist.
- Procedure: A one-shot timer is registered through the public Spot context, and callback evidence is
  polled with a bounded wait.
- Verification: The callback count and counter delta are both 1, and the client push is also exactly
  one.
- Detailed behavior: verifies [Spot Messaging §6](../spec/12-spot-messaging.en.md).

#### SM-E3 An Idle Timer Starts An Explicit Close

Priority: `P1`

The Framework does not guess inactivity and auto-close. An application timer checks last activity
and membership and calls explicit close.

**Verification question:** Is only the idle, empty Spot closed, while an active or member-containing
Spot is kept?

- Starting condition: Idle-empty, active-empty, and idle-member-containing Spots are prepared.
- Procedure: Timer callbacks are made to check each state, and public Find/closing evidence is
  collected.
- Verification: Only the idle-empty Spot is closed, with a callback reason of ExplicitClose. The
  other two keep processing requests.
- Detailed behavior: verifies [Spot Actor §7](../spec/15-spot-actor.en.md).

#### SM-E4 Confirm Observable Sequences Per Timer Overrun Policy

Priority: `P1`

When a handler takes longer than the interval, `SkipLateTicks`, `CatchUpBounded`, and
`DelayNextTick` produce different callback sequences.

**Verification question:** After an application-gate-induced overrun, do the callback count/spacing
match the configured policy?

- Starting condition: Fresh timers with the same interval are made per policy, with the first
  callback held on a gate.
- Procedure: After several due boundaries pass, the gate is released, and callback timestamps in a
  bounded observation window are collected.
- Verification: Each policy follows the spec's skip, bounded catch-up, or delayed-next rule. Exact
  scheduler nanoseconds and thread timing are not compared.
- Detailed behavior: verifies [Spot Messaging §6](../spec/12-spot-messaging.en.md).

### Track F — Channel/Node/Spot Routes Coexist On The Same MeshNode Transport

#### SM-F1 Process A Same-Node Spot Direct Request And Send

Priority: `P0`

A same-process optimization must not change the public reply/send meaning.

**Verification question:** Are a same-node request reply and send marker each observed once at the
target Spot?

- Starting condition: The caller and target Spot are in the same MeshNode process.
- Procedure: A SpotId request and send are each started once.
- Verification: The request-reply and send-handler evidence match the input markers.
- Detailed behavior: verifies [Spot Messaging §3](../spec/12-spot-messaging.en.md).

#### SM-F2 Call A Spot Of A Different MeshNode By SpotId

Priority: `P0`

A remote Spot direct caller also does not input target-owner details.

**Verification question:** Does the remote target handler process the request and send exactly once
each?

- Starting condition: The source and target MeshNodes are ready, and the User Spot is on the target.
- Procedure: The source endpoint uses only the SpotId to send a request and send.
- Verification: Only the target's evidence increases, and the request's reply returns to the source.
- Detailed behavior: verifies [Spot Messaging §3](../spec/12-spot-messaging.en.md).

#### SM-F3 Separate ChannelName/Node-Direct/Spot-Direct Namespaces

Priority: `P0`

Even using the same packet name, a different target surface distinguishes the corresponding handler
and reply context.

**Verification question:** Do three requests each reach the Channel, Node, and Spot handlers exactly
once?

- Starting condition: Three handlers with the same packet name are ready on the same MeshNode.
- Procedure: A unique-marker request is sent through each public target API.
- Verification: Each handler processes only its own marker, and the caller receives matching
  replies.
- Detailed behavior: verifies [Interaction Model §3](../spec/03-interaction-model.en.md).

#### SM-F4 Distinguish A Missing Spot From A Stale SpotRef

Priority: `P0`

A SpotId message finds the current logical object, while an exact SpotRef close limits a specific
incarnation.

**Verification question:** Are missing direct calls `NotFound`, and an old-ref close
`InvalidOperation`?

- Starting condition: A missing ID and an old SpotRef of a closed-and-recreated same ID are prepared.
- Procedure: Missing request/send and the old-ref close are run.
- Verification: The direct calls are `NotFound`, the old-ref close is `InvalidOperation`, and the
  recreated Spot processes requests.
- Detailed behavior: verifies [Failover Policy §4.1](../spec/31-failure-failover-policy.en.md).

#### SM-F5 Closing A Spot Does Not Terminate The MeshNode Channel

Priority: `P0`

A User Spot's lifecycle is separate from its containing MeshNode's and Channel handler's lifecycle.

**Verification question:** After the Spot closes, does only the Spot call fail while the Channel
request keeps succeeding?

- Starting condition: The User Spot and Channel handler are ready on the same MeshNode.
- Procedure: Both requests are confirmed, the Spot is closed, then both are called again.
- Verification: The Spot request is NotFound, and the Channel request receives a normal reply. The
  MeshNode status is ready.
- Detailed behavior: verifies [MeshNode §4](../spec/13-mesh-node.en.md).

#### SM-F6 Handle A Cross-Node Spot Call And Actor Join On The Same RouteMesh

Priority: `P0`

Spot direct and cross-node Actor Join use the same MeshNode transport, but each keeps its own target
identity and lifecycle.

**Verification question:** Do a remote Spot request/send and an Actor Join each complete exactly once
at the target?

- Starting condition: The source Entry Actor is on play-a, and the target User Spot is on play-b.
- Procedure: The source runs a target-Spot request/send, and the Actor Joins the same target.
- Verification: The Spot handlers and Join callbacks run the formal number of times at the target.
  The Actor's generation/state are preserved, and the follow-up request is also processed at the
  target.
- Detailed behavior: verifies [Spot Actor §5](../spec/15-spot-actor.en.md).

### Track G — Handle Node Crash, Scale-Out, And Placement

#### SM-G1 After A Play-Node Crash, The Application Recreates And Rebinds The Actor

Priority: `P0`

The failover scope here does not have the Framework automatically restore a crashed Actor's state.
The Application creates a new incarnation with the same ActorId after the old authority is
invalidated, and rebinds the Session.

**Verification question:** After the crash, do old Actor calls fail, with messaging recovering after
explicit recreate/rebind?

- Starting condition: play-a's Actor is bound to a Session, and play-b's independent Actor is also
  normal.
- Procedure: play-a is force-terminated, and pending/fresh request results are collected. After the
  old owner is invalidated, the same ActorId is GetOrCreate'd on a replacement or play-b, and rebound
  with the current ref.
- Verification: The old operations are a bounded error and are not auto-retried. The new incarnation
  processes request/push with a different generation, and the old-ref bind is `InvalidOperation`. The
  independent play-b Actor is unaffected.
- Detailed behavior: verifies [Failover Policy §5](../spec/31-failure-failover-policy.en.md) and
  [§6](../spec/31-failure-failover-policy.en.md).

#### SM-G2 Scale-Out Keeps Existing Owners And Only Places New Objects

Priority: `P1`

Adding a node alone does not automatically redistribute existing Actors/Spots. Only a new create uses
the current eligible capacity and weight.

**Verification question:** After play-b is added, are old objects processed on A, and directed new
objects on B?

- Starting condition: An old Actor and Spot are created when only A is eligible.
- Procedure: B is added, its readiness confirmed, and old requests are sent. A's placement weight is
  set to 0, then a new Actor and Spot are created.
- Verification: The old evidence is recorded only on A, and the new evidence only on B. Scale-out
  itself does not change old owners.
- Detailed behavior: verifies [MeshNode §5](../spec/13-mesh-node.en.md).

#### SM-G3 Concurrent Join/Leave Requests Each Produce Exactly One Membership Terminal

Priority: `P1`

Even if lifecycle requests arrive concurrently, each Actor's current membership and callback count
must match the terminal results.

**Verification question:** Do 20 Actors' mixed Join/Leave operations converge on final membership
with no duplicate callback?

- Starting condition: The Actors and source/target Spots are ready.
- Procedure: A per-Actor operation plan is fixed, and concurrent Join or Leave and a state request are
  run.
- Verification: Each operation has exactly one Accepted, Rejected, or formal conflict result. The
  accepted final memberships match the public callback counts, and there is no Actor handler
  overlap.
- Detailed behavior: verifies [Spot Actor §4](../spec/15-spot-actor.en.md).

#### SM-G4 Isolate Many Bound Session Pushes Per Target

Priority: `P2`

Even with many bindings, a push must not be delivered to a different Session.

**Verification question:** Are each Actor's successful pushes delivered only to its own bound client?

- Starting condition: 100 Actors are bound one-to-one to different Sessions.
- Procedure: Each Actor starts a unique-marker push with bounded concurrency.
- Verification: A successful push marker is observed exactly once at the correct client and absent
  from other clients. A failed terminal is not counted as a delivery success.
- Detailed behavior: verifies [Session Actor Dispatch §5](../spec/20-session-actor-dispatch.en.md).

#### SM-G5A Confirm A 100:300 Placement Weight Ratio With A Sufficient Sample

Priority: `P0`

Placement weight is the relative selection ratio for a new object's target — it does not guarantee
exact alternation.

**Verification question:** Across 800 creates on equal-capacity nodes, does the weight-300 node own
65–85%?

- Starting condition: A has weight 100, B has weight 300, and the same type's capacity is
  sufficient.
- Procedure: 800 Actors or User Spots with unique IDs are created.
- Verification: All creates succeed, and the combined owner count is 800. B's share is 65–85%, and
  existing owners are unchanged.
- Detailed behavior: verifies [MeshNode §5](../spec/13-mesh-node.en.md).

#### SM-G5B Exclude A High-Weight Node With No Capacity From New Placement

Priority: `P0`

Before weight computation, only candidates satisfying the stable type and total capacity must remain.

**Verification question:** If high-weight B's capacity is full, does the new create succeed on
eligible A?

- Starting condition: Weight-10000 B's relevant capacity is filled, and weight-1 A has an open slot.
- Procedure: A new global-ID create is run once. Separate startup variants apply weights -1 and
  10001.
- Verification: The create's owner is A, and the factory runs exactly once. The invalid weights are a
  configuration error before listener readiness.
- Detailed behavior: verifies [MeshNode §5](../spec/13-mesh-node.en.md).

## 4. Completion Criteria

- Every scenario uses only the public Framework API, public status/observer, and application
  evidence.
- Factory and handler ordering is controlled by application signals; internal CAS/queue/Store rows
  are not read.
- Stream/Session scenarios confirm the actual client reply/push/close result, not passing on server
  log alone.
- Weighted placement uses a sufficient sample and tolerance, without fixing the target-selection
  order.
- Raw invalid UTF-8 frames, UUID-generator conflicts, and private protocol failures are verified by
  contract/internal tests.
