namespace Zlink.Framework.Contracts.Handlers;

internal sealed class ZLinkMessageContext(
    string? meshName,
    string? channelName,
    string packetName,
    string? contentType,
    ZLinkMessageMetadata? metadata,
    string? correlationId) : IZLinkMessageContext
{
    public string? MeshName { get; } = meshName;

    public string? ChannelName { get; } = channelName;

    public string PacketName { get; } = packetName;

    public string? ContentType { get; } = contentType;

    public ZLinkMessageMetadata Metadata { get; } =
        metadata ?? ZLinkMessageMetadata.Empty;

    public string? CorrelationId { get; } = correlationId;
}
