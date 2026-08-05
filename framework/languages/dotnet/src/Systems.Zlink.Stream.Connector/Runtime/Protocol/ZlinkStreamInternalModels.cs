namespace Systems.Zlink.Stream.Connector.Contracts;

[Flags]
internal enum ZlinkStreamHeaderFlags : byte
{
    None = 0,
    HasRequestSeq = 0x01,
    HasMetadata = 0x02,
    PayloadCompressed = 0x04,
    HasCorrelationId = 0x08,
    HasFlowId = 0x10
}

internal enum ZlinkStreamFlowOrigin : byte
{
    Inbound = 1,
    Timer = 2,
    Application = 3,
    Lifecycle = 4
}

internal readonly record struct ZlinkStreamRequestSeq(ulong Value);

internal sealed record ZlinkStreamHeader(
    ZlinkStreamMessageKind Kind,
    ZlinkStreamCodec Codec,
    ZlinkStreamHeaderFlags Flags,
    ZlinkStreamRequestSeq? RequestSeq,
    string Name,
    ZlinkStreamMetadata Metadata,
    string? CorrelationId = null,
    string? FlowId = null,
    ZlinkStreamFlowOrigin? FlowOrigin = null);
