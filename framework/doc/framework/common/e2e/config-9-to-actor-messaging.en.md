<!-- framework-adapter-nav:start -->
[E2E table of contents](README.en.md) | [Previous: Execution Turn And Terminator](config-8-execution-turn.en.md) | [Next: Spot Actor Join/Relocation](config-10-spot-actor-relocation.en.md)
<!-- framework-adapter-nav:end -->

# Config 9 — Sending A Message Directly By ActorId

A server application can send a send or request to an Actor by its
global `ActorId` without going through a Session. This call targets
the current Ready Actor regardless of whether that Actor is bound to
a Session. Also, sending a direct message must not create a new
Session binding or change an existing one.

This config verifies this contract in a deployment where the caller
and the Actor are in different processes. The E2E client calls the
role server's application endpoint, and the role server executes the
operation via the public Framework API. Framework internal queues,
location records, and private route info aren't used for judgment.

## 1. Verification Scope

- Direct send/request for an Actor bound to a Session versus one not
  bound
- Independence between a direct message and a later Session bind
- The result for an Actor kept alive after Session unbind, versus
  after Actor removal
- The distinction between a non-existent Actor's error and an
  unreachable owner's error
- The difference between an ID-only message to an Actor recreated with
  the same `ActorId`, and a previous `ActorRef`'s lifecycle operation

## 2. Deployment Configuration

| Role | Count | What it does and why it's separate |
|---|---:|---|
| Location Store | 1 | Lets two Actor nodes, the Session gateway, and the caller server look up the same global Actor location. Uses a dedicated namespace per run. |
| Actor node | 2 | Creates the `to-actor.probe` Actor and runs the direct send/request handler. The second node is used to create a condition where the owner route is unavailable. |
| Session gateway | 2 | Accepts a Stream Session and binds/unbinds the Actor via the public binding API. Delivers a bound-session push sent by the Actor to the client. |
| Caller server | 1 | Receives the client's HTTP request and starts a global `ActorId` send/request via the public Actor client API. Doesn't create a Session or bind an Actor. |
| E2E client | 1 | Uses only the role server's public application endpoint and Stream endpoint. Doesn't call framework internal APIs directly. |

The Actor handler records the received packet name, request ID, and
application payload into application state. The Session gateway
provides the public binding-lookup result, and the Stream client
provides the push payload it actually received, as evidence. This
evidence is looked up from the role server's public endpoint and
doesn't expose the internal mailbox, binding token, or route record.

## 3. Common Run And Judgment Method

The runner creates a fresh process, Store namespace, and evidence
marker per scenario. It starts an operation after the role server's
health and public RouteMesh status become ready. The runner only
changes an external condition for a scenario that needs process
termination or a network block.

Send's API completion is distinguished from remote handler execution.
The send call's result is confirmed via the public send terminal, and
actual delivery is confirmed via the Actor handler's application
evidence. Request is judged by the reply the caller server received,
or the public error kind. The file log is only used to find the cause
of a failure.

## 4. Scenarios

### Track A — Separate Session Binding From Direct Message

#### TA-A1 Send A Direct Send/Request To A Bound Actor

Priority: `P0`

Even if an Actor is already bound to a Session, a server-to-server
direct message must be handled by the same Actor handler. If a direct
message changes an existing binding, a later Actor push can be
delivered to the wrong client.

**Verification question:** Even when a direct send/request is sent to
a bound Actor, does the handler process it and does the existing
Session binding stay intact?

- Start condition: The client connects to `session-a` and creates and
  binds `actor-bound`. The Session gateway's public binding lookup
  confirms that Actor, and the client receives the `BeforeNotify` push
  the Actor sent.
- Procedure: The caller server sends one direct send and one request
  to `actor-bound` each. After processing finishes, the Actor sends
  `AfterNotify` via the bound-session API.
- Verification: The Actor handler processes the send and the request
  once each, and the request returns a reply containing the input
  marker. The public binding-lookup result is the same before and
  after, and `BeforeNotify` and `AfterNotify` are received only by the
  originally-connected client.
- Detailed behavior: verifies the separation between direct message
  and binding from
  [Actor Model §5](../spec/14-actor-model.en.md#5-actor-messaging) and
  [Session Actor Dispatch §4](../spec/20-session-actor-dispatch.ko.md#4-session이-actor-route를-보관하는-방법).

#### TA-A2 Send A Direct Send/Request To An Unbound Actor

Priority: `P0`

Direct Actor messaging doesn't presuppose a Session binding. If this
path required a binding, a backend job or another server couldn't call
the Actor directly.

**Verification question:** Does an Actor with no bound Session still
process a direct send and reply to a direct request?

- Start condition: `actor-unbound` is created on an Actor node. Neither
  Session gateway's public binding lookup has this Actor.
- Procedure: The caller server sends one direct send and one request
  to `actor-unbound` each.
- Verification: The Actor handler processes both messages once each,
  and the caller server receives the request reply. No Session binding
  is created afterward either, and there's no push the Stream client
  received.
- Detailed behavior: verifies binding independence from
  [Actor Model §2.3](../spec/14-actor-model.en.md#23-spot-membership-and-stream-binding)
  and [§5](../spec/14-actor-model.en.md#5-actor-messaging).

#### TA-A3 Bind A Session After A Direct Message

Priority: `P0`

Even if an unbound Actor processes a direct message first, the
application must still be able to bind that Actor to a Session later.
If the earlier direct call leaves an implicit binding, the explicit
bind's result would differ.

**Verification question:** Do the two paths not affect each other,
even when an Actor that processed a direct message first is later
bound to a Session?

- Start condition: `actor-late-bind` is created and not bound to a
  Session.
- Procedure: The caller server sends one send and one request each.
  Once processing is confirmed, the client connects to `session-b`
  and binds the same Actor. The caller server sends a send and request
  again, and the Actor sends `LateBindNotify` to the bound Session.
- Verification: The Actor handler processes the direct send/request
  before and after binding, once each. The public binding lookup
  returns no Actor before binding, and returns `session-b`'s binding
  after binding completes. `LateBindNotify` is received only by the
  `session-b` client.
- Detailed behavior: verifies the independence of explicit bind and
  direct message from
  [Session Actor Dispatch §2](../spec/20-session-actor-dispatch.en.md#2-the-whole-flow-the-application-sees).

#### TA-A4 A Direct Message Continues After Unbind And Fails After Actor Removal

Priority: `P0`

Session binding and Actor lifecycle are separate. If only the binding
is released, a direct message must keep being processed while the
Actor is kept alive, and after the Actor is explicitly removed, the
same call must not succeed.

**Verification question:** Does a direct message succeed after unbind
and end in `NotFound` after the Actor is removed?

- Start condition: `actor-unbound-lifecycle` is created and bound to a
  Session. This scenario's application lifecycle policy doesn't remove
  the Actor on unbind.
- Procedure: The Actor is unbound via the Session gateway's public
  API, and the absence of a binding is confirmed. The caller server
  sends one send and one request each. After that, the Actor is
  removed via the public Actor lifecycle API and a new request is sent
  with the same `ActorId`.
- Verification: The two messages after unbind are each processed once
  by the Actor handler. The request after Actor removal ends in
  `NotFound`, and no handler evidence is added.
- Detailed behavior: verifies lifecycle separation from
  [Actor Model §2.3](../spec/14-actor-model.en.md#23-spot-membership-and-stream-binding)
  and
  [Error Model §2](../spec/32-framework-error-model.en.md#2-the-shared-errorkind).

### Track B — Distinguish Logical Target From Failure Result

#### TA-B1 Call A Non-Existent Actor

Priority: `P0`

If a global `ActorId` has no current Actor, Framework must not
arbitrarily create an Actor or hold the message. The application must
be able to distinguish target absence as `NotFound`.

**Verification question:** Does a direct send/request for a
non-existent `ActorId` end in `NotFound` without the handler running?

- Start condition: `actor-missing`, which has never been created in
  the run namespace, is used.
- Procedure: The caller server attempts one send and one request each
  to `actor-missing`.
- Verification: Both operations' public error kind is `NotFound`.
  Neither Actor node's application evidence has that Actor ID or a
  marker.
- Detailed behavior: verifies target-absence classification from
  [Error Model §2](../spec/32-framework-error-model.en.md#2-the-shared-errorkind).

#### TA-B2 An Actor Recreated With The Same ActorId Processes A New Direct Message

Priority: `P0`

An application message's target is the logical `ActorId`. So if an
Actor is removed and a new Actor is created with the same ID at the
same owner, a later direct message is processed by the new Actor. A
previous `ActorRef`, on the other hand, points at a specific
incarnation, so it must not change the new Actor's lifecycle or
binding.

**Verification question:** Does the new Actor process an ID-only
message while a previous `ActorRef`'s lifecycle operation is rejected?

- Start condition: `actor-recreated` is created on an Actor node and
  the first `ActorRef` the public API returned is kept.
- Procedure: The Actor is removed using the first `ActorRef`, then a
  new Actor is created with the same `ActorId` on the same node. The
  caller server sends one send and one request each by `ActorId`. Then
  a bind or destroy is attempted with the kept previous `ActorRef`.
- Verification: The new Actor handler processes the send and the
  request once each and returns a request reply. The previous
  `ActorRef`'s operation ends in `InvalidOperation`, and the new
  Actor's binding and lifecycle don't change.
- Detailed behavior: verifies the distinction between ID-only
  application message and exact-reference control from
  [Failover Policy §4.1](../spec/31-failure-failover-policy.en.md#41-logical-id-messaging-and-objectgeneration).

#### TA-B3 Ends In Unavailable When The Current Owner Is Unreachable

Priority: `P0`

Even if the Actor exists, while the caller can't send a message to the
current owner, that's not target absence but currently unavailable.
Framework doesn't swap this operation to a different Actor or
resubmit it internally.

**Verification question:** Does a request end in `Unavailable` when
the Actor's current-owner route is unusable, and does a new request
succeed after the connection recovers?

- Start condition: `actor-route-down` is created on `actor-b`. The
  caller server's public status sees that route as ready, and a
  normal control request has succeeded.
- Procedure: The runner blocks the network between the caller server
  and `actor-b`, and confirms via public status that the route isn't
  ready. The caller server sends one request. The block is released,
  and once public status is ready again, the application sends a new
  request.
- Verification: The request during the block ends once in
  `Unavailable`, and the Actor handler doesn't process that marker.
  There must be no evidence that Framework auto-switched to a
  different Actor or owner. After recovery, the new request is
  processed once by the same Actor and a reply is returned.
- Detailed behavior: verifies route failure from
  [Failover Policy §2](../spec/31-failure-failover-policy.en.md#2-common-judgment-criteria)
  and
  [Error Model §4](../spec/32-framework-error-model.en.md#4-send-completion-and-failure).

## 5. Completion Criteria

- Every scenario uses only the role server's public Framework API and
  public application endpoint.
- Send completion, remote handler processing, request reply, and
  Session binding are each judged by different public evidence.
- `ActorId` direct messages don't add `ObjectGeneration` as a target
  condition. A lifecycle/binding operation using an exact `ActorRef`
  applies that reference's generation rule.
- State propagation isn't estimated by a fixed sleep. Health and
  public status are used as the start condition of the next operation,
  and each operation must have exactly one terminal result within the
  timeout the spec sets.
- Every language providing Actor direct messaging uses the same
  scenario ID and application marker.
