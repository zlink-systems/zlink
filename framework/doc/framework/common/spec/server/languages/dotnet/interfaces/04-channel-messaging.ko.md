# .NET channel messaging 공개 인터페이스

[.NET 언어별 interface 목차](README.ko.md)

## 1. Node direct와 ChannelName

Node direct와 ChannelName은 서로 다른 handler family를 사용한다. [Node direct](../../../00-foundation/02-glossary.ko.md#node-direct) context는 source RID를,
[ChannelName](../../../00-foundation/02-glossary.ko.md#channelname) context는 logical membership을 제공한다.

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

`IZLinkSendHandler`와 `IZLinkRequestHandler`는 `Channel(channelName).Server()` 또는
`AddClientServerChannel(channelName).Server()` builder에 등록한다.
`IZLinkRouteSendHandler`와 `IZLinkRouteRequestHandler`는 MeshNode builder에 등록한다. 같은 packet name을
두 family에 등록할 수 있으며 각 family 안의 중복 key는 startup 오류다.

Global DI client의 Node direct operation은 MeshName을 명시한다. Channel operation은 ChannelName 하나로
process-local RouteMesh 또는 ClientServer 송신 경로를 선택한다.

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

Channel target 선택과 local Server 처리는 [Channel messaging](../../../02-channel-transport/02-channel-messaging.ko.md)과 [ClientServer channel](../../../02-channel-transport/03-client-server-channel.ko.md)이 정한다.

Object Client에는 application Node direct handler를 등록할 수 없으며 해당 RID는 Node direct target이
아니다. Caller가 Object Client RID를 지정하면 다른 target으로 바꾸지 않고
`ZLinkFrameworkErrorKind.NotFound`로 끝낸다. Node direct 호출 자체는 두 Object Client 사이의
connection intent를 만들지 않는다. 다만 어느 한쪽에 RouteMesh Channel Server membership이 있으면
Channel traffic을 위해 peer connection을 유지한다.

`IZLinkMessageContext`는 nullable MeshName과 ChannelName을 제공한다. RouteMesh·Spot·Actor handler의
MeshName은 non-null이고 ClientServer·STREAM handler에서는 null이다. Channel handler의 ChannelName은
non-null이며 Node direct 전용 context만 [MeshName](../../../00-foundation/02-glossary.ko.md#meshname)과 source RID를 추가로 제공한다.
Logical Multicast subscription은 topic과 nullable source를 추가한 `ZLinkPublishMessageContext`를 사용한다.
Correlation ID는 request에서 non-null이고 send에서 null이며 Framework가 reply route와 함께 보존한다.

Classic fanout handler는 독립 fanout channel에서 받은 typed event만 처리한다.

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

`IZLinkFanoutClient.Publish(...)`는 topic overload를 제공한다. Classic fanout의 publish 완료는 [Submit과 completion §6](../../../01-execution/01-submit-and-completion.ko.md)이 정한다. 금지 topic은 [Channel messaging §7](../../../02-channel-transport/02-channel-messaging.ko.md)에 따라 `ArgumentException`으로 표현한다.
