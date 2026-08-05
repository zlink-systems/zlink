namespace Zlink.Framework.Contracts.Channels;

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

public sealed class ZLinkRouteMessageContext : IZLinkMessageContext
{
    internal ZLinkRouteMessageContext(
        string? meshName,
        string? channelName,
        RoutingId sourceNodeRid,
        string packetName,
        string? contentType,
        ZLinkMessageMetadata? metadata,
        string? correlationId)
    {
        MeshName = meshName;
        ChannelName = channelName;
        SourceNodeRid = sourceNodeRid;
        PacketName = packetName;
        ContentType = contentType;
        Metadata = metadata ?? ZLinkMessageMetadata.Empty;
        CorrelationId = correlationId;
    }

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
    internal ZLinkPublishMessageContext(
        string? meshName,
        string? channelName,
        string packetName,
        string? contentType,
        ZLinkMessageMetadata? metadata,
        string? correlationId,
        string topic,
        string? source)
    {
        MeshName = meshName;
        ChannelName = channelName;
        PacketName = packetName;
        ContentType = contentType;
        Metadata = metadata ?? ZLinkMessageMetadata.Empty;
        CorrelationId = correlationId;
        Topic = topic;
        Source = source;
    }

    public string? MeshName { get; }

    public string? ChannelName { get; }

    public string PacketName { get; }

    public string? ContentType { get; }

    public ZLinkMessageMetadata Metadata { get; }

    public string? CorrelationId { get; }

    public string Topic { get; }

    public string? Source { get; }
}
