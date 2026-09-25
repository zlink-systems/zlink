# .NET Channel Messaging Public Interface

[.NET per-language interface table of contents](README.en.md)

## 1. Node Direct And ChannelName

Node direct and ChannelName use different handler families. The
[Node direct](../../../00-foundation/02-glossary.en.md#node-direct) context provides
the source RID, and the
[ChannelName](../../../00-foundation/02-glossary.en.md#channelname) context provides
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

Channel target selection and local Server handling follow [Channel messaging](../../../02-channel-transport/02-channel-messaging.en.md) and [ClientServer channel](../../../02-channel-transport/03-client-server-channel.en.md).

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
provides [MeshName](../../../00-foundation/02-glossary.en.md#meshname) and source
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
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken);
}
```

`IZLinkFanoutClient.Publish(...)` exposes the topic overload; [Submit and completion §6](../../../01-execution/01-submit-and-completion.en.md) defines Classic fanout publish completion. Forbidden topics map to `ArgumentException` as defined by [Channel messaging §7](../../../02-channel-transport/02-channel-messaging.en.md).
