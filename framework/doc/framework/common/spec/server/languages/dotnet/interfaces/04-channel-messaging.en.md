# .NET Channel Messaging Public Interface

[.NET exact interface table of contents](README.en.md)

## 1. Node Direct And ChannelName

Node direct and ChannelName use different handler families. The
[Node direct](../../../../01-glossary.en.md#node-direct) context provides
the source RID, and the
[ChannelName](../../../../01-glossary.en.md#channelname) context provides
logical membership.

```csharp
public interface IZLinkMessageContext
{
    string? MeshName { get; }
    string? ChannelName { get; }
    string PacketName { get; }
    string? ContentType { get; }
    ZLinkMessageMetadata Metadata { get; }
    string? CorrelationId { get; }
}

public sealed class ZLinkRouteMessageContext : IZLinkMessageContext
{
    public string? MeshName { get; }
    public string? ChannelName { get; }
    public string PacketName { get; }
    public string? ContentType { get; }
    public ZLinkMessageMetadata Metadata { get; }
    public string? CorrelationId { get; }
    public RoutingId SourceNodeRid { get; }
}

public sealed class ZLinkPublishMessageContext : IZLinkMessageContext
{
    public string? MeshName { get; }
    public string? ChannelName { get; }
    public string PacketName { get; }
    public string? ContentType { get; }
    public ZLinkMessageMetadata Metadata { get; }
    public string? CorrelationId { get; }
    public string Topic { get; }
    public string? Source { get; }
}

public interface IZLinkSendHandler<in TMessage>
{
    ValueTask HandleAsync(
        TMessage message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken);
}

public interface IZLinkRequestHandler<in TRequest, TResponse>
{
    ValueTask<TResponse> HandleAsync(
        TRequest request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken);
}

public interface IZLinkRouteSendHandler<in TMessage>
{
    ValueTask HandleAsync(
        TMessage message,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken);
}

public interface IZLinkRouteRequestHandler<in TRequest, TReply>
{
    ValueTask<TReply> HandleAsync(
        TRequest request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken);
}
```

`IZLinkSendHandler` and `IZLinkRequestHandler` are registered on the
`Channel(channelName).Server()` or
`AddClientServerChannel(channelName).Server()` builder.
`IZLinkRouteSendHandler` and `IZLinkRouteRequestHandler` are registered
on the MeshNode builder. The same packet name can be registered in both
families, and a duplicate key within one family is a startup error.

The global DI client's Node direct operation specifies MeshName. The
Channel operation selects the process-local RouteMesh or ClientServer
send path using only ChannelName.

```csharp
public interface IZLinkRouteClient
{
    IZLinkSendCall SendToNode<TMessage>(
        string meshName,
        RoutingId targetNodeRid,
        TMessage message);

    IZLinkRequestCall RequestToNode<TRequest>(
        string meshName,
        RoutingId targetNodeRid,
        TRequest request);

    IZLinkSendCall SendToChannel<TMessage>(
        string channelName,
        TMessage message);

    IZLinkRequestCall RequestToChannel<TRequest>(
        string channelName,
        TRequest request);
}
```

Node direct submits to a single target RID that isn't an Object Client.
The Channel operation selects the unique
[RouteMesh](../../../../01-glossary.en.md#routemesh) or ClientServer send
path for a ChannelName from the process-local route index. It selects one
ready positive-weight member by round-robin and submits within the same
operation, and the client doesn't return the selected RID. If ClientServer
has a local Server on the same ChannelName, it's included as a candidate
under the same readiness/weight/drain conditions as a remote Server —
local priority or remote exclusion isn't applied. Even if a local Server
is selected, the same codec, timeout, cancellation, correlation, and
terminal completion contract as a remote Server applies. A separate
public path that calls a local handler directly isn't provided.

An application Node direct handler can't be registered on an Object
Client, and that RID isn't a Node direct target. If the caller specifies
an Object Client RID, it doesn't switch to a different target and ends
with `ZLinkFrameworkErrorKind.NotFound`. The Node direct call itself
doesn't create connection intent between two Object Clients. However, if
either side has RouteMesh Channel Server membership, the peer connection
is kept for Channel traffic.

`IZLinkMessageContext` provides a nullable MeshName and ChannelName. The
MeshName on a RouteMesh/Spot/Actor handler is non-null, and it's null on
a ClientServer/STREAM handler. A Channel handler's ChannelName is
non-null, and only the Node-direct-dedicated context additionally
provides [MeshName](../../../../01-glossary.en.md#meshname) and source
RID. A Logical Multicast subscription uses `ZLinkPublishMessageContext`,
which adds topic and a nullable source. Correlation ID is non-null on a
request and null on a send, and the framework preserves it together with
the reply route.

A classic fanout handler only processes typed events received on an
independent fanout channel.

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

public interface IZLinkFanoutHandler<in TEvent>
{
    ValueTask HandleAsync(
        TEvent message,
        CancellationToken cancellationToken);
}
```

`IZLinkFanoutClient.Publish(...)` takes a ChannelName and typed event,
and a call that needs an explicit topic uses the
[topic](../../../../01-glossary.en.md#topic) overload. If topic is
omitted, the framework uses the event's
[packet name](../../../../01-glossary.en.md#packet-name) as topic. A
reserved topic is rejected with `ArgumentException`. The `Async(...)` of
the returned dedicated call completes normally once source-local publish
admission finishes. It doesn't return subscriber count or receipt
completion. `IZLinkPublishCall` is a Logical-Multicast-dedicated call and
isn't used for
[classic fanout](../../../../01-glossary.en.md#classic-fanout).
