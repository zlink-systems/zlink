namespace Zlink.Framework.Runtime.Dispatch;

internal sealed record ZLinkDispatchFailure(
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
    Exception? Exception = null);