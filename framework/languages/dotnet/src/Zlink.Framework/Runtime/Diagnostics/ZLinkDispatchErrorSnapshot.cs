namespace Zlink.Framework.Runtime.Diagnostics;

internal sealed record ZLinkDispatchErrorSnapshot(
    ZLinkDispatchErrorSurface Surface,
    ZLinkDispatchMessageKind MessageKind,
    ZLinkDispatchErrorReason Reason,
    ZLinkDispatchErrorAction Action,
    string? PacketName,
    string? ChannelName = null,
    string? Topic = null,
    string? SpotId = null,
    string? ActorId = null,
    string? SourceRid = null,
    string? CorrelationId = null,
    string? MeshName = null,
    string? ChannelRouteKind = null,
    string? TargetRid = null,
    string? ServerRid = null,
    string? InstanceSpotType = null,
    string? ActivationState = null,
    string? FlowId = null,
    ZLinkFlowOrigin? FlowOrigin = null,
    string? StreamSessionId = null
)
{
    internal ZLinkDispatchErrorDetails Details { get; init; }
}
