---
title: "Channel Messaging"
---

# Channel Messaging

[Spec table of contents](README.en.md) · [Previous: RouteMesh Topology](07-channel-topology.ko.md) · [Next: ClientServer Channel](09-client-server-channel.ko.md)

> **What this chapter defines** — the common contract for Node direct, which sends
> to a specific MeshNode, and Channel messaging, which selects one Server by
> ChannelName.


## 1. Scope

This document explains the common contract for Node direct, which sends to a
specific MeshNode, and Channel messaging, which selects one Server by ChannelName,
in ZLink Framework.

The two methods differ in the target the application specifies and the scope the
framework selects from.

| Method | Value the application specifies | How the framework decides the actual target |
|---|---|---|
| [Node direct](01-glossary.en.md#node-direct) | Specifies `MeshName` and the target node's RID. | Only uses a ready [MeshNode](01-glossary.en.md#meshnode) on the RouteMesh of the same MeshName that exactly matches the specified RID and isn't Object Client. Doesn't switch to a different RID. |
| [ChannelName](01-glossary.en.md#channelname) select-one | Specifies one `ChannelName`. | Finds the send path registered for ChannelName in the current process. On a [RouteMesh](01-glossary.en.md#routemesh) path, selects one [ready](01-glossary.en.md#ready) Server membership for that ChannelName; on a ClientServer path, selects one ready server. |

ChannelName isn't a socket or endpoint name. It's a logical address for finding
one RouteMesh or ClientServer send path registered in the current process.

Physical connection and [membership](01-glossary.en.md#membership) are defined by
[Channel Topology](07-channel-topology.ko.md); payload and metadata by
[Message Model](04-message-model.en.md); completion and execution order by
[Async Execution Policy](05-async-execution-policy.en.md).

## 2. An Example Expressing The Common Behavior As A .NET API

The C# code below is a reference showing how the common contract appears in the
.NET public API. It doesn't require the same signature in other languages.

The exact .NET signature is defined by
[.NET Channel Messaging Public Interface](server/languages/dotnet/interfaces/04-channel-messaging.ko.md).

```csharp
public interface IZLinkRouteClient
{
    // sends a message on the specified MeshName without changing the target RID.
    IZLinkSendCall SendToNode<TMessage>(
        string meshName,
        RoutingId targetNodeRid,
        TMessage message);

    IZLinkRequestCall RequestToNode<TRequest>(
        string meshName,
        RoutingId targetNodeRid,
        TRequest request);

    // selects one ready target from the send path registered under ChannelName.
    IZLinkSendCall SendToChannel<TMessage>(
        string channelName,
        TMessage message);

    IZLinkRequestCall RequestToChannel<TRequest>(
        string channelName,
        TRequest request);
}
```

How Node direct and ChannelName use different handler interfaces is explained in
[5. How To Find And Run A Handler](#5-how-to-find-and-run-a-handler).

The following code is an example using both targeting methods from the same
client.

```csharp
await routeClient
    .SendToNode(
        "game-mesh",        // specifies the physical RouteMesh the RID belongs to.
        targetNodeRid,      // the framework doesn't switch to a different RID.
        nodeCommand)
    .Async(cancellationToken);

MatchReply reply = await routeClient
    .RequestToChannel(
        "match",            // specifies only the logical ChannelName, not an endpoint.
        request)
    .Async<MatchReply>(cancellationToken); // waits for the selected Server handler's reply.
```

## 3. How A Target Is Selected

### 3.1 Node Direct

Node direct keeps the caller-specified `MeshName` and target RID as-is. It only
sends a message to a MeshNode satisfying all of the following conditions.

- Participates in the RouteMesh of the same [MeshName](01-glossary.en.md#meshname).
- Matches the specified target RID.
- Object role isn't `Client`.
- Is in a ready state, able to receive the message.

If the target RID isn't a member, or doesn't become ready by the time limit, it
ends with a target error or timeout. The framework doesn't automatically switch to
a different RID on the same MeshName.

Object Client can't register an application Node direct handler and isn't a Node
direct target. This restriction doesn't apply to a RouteMesh Channel Server
registered on the same MeshNode. Automatic discovery only skips creating a
connection intent for a pair where both sides are Object Client and neither has
RouteMesh Channel Server membership. A manual endpoint also checks the same
condition at handshake, closing the connection before ready and not reconnecting
under the same configuration generation. If a caller specifies an Object Client
RID as a Node direct target, it isn't switched to a different target — it ends
with `NotFound`.

### 3.2 ChannelName Select-One

[ChannelName select-one](01-glossary.en.md#select-one) is a method of selecting
one Server, among several, to receive the current call. The framework processes
the following order as one operation.

A target preparing to terminate or move to a different host is excluded from new
call selection first, but can finish already-accepted work within a set time.
This state — blocking new work and cleaning up existing work — is called
draining.

1. Finds one send path registered for ChannelName in the current process.
2. On a RouteMesh path, only Server membership for the same ChannelName is used
   as candidates.
3. On a ClientServer path, only ready servers for that ChannelName are used as
   candidates.
4. Excludes targets with weight 0 and [draining targets](01-glossary.en.md#drain).
5. Picks one ready target with weight greater than 0, reflecting the
   [weight](01-glossary.en.md#weight) ratio, and immediately submits the message.

The framework computes the sum of remaining positive weights, after applying
eligibility and drain conditions, using at least a 64-bit integer. It selects a
target by the relative ratio computed so this sum doesn't overflow.
For example, if two otherwise-equal candidates have weight `100` and `300`, the
long-run selection ratio is about `1:3`.

### Selection Order

The framework keeps one accumulator per candidate and picks a target via the
following procedure. The accumulator starts at `0`.

1. Adds each candidate's own weight to its accumulator.
2. Picks the candidate with the largest accumulator. On a tie, picks the one
   earlier in **ascending candidate identifier order**. The identifier differs by
   topology — a RouteMesh path uses [NodeRid](01-glossary.en.md#meshnode); a
   ClientServer path uses Server RID. Comparison compares the identifier's byte
   sequence as unsigned values from the front, and if one is a prefix of the
   other, the shorter one comes first. Using a different value pointing at the
   same target — like connection path or registration source — as the identifier
   causes the order to diverge between implementations.
3. Subtracts the sum of every candidate's weight from the picked candidate's
   accumulator.

The accumulator is kept by that ChannelName's send path. If the candidate list
changes, only the accumulators of candidates in the new list are kept — the rest
are discarded.

This procedure keeps the long-run ratio while **preventing consecutive
selections from piling onto one candidate.** With two candidates A and B at
weight `100` and `300`, four consecutive calls give `B, A, B, B` (assuming A's
identifier comes first) — not a pile-up like `A, B, B, B`. Candidates with equal
weight are selected alternately, so this also satisfies
[ClientServer Channel](09-client-server-channel.ko.md)'s rotation requirement.

The same candidate list and accumulator state always produce the same order. The
application can rely on this reproducibility.

A ClientServer's local Server is a candidate on equal footing with a remote
Server. A local Server only enters the candidate set once its listener bind and
ClientServer service admission finish (making it ready), its weight is greater
than 0, and it isn't draining. The framework doesn't pick the local Server first,
or exclude it from candidates, just because it's in the same process.

Even if the local Server is selected, its handler isn't called directly. The
actual ClientServer record is sent from the Client `DEALER` to the Server
`ROUTER`. A local-transport bypass path that skips codec, admission, HWM,
timeout, correlation, and reply handling isn't provided.

This local-candidate rule only applies to a ClientServer path. On a RouteMesh
path, the sending MeshNode itself isn't a candidate even if it registered the
Server role for the same ChannelName. This is because a RouteMesh candidate is
the Server membership [10 Channel Topology](07-channel-topology.ko.md) §4.2
publishes to the descriptor, and that set only represents membership that can be
a remote target — a MeshNode doesn't form a peer connection with itself.
Selection sends to the chosen target over the existing RouteMesh peer connection
per the same document's §4.2.1, and Channel registration doesn't create a new
socket. So calling RouteMesh select-one on a MeshNode where only itself is that
ChannelName's Server has no candidates, and it fails with no target. To handle
it in the same process, use a ClientServer path.

The two paths also handle the "no candidate yet" case differently. RouteMesh
fails immediately with no target, as above. ClientServer waits a bounded time at
the call moment if there's no ready candidate, then fails. The wait limit is the
shorter of that call's request timeout and 5 seconds; if no ready candidate
appears within that time, it fails with no target. Framework startup doesn't
wait for local ClientServer admission to complete.

The two paths are handled differently because the meaning of "no candidate"
differs. On RouteMesh, no candidate means no peer has published Server
membership for that ChannelName, and waiting doesn't make one appear. On
ClientServer, the local Server already exists in the same process's
configuration — only its admission hasn't finished yet. Failing immediately in
this window would give the application a no-target result on a call right after
startup, even though the configuration is correct. This wait doesn't trigger
admission — it only waits for admission already in progress to finish, and
applies equally to remote ClientServer candidates regardless of local status.

```mermaid
sequenceDiagram
    participant Caller
    participant Index as Process Channel index
    participant Selector as Target selector
    participant Transport as Selected send route
    participant Target as Selected server

    Caller->>Index: submit ChannelName and message
    Index->>Index: check the registered send path
    alt RouteMesh path
        Index->>Selector: pass the ready Server candidates for the same ChannelName
    else ClientServer path
        Index->>Selector: pass the ready server candidates for the same ChannelName
    end
    Selector->>Selector: exclude weight 0 and draining targets
    Selector->>Transport: submit the message to the selected target
    Transport-->>Index: source-local queue acceptance complete
    Index-->>Caller: complete normally with no return data
    Transport->>Target: deliver the message
```

The diagram above shows target selection and async submit completion for a
one-way send. Normal completion means the selected send path's source-local
queue accepted the message. The framework doesn't return acceptance status, the
selected RID, or server identity as an application result.

### 3.3 An Unregistered ChannelName

The same process can have MeshNodes and ClientServer clients for several
MeshNames. But if the called ChannelName isn't registered, a different MeshNode
or ClientServer client isn't searched and used instead.

This restriction only applies to the current ChannelName call's automatic
fallback. The application can start a separate call under a different registered
ChannelName, or start a new Node direct call specifying MeshName and target RID.
The framework doesn't switch the failed original call's target or send path to
this new call.

Registering the same ChannelName under two or more physical send paths also
isn't allowed. In this case host startup fails.

## 4. Why It Doesn't Automatically Resend After Selection

After the framework selects a target and submits the request, a connection
closure or timeout can occur. In this case the same request isn't automatically
resent to a different Server member.

This is because the first target may have already run the request, with only
the reply not delivered. Resending to a different target could run the same
work twice.

The application can explicitly start a new request after receiving a failure
result. The new request is a separate operation, not an automatic resend of the
previous request. The application must handle duplicate execution, considering
that the previous target may have already run the work.

The acceptance moment of a one-way send, and a request's final completion
condition, are defined by
[Async Execution Policy](05-async-execution-policy.en.md).

## 5. How To Find And Run A Handler

Node direct and ChannelName handlers register in different
[handler namespaces](01-glossary.en.md#handler-namespace).

| Handler kind | Value distinguishing the handler | Queue it runs on |
|---|---|---|
| Node direct | Uses MeshName, message kind, and packet name together. | Runs on the Node application queue of the target MeshNode, which isn't Object Client. |
| ChannelName | Uses ChannelName, message kind, and packet name together. | Runs on the Channel application queue of the selected RouteMesh Server or ClientServer server. |

Registering a handler with the same [message kind](01-glossary.en.md#message-kind)
and [packet name](01-glossary.en.md#packet-name) twice in the same handler scope
fails startup. The same packet name can be used across different ChannelNames or
the Node direct scope.

The following .NET interface excerpt shows the two handler scopes are also
separated in the public API. `IZLinkRouteRequestHandler` handles a Node direct
request, and `IZLinkRequestHandler` handles a ChannelName request.

```csharp
public interface IZLinkRouteRequestHandler<in TRequest, TReply>
{
    ValueTask<TReply> HandleAsync(
        TRequest request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken);
}

public interface IZLinkRequestHandler<in TRequest, TResponse>
{
    ValueTask<TResponse> HandleAsync(
        TRequest request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken);
}
```

A handler is found by message kind and packet name. In the example below,
`AddRouteRequestHandler` registers in the Node direct handler scope, and
`AddRequestHandler` registers in the `"billing"` ChannelName's request handler
scope.

```csharp
var mesh = options.AddRouteMesh("service-mesh");

mesh.AddRouteRequestHandler<NodeBillingHandler, BillingRequest, BillingReply>(
    packetName: "billing.charge"); // registers the same packet name in the Node direct scope.

mesh.Channel("billing")
    .Server()
    .AddRequestHandler<BillingHandler, BillingRequest, BillingReply>(
        packetName: "billing.charge"); // registers the same name in the billing Channel scope.
```

For example, sending a `billing.charge` request via the `"billing"` ChannelName
runs `BillingHandler`. Sending the same packet name via Node direct runs
`NodeBillingHandler`, found in the separate Node direct scope.

The code above is a .NET expression showing the contract for readability. The
exact full signature is defined by
[.NET Channel Messaging Interface](server/languages/dotnet/interfaces/04-channel-messaging.ko.md) and
[.NET Configuration Interface](server/languages/dotnet/interfaces/03-configuration-topology.en.md).

### 5.1 Information Provided To The Handler Context

The Channel handler and filter context provide the following information.

- ChannelName
- Message kind and packet name
- Metadata
- Request correlation

Since ChannelName already uniquely determines the send path, a Channel handler
context doesn't require MeshName.

The Node direct handler context also provides the following physical route
information.

- MeshName
- Source node RID

Physical information the handler doesn't need for business processing — like
RouteMesh or ClientServer kind, endpoint, and selection result — is left in
monitoring.

### 5.2 Spot And Actor Payload Use Separate Target APIs

Node direct, Spot direct, and Actor direct are different addressing methods, and
their handlers aren't mixed either.

A message sent via Node direct is processed by the specified MeshNode's Node
direct handler. The framework doesn't turn this message into a Spot or Actor
message based on the payload's type or content. To send a message to a
[Spot](01-glossary.en.md#spot) or Actor, the application must use the dedicated
API that specifies a global Spot ID or ActorId from the start.

The framework preserves the request's reply route and correlation. An
application handler doesn't directly build a source endpoint or internal route
frame.

## 6. The Boundary With Classic Fanout

Classic fanout is a feature delivering events to subscribers, over a separate
PUB/SUB socket, whose connection and subscription are both ready. It doesn't
share a target set with RouteMesh ChannelName select-one or Spot Logical
Multicast.

[Classic fanout](01-glossary.en.md#classic-fanout) doesn't provide the following
functionality.

- Durable storage of a message
- Subscriber processing acknowledgement
- Replay, resending a message later
- Lossless delivery

Classic fanout is loss-tolerant delivery. If a subscriber's receipt is slow and
the publisher's send queue reaches HWM, the message to that subscriber is
dropped and publish ends successfully. Delivery to the remaining subscribers
isn't affected. The publisher isn't stalled by one slow subscriber.

Delivery that can't tolerate loss is handled by RouteMesh, not Classic fanout.
A Spot's [Logical Multicast](01-glossary.en.md#logical-multicast) doesn't use a
PUB/SUB socket — it delivers to each participating node over the MeshNode
connection, so it isn't subject to this loss rule.

### 6.1 The Framework's Connection-Liveness Topic Can't Be Used

The framework checks whether a subscriber keeps receiving the signal a publisher
sends, within a fixed time, on a Classic fanout connection. Checking whether a
connected peer's signal keeps arriving is called liveness checking.

The publisher periodically sends an internal signal so connection status can be
checked even with no application event. This signal is called a liveness beacon
and uses the five bytes `01 5A 4C 46 31` as its topic.

The application can't use exactly this same value as a
[topic](01-glossary.en.md#topic) in the public publish API. This restriction
distinguishes the framework's internal signal from application events.
Specifying this value causes a call-argument error.

A topic that differs in length or by even one byte, as shown below, can be
used.

```text
01 5A 4C 46 31       not usable: exactly matches the internal signal's topic.
01 5A 4C 46          usable: different length.
01 5A 4C 46 31 00    usable: one extra byte.
01 5A 4C 46 32       usable: last byte differs.
```

A subscriber doesn't treat this topic's signal as an application event. Since
the framework only uses it to check connection status, a registered fanout
handler isn't run, and it isn't published to message-flow observation, which
records application message delivery flow either. The byte format of this
[liveness beacon](01-glossary.en.md#liveness-beacon) and the time criterion for
judging a connection lost are defined by
[Transport Liveness](29-transport-liveness.ko.md).

A Spot's Channel-scoped [Logical Multicast](01-glossary.en.md#logical-multicast)
is defined by [20 Spot Messaging](12-spot-messaging.en.md).

### 6.2 Classic Fanout's Interface And Usage Example

Classic fanout doesn't select one Server, like a ChannelName request does. It
delivers an event to subscribers connected to the publisher whose subscription
to that topic is ready. Each subscriber's registered typed fanout handler
processes the event.

In the following .NET interface excerpt, `Publish` returns a dedicated
`IZLinkFanoutPublishCall`. This call's `Async` waits for the local publisher
transport to accept the event — it doesn't wait for subscriber handler
completion. Normal completion has no public return value.

```csharp
public interface IZLinkFanoutClient
{
    IZLinkFanoutPublishCall Publish<TEvent>(
        string channelName,
        TEvent message);

    IZLinkFanoutPublishCall Publish<TEvent>(
        string channelName,
        string topic,
        TEvent message);
}

public interface IZLinkFanoutPublishCall
{
    ValueTask Async(
        CancellationToken cancellationToken = default);
}

public interface IZLinkFanoutHandler<in TEvent>
{
    ValueTask HandleAsync(
        TEvent message,
        CancellationToken cancellationToken);
}
```

The following example shows building a `"system-events"` fanout channel and
processing a `SystemNotice` the publisher sent, in the subscriber's
`SystemNoticeHandler`.

```csharp
var fanout = options.AddFanoutChannel("system-events");

fanout.EnablePublisher(); // opens a PUB listener for this process to publish events.

fanout
    .EnableSubscriber()
    .AddHandler<SystemNoticeHandler, SystemNotice>(
        packetName: "system.notice"); // registers the typed handler to process received events.

await fanoutClient
    .Publish("system-events", new SystemNotice("maintenance starting"))
    .Async(cancellationToken); // waits for local send acceptance, not subscriber completion.
```

If the topic must be explicitly specified, use the
`Publish(channelName, topic, message)` overload. If omitted, the framework uses
the event's packet name as the topic. The interface above has no metadata
setter. So this document's Node direct/ChannelName application metadata
contract doesn't apply to Classic fanout publish.

The exact full signature is defined by
[.NET Channel Messaging Interface](server/languages/dotnet/interfaces/04-channel-messaging.ko.md) and
[.NET Configuration Interface](server/languages/dotnet/interfaces/03-configuration-topology.en.md).

## 7. Failure And Termination

| Condition | Result |
|---|---|
| The Node direct target isn't a member of the same MeshName. | Ends with a target-not-found error. |
| The Node direct target's object role is `Client`. | Ends with `NotFound` since it's not an application target. Doesn't create a Client pair connection. |
| ChannelName isn't registered in the current process. | Ends with `NotFound` and isn't sent via a different send path. |
| ChannelName has no selectable target. | Ends with `NotFound`. |
| A known target's connection doesn't become ready by the time limit. | Ends with a route connection error or timeout. |
| No request handler was found, or the payload couldn't be interpreted. | Completes with an error reply if a reply route remains. |
| No one-way handler was found, or the payload couldn't be interpreted. | Doesn't deliver the message to a handler — records it in runtime observability information. |
| The host isn't accepting new submits. | Fails with that language's shutdown error. |

For a ChannelName call, the framework selects one Server member. A member in a
draining state — finishing existing work while stopping new-work acceptance —
is excluded from this selection candidate set, so it doesn't receive a new
ChannelName message.

Node direct behaves differently since the caller directly specifies the target
RID. Even if the specified node is draining, the framework doesn't switch to a
different RID. Whether this call succeeds is decided by the specified node's
connection and acceptance state.

A server message that doesn't match a request a ClientServer client submitted
is recorded as `ProtocolError` and isn't delivered to an application handler.

Each service runtime converts a transport-specific error into a common
framework result. A transport library's internal result isn't directly exposed
on the public call.

The full termination order is defined by
[Graceful Drain](28-graceful-drain-handoff.en.md).

## 8. Metadata And Observability

### 8.1 Delivering Application Metadata Along With A Message

Application metadata is small key-value information sent separately from
business payload. Setting metadata on a Node direct or ChannelName send/request
call provides it to the selected target's handler context as an unchangeable
[metadata snapshot](01-glossary.en.md#metadata-snapshot). This rule is the same
whether a RouteMesh or ClientServer send path is used.

As shown in the following .NET interface excerpt, send and request calls use a
common metadata-setting interface. You can set one pair directly, or pass an
already-built metadata bundle.

```csharp
public interface IZLinkMetadataCall<TSelf>
{
    TSelf Metadata(string key, string value);
    TSelf Metadata(ZLinkMessageMetadata metadata);
}

public interface IZLinkSendCall : IZLinkMetadataCall<IZLinkSendCall>
{
    ValueTask Async(
        CancellationToken cancellationToken = default);
}

public interface IZLinkRequestCall : IZLinkMetadataCall<IZLinkRequestCall>
{
    ValueTask<TReply> Async<TReply>(
        CancellationToken cancellationToken = default);
}
```

The following example shows sending tenant and locale together with a Channel
request, and how the selected Server's handler reads them. Node direct also
uses the same `Metadata(...)` call.

```csharp
var reply = await routeClient
    .RequestToChannel("billing", new BillingRequest(orderId))
    .Metadata("tenant-id", "tenant-42") // delivers tenant separately from the business payload.
    .Metadata("locale", "ko-KR")        // adds more metadata needed for the same call.
    .Async<BillingReply>(cancellationToken);

public sealed class BillingHandler
    : IZLinkRequestHandler<BillingRequest, BillingReply>
{
    public ValueTask<BillingReply> HandleAsync(
        BillingRequest request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        var tenantId = context.Metadata.Find("tenant-id");
        // the selected Server reads the value from the unchangeable metadata snapshot.
        return HandleBillingAsync(tenantId, request, cancellationToken);
    }
}
```

Metadata's public contract is as follows.

| Item | Contract |
|---|---|
| Key and value | UTF-8, and don't contain NUL. |
| Total size | At most 1024 bytes, including encoded key, value, and structural overhead. |
| Setting the same key multiple times | The last value is sent. |
| Reading in a handler | Provided as an unchangeable snapshot; the application copies it to keep it after the handler turn ends. |
| Malformed metadata | Treated as a protocol error without running the handler. |
| Reply | Request metadata isn't auto-copied, and a regular reply has no metadata setter. |

Even if a handler starts a new request to a different node or Channel, the
current request's metadata isn't auto-copied. A value that needs to carry over
must be explicitly set by the application on the new call. Information the
framework auto-propagates, like trace correlation, is a separate framework
field from application metadata.

Metadata is logically delivered together with the message, but the frame
layout and encoding method inside the packet aren't part of the public
contract. The application doesn't directly build or interpret the metadata
frame. Detailed ownership and delivery rules are defined by
[Message Model](04-message-model.en.md).

The exact .NET signature is defined by
[.NET Common Runtime Interface](server/languages/dotnet/interfaces/01-common-runtime.ko.md) and
[.NET Configuration Interface](server/languages/dotnet/interfaces/03-configuration-topology.en.md).

### 8.2 Observability Information

Observability information must distinguish the following values.

| Item | Meaning |
|---|---|
| ChannelName | Indicates which logical Channel call this is. |
| Send-path kind | Indicates whether RouteMesh or ClientServer path was used. |
| MeshName | Indicates the physical mesh, when a RouteMesh path was used. |
| Source and target RID, or [server identity](01-glossary.en.md#server-identity) | Identifies the node the message actually moved to. |
| Selection result and send-path acceptance | Observes target selection and source-local queue admission, distinguished from each other. |
| Handler delivery result | Indicates the result of delivery to the target queue and handler. |
| Drain state | Indicates why a target was excluded from new selection. |

This physical identifier isn't added to the Channel handler context. Packet
payload, or a business identifier with too large a value space, isn't used as a
metric label.

## 9. Verification Requirements

The implementation and contract test must verify the following conditions.

- A Node direct message isn't delivered to a node other than the RID the caller
  specified.
- ChannelName only selects one send path registered in the current process.
- An unregistered ChannelName isn't automatically delivered via a different
  path.
- ChannelName select-one reflects weight, ready, and drain state together.
- RouteMesh's Node direct and ChannelName use the same MeshNode ROUTER.
- ClientServer Channel uses a separate client transport.
- Targets and handler scopes of different MeshNames aren't mixed.
- After a request failure, it isn't automatically resent to a different Channel
  member.
- Node direct payload doesn't go into a Spot callback or Actor handler.
- A reply completes the original request exactly once and isn't redelivered as
  a new application packet.
- A public publish rejects the fanout-liveness-only topic.
- The liveness beacon isn't delivered to an application handler.
- Metadata set on a Node direct or ChannelName call can be read as an
  unchangeable [snapshot](01-glossary.en.md#snapshot) in the selected target's
  handler context.
- Setting the same metadata key multiple times applies the last value, and
  total size and malformed input are handled per the message model contract.
- Request metadata isn't auto-copied to a reply or to a request a handler
  newly starts.
- Classic fanout's dedicated publish call doesn't provide an application
  metadata setter.
