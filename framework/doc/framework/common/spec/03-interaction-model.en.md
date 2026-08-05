---
title: "ZLink Framework Interaction Model"
---

# ZLink Framework Interaction Model

[Spec table of contents](README.en.md) · [Previous: ZLink Framework Overview](02-overview.en.md) · [Next: Framework Message Contract](04-message-model.en.md)

> **What this chapter defines** — the target, completion meaning, and execution
> owner of framework operations.


## 1. Purpose

This document defines the target, completion meaning, and execution owner of ZLink
Framework operations. The exact per-language method signature is owned by each
package's `languages/<lang>/` documents.

The method of publishing one message to several remote nodes participating in the
same Channel and to local Spots is called `Logical Multicast`. The value the
Location Store records for which node currently processes a global
[Spot](01-glossary.en.md#spot) or Actor is called `authority`.

## 2. Common Model

| Model | Target selection | Completion the caller observes |
|---|---|---|
| node direct send | The caller directly specifies one RID belonging to the same `MeshName`. | Completes with no return data once the source-local queue accepts the message. |
| [node direct](01-glossary.en.md#node-direct) request | The caller directly specifies one RID belonging to the same `MeshName`. | Completes with one of reply, timeout, or route error. |
| channel send | The framework selects one ready target from the RouteMesh or ClientServer send paths registered under `ChannelName`. | Completes with no return data once the selected send path's source-local queue accepts it. |
| channel request | The framework selects one [ready target](01-glossary.en.md#ready-target) from the [RouteMesh](01-glossary.en.md#routemesh) or ClientServer send paths registered under `ChannelName`. | Completes with one of reply, timeout, or route error. |
| [Logical Multicast](01-glossary.en.md#logical-multicast) | The framework selects matching targets among `ChannelName`'s remote members and local Spots. | Completes with no return data once it secures a bounded worker and source-local capacity and starts the publish transaction. Doesn't wait for per-target submission or handler completion. |
| Spot message | The caller specifies a global Spot ID and the framework finds the [owner](01-glossary.en.md#owner) of the current Ready [authority](01-glossary.en.md#authority). | Send completes with no return data after source-local queue acceptance; request completes with the reply result. |
| Actor message | The caller specifies a global Actor ID and the framework finds the current [Ready](01-glossary.en.md#ready) authority's owner. | Send completes with no return data after source-local queue acceptance; request completes with the reply result. |
| Object create/get-or-create | The caller specifies a global ID and stable type, adding placement intent if needed. | Returns an exact `ActorRef`/`SpotRef` or a typed creation error. |
| classic fanout | The framework uses the ready subscriber set as the target. | Completes with no return data once the local publisher queue accepts it. |
| STREAM | The caller uses the connection identified by session RID. | A one-way packet completes with no return data after local queue acceptance; a request returns a reply. |

The method by which the framework picks one matching target in a Channel operation
is called `select-one`.

### 2.1 The Public Interface That Starts An Interaction

The following table shows where the application starts each interaction. `client`
is obtained via DI or the current handler context — the application doesn't
directly select a transport socket or endpoint.

| Interaction | Starting interface | Target the caller specifies |
|---|---|---|
| Node direct/Channel select-one | `IZLinkRouteClient` | Node direct: MeshName and target RID; Channel: ChannelName |
| Spot send/request | `IZLinkSpotClient` | Global Spot ID |
| Actor send/request | `IZLinkActorClient` | Global Actor ID |
| User Spot create/lookup | `IZLinkSpotManager` | Stable Spot type and, if needed, global Spot ID |
| Actor create/lookup | `IZLinkActorManager` | Global Actor ID and stable Actor type |
| Logical Multicast | `IZLinkSpotPublisherClient` | ChannelName and topic |
| Classic fanout | `IZLinkFanoutClient` | Fanout ChannelName and optional topic |
| STREAM send/reply | `IZLinkSessionClient` | The current STREAM session |

The code below is an explanatory declaration, abbreviated in .NET notation, to show
the shape of a common interaction. The exact per-language signature is owned by
[.NET Channel Messaging](server/languages/dotnet/interfaces/04-channel-messaging.ko.md),
[.NET Spot](server/languages/dotnet/interfaces/05-spots.en.md),
[.NET Actor](server/languages/dotnet/interfaces/06-actors.ko.md), and
[.NET STREAM Session](server/languages/dotnet/interfaces/07-stream-session.ko.md).

```csharp
public interface IZLinkRouteClient
{
    // a direct operation where the caller specifies both Mesh and target node.
    IZLinkSendCall SendToNode<T>(string meshName, RoutingId targetNodeRid, T message);
    IZLinkRequestCall RequestToNode<T>(string meshName, RoutingId targetNodeRid, T request);

    // a select-one operation where the framework picks one ready target for ChannelName.
    IZLinkSendCall SendToChannel<T>(string channelName, T message);
    IZLinkRequestCall RequestToChannel<T>(string channelName, T request);
}

public interface IZLinkSpotClient
{
    // the framework finds the global Spot ID's current owner and sends.
    IZLinkSpotSendCall SendToSpot<T>(string spotId, T message);
    IZLinkSpotRequestCall RequestToSpot<T>(string spotId, T request);
}

public interface IZLinkActorClient
{
    // the framework finds the global Actor ID's current owner and sends.
    IZLinkActorSendCall SendToActor<T>(string actorId, T message);
    IZLinkActorRequestCall RequestToActor<T>(string actorId, T request);
}

public interface IZLinkSpotPublisherClient
{
    // publishes together to matching remote MeshNode and local Spot subscriptions.
    IZLinkPublishCall Publish<T>(string channelName, string topic, T message);
}

public interface IZLinkSpotManager
{
    // Create issues a new RID; GetOrCreate uses the RID the application specifies.
    IZLinkSpotCreateCall Create(string spotType);
    IZLinkSpotGetOrCreateCall GetOrCreate(string spotId, string spotType);
}

public interface IZLinkActorManager
{
    // the Actor create family always takes a global Actor ID and stable Actor type together.
    IZLinkActorCreateCall Create(string actorId, string actorType);
    IZLinkActorGetOrCreateCall GetOrCreate(string actorId, string actorType);
}

public interface IZLinkFanoutClient
{
    // builds a call to submit an event to the classic-fanout-only publisher transport.
    IZLinkFanoutPublishCall Publish<T>(string channelName, string topic, T message);
}

public interface IZLinkSessionClient
{
    // Send is a server-initiated packet; Reply is the current STREAM request's reply.
    IZLinkSessionSendCall Send<T>(T message);
    IZLinkSessionReplyCall Reply<T>(T message);
}
```

A `Send...` call waits up to local outbound admission via `Async()` and completes
with no return value. A `Request...` call waits for a reply via `Async<TReply>()`.
Even in a language declaring `Yield<TReply>()`, this operation can only be used on
the shared turn of a `SpotWide` User Spot or Instance Spot.

## 3. Node Direct And Channel Select-One

Node direct is used for infrastructure and explicit owner routing. If the target
RID isn't a current Mesh member, it's `NotFound`; if it's a member but the pipe
isn't ready, it waits up to the send-readiness limit and then ends with
`Unavailable`. A Node direct operation doesn't automatically resend a failed
request to a different node. A global Spot/Actor message only uses a cached Ready
route and a committed Message Follow route. If it can't relay to the current owner
within the Message Follow limit, it ends with `Unavailable` — the source doesn't
read the Store and resubmit the same operation to a different owner.

A Channel operation first decides the process-local send path by ChannelName. A
RouteMesh path picks one, with weight greater than 0, from the ready members at the
moment of the call; a ClientServer path picks one from ready servers. No
application callback sits between selection and submit.
[Weight](01-glossary.en.md#weight) 0 excludes it from new channel selection, and on
RouteMesh also excludes it from Logical Multicast remote targets. It doesn't affect
RID direct or an already-submitted operation.

If select-one's non-blocking submit isn't accepted due to insufficient capacity,
that first selection isn't a public target commitment. The framework service
runtime can pick a different target among the current eligible members of the same
[ChannelName](01-glossary.en.md#channelname), up to a successful admission after
send-ready. The target is confirmed at the moment the transport queue accepts the
operation, and afterward the same operation isn't replayed to a different member.
A direct call doesn't use this re-selection rule. Node direct keeps RID; Spot/Actor
keeps global ID; session keeps a binding token — physical peer lifecycle generation
isn't exposed as public target identity.

Since the same ChannelName can't be registered under multiple physical send paths,
the caller doesn't specify MeshName or ClientServer kind. Registering ChannelName
under different topologies in the same process fails host startup as a
configuration error.

Node direct keeps using [MeshName](01-glossary.en.md#meshname)/RID. A Logical
Multicast caller only specifies ChannelName and topic — the process-local channel
index decides the owner RouteMesh's MeshNode. The selected owner MeshName is only
observed in internal routing and runtime monitoring.

## 4. Send And Request

`send` is a one-way operation with no reply. The public call only provides a
single async submit — it doesn't provide a synchronous terminator that tries once
immediately. The return isn't confirmation that the destination handler ran — it
indicates whether the framework accepted the message onto the local outbound
queue. If the queue is temporarily full, it waits for admission up to a finite
send timeout. A one-way error occurring after acceptance is reported via the
runtime error sink and monitoring.

Global Spot/Actor send also uses the same async terminator. The source resolves
the current Ready authority and completes the submit via local outbound admission.
A cache hit also keeps the same public meaning, so it neither provides a
synchronous submit depending on cache state, nor requires the caller to supply an
owner node and generation. A message call doesn't create a Missing object's
creation intent by default. Only when Instance intent is specified on a
Spot-specific fluent call is a new Spot created and prepared to process the first
message, when no Instance Spot is running. This process is called
`cold activation`. The starting method still only takes a global
[Spot ID](01-glossary.en.md#spot-id) — an optional
[stable type](01-glossary.en.md#stable-type) and initial Mesh are
[cold activation](01-glossary.en.md#cold-activation) options on the fluent call.

A valid one-way call completes with no return value once source-local admission
succeeds. If capacity isn't secured by the send timeout, `DeadlineExceeded`; a
missing target/route and runtime shutdown complete with an operation-specific
exception. Invalid argument/handle/state and a duplicate submit are also local
exceptional completions. Cancellation is expressed as that language's cancelled
awaitable. The framework never automatically resubmits an operation after any
terminal completion.

A `request` builds reply correlation on the selected send path and delivers the
terminal result exactly once. Request timeout is the time waiting for a reply.
Send-stage backpressure is handled by send timeout. The framework doesn't
automatically resend a request that ended in route error or timeout. Each
language's transport error is converted into one of this document's closed
framework results — a transport-specific result isn't exposed on the public call.

The exact meaning of the common kinds `Send` and `Request` return, and of timeout
and cancellation, is defined by the [Framework Error Model](32-framework-error-model.en.md).

A request sent to a different RouteMesh or ClientServer Channel also follows the
same single terminal-completion rule. If started from a Spot, the framework
preserves the original Spot activation and generation in the completion record,
and doesn't re-dispatch the reply as a new application message.

Messages successfully submitted by the same origin to the same destination pipe
are FIFO. A global order across different destinations, origins, or sessions isn't
guaranteed.

## 5. Spot Logical Multicast

A Logical Multicast publish takes the target ChannelName,
[topic](01-glossary.en.md#topic), and typed payload. At publish time it snapshots
the remote [MeshNode](01-glossary.en.md#meshnode) and local Spot matches.

- Submits a routed message once per remote MeshNode.
- The receiving MeshNode checks its local subscription for
  `(ChannelName, topic filter)`.
- Matching Spot queues on the same node share a reference to immutable payload
  storage.
- Doesn't relay to a different MeshNode or replay a past event.

The framework service runtime submits the publish transaction to a bounded I/O
executor. If a worker slot isn't secured by the send timeout, the transaction
doesn't start and it fails with `DeadlineExceeded`. Once handoff succeeds and the
transaction starts, the public terminal completes normally with no return data,
and the runtime keeps submitting to each remote target and local Spot queue
internally.
Since the transaction start is the commit point of the
[snapshot](01-glossary.en.md#snapshot) operation, cancellation or shutdown doesn't
stop processing of remaining targets.
An earlier-accepted remote target or local Spot queue isn't canceled because a
later target failed.

It completes normally even if every snapshot target is 0. Remote unreachability,
insufficient outbound capacity, and local Spot queue drops occurring after the
transaction starts don't roll back already-accepted targets or retry the whole
publish. Per-target accept/failure results aren't returned as a public result or
aggregated into publish-only monitoring values.

Publish's normal completion means the transaction started. It doesn't guarantee
submission to the fixed snapshot's targets, Spot handler execution, subscriber
receipt, or local Spot queue acceptance on the receiving MeshNode after the remote
ROUTER accepts it.

## 6. Classic Fanout

[Classic fanout](01-glossary.en.md#classic-fanout) is a publisher/subscriber
channel independent of MeshNode. It only delivers a new event to a subscriber
whose current connection and [subscription](01-glossary.en.md#subscription) are
ready. The publisher doesn't store an event before connection or during a
disconnection, and doesn't replay it after reconnecting.

The publisher call only provides a single async terminator that waits for local
admission up to the publisher socket's send timeout. Even with 0 subscribers, it
completes normally with no return value once the local publisher queue accepts the
event. This completion doesn't mean subscriber receipt or handler completion.

Publish's common input is ChannelName, topic, and typed event. A convenience call
using the typed event's packet name as the topic also builds the same operation.
Both calls use the same publisher transport, timeout, and async completion rules;
subscriber dispatch selects the handler by
[packet name](01-glossary.en.md#packet-name) and preserves topic in the handler
context.

The publisher publishes ChannelName and the actual endpoint to a dedicated
location descriptor. An automatic subscriber connects to every live publisher for
the same ChannelName and doesn't connect to a different ChannelName or a
different [descriptor](01-glossary.en.md#descriptor) kind. A manual subscriber
only connects to the specified endpoint.

Logical Multicast and classic fanout both offer a publish/subscribe usage
experience, but since their delivery targets and guarantees differ, they're
registered as separate features.

## 7. Spot And Actor

A Spot is a logical mailbox owned by a MeshNode. An Entry Spot's and Instance
Spot's direct message, Logical Multicast, timer, and lifecycle callback are all
processed serially on each Spot's execution gate. A User Spot's default
`SpotWide` mode has the whole Spot/member Actor/timer/lifecycle callback use a
common gate; the optional `PerActor` mode uses per-Actor, per-Spot-lane, and
per-timer gates. A Node callback doesn't read the Spot queue on its behalf.

[Instance Spot](01-glossary.en.md#entry-user-instance-spot) is a Spot kind with no
Actor membership. Missing Instance creation is only started by explicit
[Instance intent](01-glossary.en.md#instance-intent) on a
[Spot direct](01-glossary.en.md#spot-direct) fluent call. The single owner a
[Location Store](01-glossary.en.md#location-store) reservation decided runs the
factory, confirms the durable activation inbox first record, then commits a
location `Ready` including recovery root/cursor. The framework restores the first
record to the local queue head, then opens the activation barrier. A Creating
competitor joins the same attempt's terminal result and doesn't start a separate
[factory](01-glossary.en.md#factory) or message.

`ActorRef` and `SpotRef` are immutable location snapshots holding the global ID,
ObjectGeneration, and the MeshName/NodeRid at lookup time. They don't include
endpoint, internal frames, or runtime resources. A bound session's `Ref`/`ref()`
accessor returns a new immutable snapshot, holding the same ActorId/
ObjectGeneration and the target MeshName/NodeRid, once an Actor relocation's route
switch completes. A previously returned ref value doesn't change. A regular
message uses the global ID, not a ref, and the framework resolves current
authority.

An Actor message resolves the global Actor ID's current authority, then adds
directly to the Actor mailbox. Actor payload doesn't go through the Spot message
queue. An Entry Spot Actor and a `PerActor` User Spot's Actor use a per-Actor
gate; a `SpotWide` User Spot's member Actor uses the User Spot's common gate. To
read or change Spot-owned state, an explicit Spot send/request is submitted and
processed on that Spot's turn.

Node, Spot, and Actor completion and send-ready are processed in an
infrastructure execution area that can proceed even while an application handler
is waiting.

## 8. STREAM Session

A STREAM session owns connection lifecycle and packet order. The framework's
internal recv loop puts a packet on a managed queue and then runs the session
callback. The same session's packets and lifecycle callbacks run serially — a
global order across different sessions isn't guaranteed.

Once a Session and Actor are bound, session ingress submits complete messages to
the Actor mailbox. A message an Actor sends to the client uses the current
binding's session FIFO. During an Actor move, a session barrier distinguishes
old-epoch from new-epoch order.

The server package's bound session send, session Actor relay, and explicit STREAM
send/reply also return the same async-only one-way admission result. A separate
stream connector package's send builder follows the connector package's contract.
A STREAM reply uses that STREAM socket's send timeout and doesn't use the
caller's request timeout as the reply admission deadline. If the reply sequence
or one-shot token is invalid, or the same reply call is submitted twice, it ends
as a local exceptional completion. The first valid reply terminator atomically
consumes the token before transport admission. Even if this terminator completes
via backpressure, timeout, or cancellation, the token isn't reused. If two calls
built from the same token race, only one starts transport admission.

## 9. Representative Public Call Examples

The following code compares how the interactions from the previous sections
specify a target and which terminal method they end with. `routes`, `spots`,
`actors`, manager, and publisher are public clients obtained via DI, and RID and
ID are assumed already held by the application. The business message types are
illustrative examples.

### 9.1 Node Direct And Channel Select-One

```csharp
// Node direct: the application specifies the exact node RID in the "world" Mesh.
await routes
    .SendToNode("world", targetNodeRid, new ReloadConfig())
    .Async(cancellationToken);

// Channel select-one: the framework picks one ready server in the "game" Channel.
MatchFound match = await routes
    .RequestToChannel("game", new FindMatch(playerId))
    .Timeout(TimeSpan.FromSeconds(2))
    .Async<MatchFound>(cancellationToken);
```

The first call doesn't pick a different node if the specified RID fails. The
second call can pick a different eligible target under the same ChannelName only
until one target accepts the operation.

### 9.2 Spot/Actor Messages And Creation

```csharp
// Existing Spot: the framework finds the Spot ID's current Ready owner and sends the request.
RoomState room = await spots
    .RequestToSpot(roomId, new GetRoomState())
    .Timeout(TimeSpan.FromSeconds(1))
    .Async<RoomState>(cancellationToken);

// Missing Instance Spot: only prepares the Spot when explicit intent is given, and processes the same first request.
ShardState shard = await spots
    .RequestToSpot(shardRid, new LoadShard())
    .InstanceSpot("world-shard")
    .InMesh("world")
    .Async<ShardState>(cancellationToken);

// A User Spot is either explicitly created via a manager call or joins an existing creation attempt.
ZLinkSpotCreateResult createdSpot = await spotManager
    .GetOrCreate(roomId, "room")
    .InMesh("world")
    .Async(cancellationToken);

// An Actor message goes directly into the Actor queue without going through the member Spot's message queue.
PlayerState player = await actors
    .RequestToActor("player-42", new GetPlayerState())
    .Async<PlayerState>(cancellationToken);

// Actor creation specifies the global Actor ID and stable Actor type together.
// The result distinguishes an existing-Actor lookup, new-creation approval, and application decline.
ZLinkActorCreateResult actorCreation = await actorManager
    .GetOrCreate("player-42", "player")
    .InMesh("world")
    .Async(cancellationToken);
```

A regular Spot/Actor message doesn't take a target node or endpoint. Even though
`SpotRef` and `ActorRef` have a NodeRid, it isn't used as a regular message's
address — the framework re-confirms the global ID's current authority.

### 9.3 Logical Multicast And Classic Fanout

```csharp
// Logical Multicast: publishes together to matching remote nodes and local Spot subscriptions.
await spotPublisher
    .Publish("world-events", "zone.7", new WeatherChanged("rain"))
    .Async(cancellationToken);

// Per-target submission results aren't returned or aggregated into publish-only monitoring.

// Classic fanout: targets subscribers currently connected on an independent publisher transport.
await fanout
    .Publish("telemetry", "server.health", new HealthSample(cpu, memory))
    .Async(cancellationToken);
```

Neither publish waits for handler completion. Per-target submission results
aren't returned as a public result or aggregated into publish-only monitoring.

### 9.4 STREAM Session

```csharp
// submits a server-initiated one-way packet to the current session FIFO.
await sessionClient
    .Send(new ServerNotice("maintenance"))
    .Async(cancellationToken);

// only consumes the current request's reply capability, exactly once, from within a STREAM request handler.
await sessionClient
    .Reply(new LoginAccepted(playerId))
    .Async(cancellationToken);

// bound Actor relay submits to the Actor mailbox using the current session binding.
await sessionActor
    .RelayAsync(
        ZLinkMessage.From(new ClientInput(sequence, command)),
        cancellationToken);
```

`Reply(...)` isn't an API for sending an arbitrary server-initiated message. It
consumes the reply capability of the STREAM request the current handler received.
A regular server-initiated packet uses `Send(...)`.

## 10. Handler Failure

A request whose reply route can be restored completes with a structured error
reply. A message whose reply route can't be restored, and a one-way message, are
dropped, leaving a log, metric, and observer event matching the cause. An
application handler exception is also recorded as an error on the one-way path.
Observer failure doesn't change the original reply or drop result.

## 11. Termination

Once `Relocate` publishes a `Relocating` intent or `Shutdown` starts an admission
seal, new channel selection, Logical Multicast targets, and new state assignment
are restricted. A relocation unit that hasn't obtained a permit under
`Relocating` keeps processing existing messages and timers, and only seals once
it obtains a permit at a queue-turn boundary. After `Draining`, only
already-admitted messages, request completion, Actor relocation, and
[STREAM session](01-glossary.en.md#stream-session) barriers proceed, up to the
configured [deadline](01-glossary.en.md#deadline). Remaining operations after the
deadline complete with a per-owner terminal [shutdown](01-glossary.en.md#shutdown)
result.

A draining MeshNode is excluded from new Instance placement candidates.
`Shutdown` doesn't move an existing Instance Spot to a different node — it
processes accepted turns up to the deadline and then cleans up. `Relocate` only
materializes on the target the existing owner allowed by the per-type maintenance
policy and authority transaction. Both operations verify the framework admission
seal and current location authority, preventing a stale owner from applying
`Closing` or release.
