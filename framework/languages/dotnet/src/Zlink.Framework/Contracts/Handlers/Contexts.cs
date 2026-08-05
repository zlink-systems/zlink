namespace Zlink.Framework.Contracts.Handlers;

public interface IZLinkMessageContext
{
    string? MeshName { get; }

    string? ChannelName { get; }

    string PacketName { get; }

    string? ContentType { get; }

    ZLinkMessageMetadata Metadata { get; }

    string? CorrelationId { get; }
}
