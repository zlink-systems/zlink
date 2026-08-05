namespace Zlink.Framework.Runtime.Handlers;

internal sealed class ZLinkHandlerFilterContext(
    IZLinkMessageContext message,
    ZLinkHandlerDispatchKind dispatchKind) : IZLinkHandlerFilterContext
{
    public string? MeshName => message.MeshName;

    public string? ChannelName => message.ChannelName;

    public string PacketName => message.PacketName;

    public string? ContentType => message.ContentType;

    public ZLinkMessageMetadata Metadata => message.Metadata;

    public string? CorrelationId => message.CorrelationId;

    public ZLinkHandlerDispatchKind DispatchKind { get; } = dispatchKind;
}
