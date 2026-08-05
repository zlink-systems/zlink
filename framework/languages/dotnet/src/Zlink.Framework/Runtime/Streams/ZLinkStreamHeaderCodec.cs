using Systems.Zlink.Stream.Connector.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkStreamHeaderCodec
{
    private static readonly ZlinkStreamHeaderCodec WireCodec = new();

    public static ReadOnlyMemory<byte> Encode(ZlinkStreamHeader header)
    {
        if (header.Kind != ZlinkStreamMessageKind.Control
            && header.FlowId is null
            && header.FlowOrigin is null
            && ZLinkFlowContext.Current is { } flow)
        {
            header = header with
            {
                FlowId = flow.FlowId,
                FlowOrigin = (ZlinkStreamFlowOrigin)(byte)flow.Origin
            };
        }

        return WireCodec.Encode(header);
    }

    public static ZlinkStreamHeader Decode(ReadOnlyMemory<byte> header) =>
        WireCodec.Decode(header);
}
