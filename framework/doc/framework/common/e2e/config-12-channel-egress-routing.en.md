<!-- framework-adapter-nav:start -->
[E2E table of contents](README.en.md) | [Previous: Observability/Operational Deployment](config-11-observability-ops.en.md) | [Next: One-Way Submit Admission](config-13-submit-admission.en.md)
<!-- framework-adapter-nav:end -->

# Config 12 — Selecting The Send Path That Matches A ChannelName

An application specifies the logical target to send a message to via
`ChannelName`. Even if one process uses several RouteMesh and
ClientServer Channels together, Framework must select exactly one send
path registered under that name. Registering the same name under
several different send paths, or routing an unready path through a
different topology, can let an unintended server process the request.

This config verifies that Channel send/request, target selection, and
reply don't get mixed up in a deployment with several processes and
topologies together. The client calls only the role server's
application endpoint, and private egress index, descriptor record, and
physical connection aren't used for judgment.

## 1. Verification Scope

- Bidirectional Channel request and remote Channel selection on
  RouteMesh
- A different Channel request started from a handler, Spot callback,
  or timer
- ClientServer weight, drain, restart, and a Client/Server combination
  in the same process
- Public error when a Channel role is missing or the connection isn't
  ready
- A startup error for registering the same `ChannelName` under
  multiple send paths in one process
- A listener's port `0` bind and advertised endpoint
- ClientServer one-way send

## 2. Deployment Configuration

| Role | Count | What it does and why it's separate |
|---|---:|---|
| Location Store | 1 | Lets automatic RouteMesh and ClientServer discovery use the same run namespace. |
| Session server | 1 | Provides the `game.session` Server on the `game` RouteMesh, and calls `game.play` and `game.api`. |
| Play server | 1 | The `game.play` Server, which also calls `audit.record` on a separate `audit` RouteMesh and the ClientServer `workflow.command`. Also provides a Spot callback and timer. |
| API server | 2 | Configures `game.api` Server membership with different weights. |
| Workflow server | 2 | The ClientServer `workflow.command` Server. One variant also registers a Client role of the same name. |
| Workflow caller | 1 | The ClientServer `workflow.command` Client. |
| Audit server | 1 | The `audit.record` Server on the `audit` RouteMesh. |
| E2E client | 1 | Calls each role's public application endpoint and collects the reply and application evidence. |

Each handler records the operation ID, role name, received payload,
and reply value into application state. The client looks up this
value from the public evidence endpoint. Listener endpoint and Channel
ready state only use the value the formal public status API returned.

## 3. Common Run And Judgment Method

The runner creates a fresh process, port, and Store namespace per
scenario. It sends the message after confirming the role server's
health and that Channel's public ready status. The runner performs
network block, process stop, and restart externally.

Weighted selection doesn't compare exact ordering of a small number of
requests. With targets at weight 100 and 300, it sends 800 different
requests, and passes if the weight-300 target's processing ratio is
65–85%. Every request must get exactly one reply, and the sum of
handler counts must also be 800. Fixed sleep, exact alternation,
internal retry count, and log order aren't used for judgment.

## 4. Scenarios

### Track A — Use The RouteMesh Path Registered Under ChannelName

#### CH-E2E-01 Send A Bidirectional Request On The Same RouteMesh

Priority: `P0`

If two processes provide different Server Channels on the same
RouteMesh, either side must be able to call the other's Channel
without configuring a separate reverse connection.

**Verification question:** Do Session and Play's respective handlers
each process a request sent to the other's ChannelName once?

- Start condition: Session's `game.play` and Play's `game.session` are
  ready in public status. Since each process's local Server membership
  isn't its own RouteMesh candidate, confirm the remote Server path the
  caller uses.
- Procedure: Session sends one `game.play` request, and Play sends one
  `game.session` request.
- Verification: Each request receives exactly one reply containing the
  other role's marker. Each handler processes its own request once,
  and the other role's handler doesn't record the same operation ID.
- Detailed behavior: verifies
  [Channel Topology §4.2](../spec/07-channel-topology.en.md#42-a-channel-call-can-start-without-a-local-server-role)
  and
  [Channel Messaging §3.2](../spec/08-channel-messaging.en.md#32-channelname-select-one).

#### CH-E2E-02 A Handler Calls A Channel Of A Different Topology

Priority: `P0`

While processing the original request, the Play handler calls the
Audit RouteMesh and the Workflow ClientServer. If the three requests'
replies mix up, the original caller could receive a different
operation's result.

**Verification question:** Even after the handler calls two downstream
Channels, is each reply linked to the original operation exactly once?

- Start condition: `game.play`, `audit.record`, and `workflow.command`
  are all ready in public status.
- Procedure: Session sends Play a request containing an operation ID.
  The Play handler requests Audit and Workflow in order with the same
  ID, and puts both results into the original reply.
- Verification: The Audit and Workflow handlers each process that ID
  once. Session receives one reply containing both downstream results
  and doesn't receive a separate unsolicited message.
- Detailed behavior: verifies the distinction between nested request
  and reply from
  [ClientServer Channel §6.2](../spec/09-client-server-channel.en.md#62-when-a-handler-calls-a-different-target).

#### CH-E2E-03 Send A ClientServer Request From A Spot Callback And Timer

Priority: `P1`

Even if a Spot callback waits on a request or a timer changes the same
Spot's state, the Spot's serial-turn contract must be kept. Dispatching
a downstream reply like a new business packet would change the state-
change order.

**Verification question:** After a Spot callback and timer each wait
for a ClientServer reply, do they complete in the defined application-
state order?

- Start condition: Play's Spot, timer, and `workflow.command` are
  ready. The application state's sequence is `0`.
- Procedure: The client calls the Spot handler, making it wait for a
  Workflow request. After the handler finishes, it schedules a timer
  via the public application endpoint and bounded-polls until the
  timer evidence appears.
- Verification: Application evidence is in the order `handler-start,
  workflow-reply, handler-end, timer-start, workflow-reply,
  timer-end`, and the sequence increases once per step. The timer
  result isn't estimated with a fixed sleep.
- Detailed behavior: verifies
  [Async Execution Policy §2](../spec/05-async-execution-policy.en.md#2-request-completion)
  and
  [ClientServer Channel §6.2](../spec/09-client-server-channel.en.md#62-when-a-handler-calls-a-different-target).

#### CH-E2E-06 Fails To Start If The Same ChannelName Is Registered Under Multiple Send Paths

Priority: `P0`

If the same `ChannelName` points at both RouteMesh and ClientServer in
one process, it's undecidable which path to select at call time.
Framework must reject the configuration before publishing the
listener.

**Verification question:** Does a host with a duplicate egress
registration shut down with a configuration error?

- Start condition: A negative host registers `duplicate.channel` on
  both a RouteMesh and a ClientServer Client via the public builder.
- Procedure: The runner starts the negative host and confirms the
  process terminal and health. Then it starts a normal host using
  different names.
- Verification: The negative host doesn't become ready and exits with
  a public configuration error. The normal host has both Channels
  ready and can call each once.
- Detailed behavior: verifies
  [Channel Topology §4.4](../spec/07-channel-topology.en.md#44-in-one-process-a-channelname-points-to-only-one-send-path).

#### CH-E2E-07A An Unregistered ChannelName Is NotFound

Priority: `P0`

If calling a name with no send path in the process falls back to a
different RouteMesh or ClientServer instead, the wrong business
handler runs.

**Verification question:** Does a request to an unregistered
ChannelName end in `NotFound`?

- Start condition: The caller process doesn't register
  `missing.channel` on any topology.
- Procedure: The caller endpoint starts one `missing.channel` request.
- Verification: The public error kind is `NotFound`, and no role
  handler's evidence has the operation ID.
- Detailed behavior: verifies
  [Channel Messaging §3.3](../spec/08-channel-messaging.en.md#33-an-unregistered-channelname).

#### CH-E2E-07B Calls A Remote Member Even With Only A Local Server Role

Priority: `P0`

A RouteMesh Channel's Server role can both provide a handler and start
a request within the same Channel membership. Just because there's no
separate Client role, it must not directly call the local handler or
fail.

**Verification question:** Does an API server correctly call a
different, ready API server of the same ChannelName?

- Start condition: Two API servers are ready as `game.api` Server, and
  a Client role of the same name isn't added to the server starting
  the call.
- Procedure: The first API server's application endpoint starts 20
  `game.api` requests.
- Verification: Each request receives exactly one reply, and at least
  one is processed by a different process's handler. The sum of
  handler counts is 20, with no duplicate operation ID.
- Detailed behavior: verifies
  [Channel Topology §4.2](../spec/07-channel-topology.en.md#42-a-channel-call-can-start-without-a-local-server-role).

#### CH-E2E-07C Unavailable When A Known Target Is Unreachable

Priority: `P0`

Even if target membership is known, while the connection isn't ready,
it's currently unavailable rather than target absence. Framework
doesn't route around to a different topology or repeatedly resubmit
until timeout.

**Verification question:** Does a request end in `Unavailable` when a
known target's connection isn't ready?

- Start condition: After the caller discovers the target descriptor,
  the runner blocks the network toward that target. Confirm via public
  status that the Channel isn't ready.
- Procedure: The caller sends one request containing an operation ID.
- Verification: The request ends in exactly one `Unavailable` terminal,
  with no handler recording the operation ID. It's a failure if a
  different RouteMesh or ClientServer processes it.
- Detailed behavior: verifies
  [Error Model §4](../spec/32-framework-error-model.en.md#4-send-completion-and-failure)
  and
  [§5](../spec/32-framework-error-model.en.md#5-request-completion-and-failure).

#### CH-E2E-11 Call A Different MeshNode's Server Using Only ChannelName

Priority: `P0`

The application selects a remote membership by `ChannelName` alone,
without passing a target RID or endpoint. Requiring the caller to pick
a physical route would break the Channel's location transparency.

**Verification question:** Does Session receive the remote API
handler's reply and send processing by specifying only the name
`game.api`?

- Start condition: Session and two API servers are ready on the same
  `game` RouteMesh.
- Procedure: Session starts one `game.api` request and one send each.
- Verification: The request receives a reply containing the API role's
  marker, and the send marker is recorded once at exactly one API
  handler. The caller endpoint doesn't take MeshName, RID, or endpoint
  as input.
- Detailed behavior: verifies ChannelName select-one from
  [Channel Messaging §3.2](../spec/08-channel-messaging.en.md#32-channelname-select-one).

### Track B — Select A ClientServer Target And Handle Lifecycle

#### CH-E2E-04A Select A Target By ClientServer Weight

Priority: `P0`

If several ClientServer Servers are ready, weight decides the relative
ratio at which a new request picks each target. It doesn't guarantee
exact order over a short span.

**Verification question:** With two servers at weight `100:300`, are
they selected at roughly a `1:3` ratio over enough requests?

- Start condition: Two Workflow servers are ready at weight 100 and
  300 respectively.
- Procedure: The Workflow caller sends 800 requests with different
  operation IDs, sequentially or with limited concurrency.
- Verification: All 800 receive exactly one reply, and the sum of
  handler counts is 800. The weight-300 server's processing ratio is
  65–85%.
- Detailed behavior: verifies
  [ClientServer Channel §5](../spec/09-client-server-channel.en.md#5-weight-and-target-selection).

#### CH-E2E-04B A Draining Server Is Excluded From New Requests

Priority: `P0`

Drain lets already-accepted requests finish while stopping new-request
selection. Immediately failing in-progress requests, or continuing to
accept new requests, would make a graceful shutdown impossible.

**Verification question:** Does a request accepted before drain
complete, and does a new request after drain get processed by a
different server?

- Start condition: Two Workflow servers are ready. Server A's handler
  is configured to hold the first request's reply until it receives an
  application signal.
- Procedure: Confirm via public evidence that the first request
  arrived at handler A. Start a public drain operation on A, then send
  50 new requests. Finally, release handler A's signal.
- Verification: The first request completes once with A's reply. All
  50 new ones are processed by B, with no additional marker on A.
- Detailed behavior: verifies
  [ClientServer Channel §7](../spec/09-client-server-channel.en.md#7-drain-blocking-new-requests-and-finishing-already-received-ones).

#### CH-E2E-04C Process New Requests After A Server Restart

Priority: `P0`

Once a server process restarts, the previous lifecycle's connection
and reply are no longer current. The client must process a request
started after the new server is ready in the new lifecycle.

**Verification question:** After a Workflow server restart, does the
first new request succeed with no application retry or fixed settle?

- Start condition: Only server A runs, and a normal control request
  has succeeded.
- Procedure: The runner stops A and confirms not-ready via public
  status. It starts the same role as a new process, and the moment
  ready is confirmed, sends one request with a new operation ID.
- Verification: The new request receives exactly one reply containing
  the restarted server's lifecycle marker. The previous process's
  marker doesn't appear in the new request's evidence.
- Detailed behavior: verifies
  [ClientServer Channel §8](../spec/09-client-server-channel.en.md#8-server-restart).

#### CH-E2E-05 A Process Without A Client Role Can't Start A ClientServer Request

Priority: `P1`

On ClientServer, only a process that registered a Client role has a
server connection and send path. A process with only a Server role
must not call the same name and directly execute the local handler.

**Verification question:** Does a ClientServer request from a process
registered with only a Server role end in `NotFound`?

- Start condition: The negative Workflow process registers only the
  `workflow.command` Server role. A separate normal caller is ready
  with a Client role.
- Procedure: The negative process and the normal caller each start one
  request.
- Verification: The negative process's request is `NotFound` and the
  handler doesn't run. The normal caller's request is processed once
  by the Workflow handler.
- Detailed behavior: verifies role responsibility from
  [ClientServer Channel §3](../spec/09-client-server-channel.en.md#3-client-and-server-roles).

#### CH-E2E-10 ClientServer One-Way Send Doesn't Produce A Reply

Priority: `P0`

A one-way send submits a message to exactly one ready server and
doesn't wait for a request reply.

**Verification question:** Is a ClientServer send processed once by
exactly one handler, without producing a client reply?

- Start condition: Two Workflow servers and the caller are ready.
- Procedure: The caller submits one send with a unique marker and
  bounded-polls the public handler evidence.
- Verification: The send's public terminal succeeds, and the marker is
  recorded once at exactly one server's handler. The client has
  neither a request completion nor an unsolicited payload.
- Detailed behavior: verifies
  [ClientServer Channel §6](../spec/09-client-server-channel.en.md#6-send-request-and-reply).

#### CH-E2E-12 A Client And Server In The Same Process Are Also Selected As Normal Candidates

Priority: `P0`

One process can register a Client and Server of the same ClientServer
Channel, one per role. The local Server also applies the same weight
rule as a remote Server, without being unconditionally prioritized or
excluded.

**Verification question:** If a local and remote Server have the same
weight, are both selected over enough requests?

- Start condition: Workflow process A registers both a
  `workflow.command` Client and Server, and process B registers a
  Server of the same name. Both servers' weight is 100.
- Procedure: A's application endpoint sends 400 requests.
- Verification: All 400 receive exactly one reply, and the local and
  remote handlers each process 35–65%. The sum of handler counts is
  400, and the local call gets the same application result as the
  remote one.
- Detailed behavior: verifies
  [ClientServer Channel §5.1](../spec/09-client-server-channel.en.md#51-a-server-in-the-same-process-is-also-a-selection-candidate).

### Track C — A Channel Handler Calls A Different Public Target

#### CH-E2E-08 A ClientServer Handler Calls Spot And Actor In Sequence

Priority: `P1`

A ClientServer handler can call a Spot and Actor registered on
RouteMesh via the public state-address API. If each operation's
identity and reply mix up, the original ClientServer request could
complete incorrectly.

**Verification question:** After the Workflow handler finishes the
Spot and Actor requests, does it complete the original reply exactly
once with both results?

- Start condition: The Workflow server is an Object Client on the
  `game` RouteMesh, and has ready Spot and Actor IDs as an application
  fixture.
- Procedure: The Workflow caller sends a request containing an
  operation ID. The handler runs the Spot request and Actor request in
  order.
- Verification: The Spot and Actor handlers each process the same
  operation ID once. The Workflow caller receives exactly one reply
  containing both results, and no separate payload.
- Detailed behavior: verifies
  [ClientServer Channel §6.2](../spec/09-client-server-channel.en.md#62-when-a-handler-calls-a-different-target)
  and
  [Spot Messaging](../spec/12-spot-messaging.en.md).

### Track D — Fix The Remote Listener Address

#### CH-E2E-09 Build A Remote Connection With Port 0 And An Advertised Host

Priority: `P0`

Binding to port `0` lets the OS decide the actual port. Since a
wildcard bind address isn't an address the remote can connect to,
Framework must combine the confirmed port with `AdvertiseHost` when
publishing.

**Verification question:** Do all four listener kinds start with port
0, provide a public endpoint, and get an actual connection from a
remote client?

- Start condition: RouteMesh, ClientServer, classic fanout publisher,
  and Stream server are configured with port 0, a wildcard BindHost,
  and a reachable AdvertiseHost.
- Procedure: After each listener's public status becomes ready, read
  the actual bound port and advertised endpoint. A corresponding
  remote client sends one normal message on each topology.
- Verification: Every actual port is non-zero, and the advertised
  endpoint has neither the wildcard host nor port 0. Each remote
  client receives the corresponding handler or subscriber result, and
  doesn't connect to a different topology's endpoint.
- Detailed behavior: verifies
  [Network Listener Identity §4](../spec/10-network-listener-identity.en.md#4-how-to-confirm-the-port)
  and
  [§5](../spec/10-network-listener-identity.en.md#5-records-per-listener-kind).

## 5. Completion Criteria

- Every procedure and judgment uses only the public Framework API,
  public status, and the role server's application evidence.
- Internal egress index, descriptor record, reply token, socket count,
  and physical connection ID aren't a pass condition.
- A weighted scenario uses a sufficient sample and a stated tolerance,
  and doesn't require exact selection order.
- Readiness and handler completion are confirmed via bounded polling,
  not dependent on a fixed sleep or log flush order.
- Every request must have exactly one terminal result among reply,
  public error, timeout, or cancellation.
