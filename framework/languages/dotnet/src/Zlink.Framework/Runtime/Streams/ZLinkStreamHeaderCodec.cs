using Systems.Zlink.Stream.Connector.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkStreamHeaderCodec
{
    private static readonly ZlinkStreamHeaderCodec WireCodec = new();

    public static ReadOnlyMemory<byte> Encode(ZlinkStreamHeader header)
    {
        if (
            header.Kind != ZlinkStreamMessageKind.Control
            && header.FlowId is null
            && header.FlowOrigin is null
            && ZLinkFlowContext.Current is { } flow
        )
        {
            header = header with
            {
                FlowId = flow.FlowId,
                FlowOrigin = ToConnectorOrigin(flow.Origin),
            };
        }

        return WireCodec.Encode(header);
    }

    /// <summary>
    ///     Maps a connector flow origin onto the framework enum.
    /// </summary>
    /// <remarks>
    ///     The framework enum carries the wire values 1..4 while
    ///     <see cref="ZlinkStreamFlowOrigin" /> uses ordinals 0..3, so the two are mapped
    ///     by name. A numeric cast would shift every value by one.
    /// </remarks>
    public static ZLinkFlowOrigin ToFrameworkOrigin(ZlinkStreamFlowOrigin origin) =>
        origin switch
        {
            ZlinkStreamFlowOrigin.Inbound => ZLinkFlowOrigin.Inbound,
            ZlinkStreamFlowOrigin.Timer => ZLinkFlowOrigin.Timer,
            ZlinkStreamFlowOrigin.Application => ZLinkFlowOrigin.Application,
            ZlinkStreamFlowOrigin.Lifecycle => ZLinkFlowOrigin.Lifecycle,
            _ => ZLinkFlowOrigin.Application,
        };

    private static ZlinkStreamFlowOrigin ToConnectorOrigin(ZLinkFlowOrigin origin) =>
        origin switch
        {
            ZLinkFlowOrigin.Inbound => ZlinkStreamFlowOrigin.Inbound,
            ZLinkFlowOrigin.Timer => ZlinkStreamFlowOrigin.Timer,
            ZLinkFlowOrigin.Application => ZlinkStreamFlowOrigin.Application,
            ZLinkFlowOrigin.Lifecycle => ZlinkStreamFlowOrigin.Lifecycle,
            _ => ZlinkStreamFlowOrigin.Application,
        };

    public static ZlinkStreamHeader Decode(ReadOnlyMemory<byte> header, bool captureFlow = true) =>
        WireCodec.Decode(header, captureFlow);
}
