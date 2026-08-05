namespace Zlink.Framework.Contracts.Dispatch;

internal enum ZLinkMessageFlowOutcome
{
    Received = 0,
    Dispatched = 1,
    Replied = 2,
    Dropped = 3,
    Sent = 4,
    ReplyReceived = 5,
    Error = 6
}

internal sealed record ZLinkMessageFlowEvent(
    ZLinkMessageFlowOutcome Outcome,
    ZLinkDispatchErrorSurface Surface,
    ZLinkDispatchMessageKind MessageKind,
    string? PacketName = null,
    string? ChannelName = null,
    string? Topic = null,
    string? CorrelationId = null,
    string? SourceRid = null,
    string? LocalRid = null,
    string? PeerRid = null,
    string? SocketRole = null,
    string? SpotId = null,
    string? ActorId = null,
    long? MessageSize = null,
    ZLinkDispatchErrorReason? ErrorReason = null,
    ZLinkDispatchErrorAction? ErrorAction = null,
    string? ErrorType = null,
    string? ErrorMessage = null)
{
    public string FlowId { get; init; } = string.Empty;

    public ZLinkFlowOrigin? FlowOrigin { get; init; }
}

internal enum ZLinkDispatchErrorSurface
{
    Channel = 0,
    RouteMeshChannel = 1,
    SpotRoute = 2,
    SpotSubscription = 3,
    SpotActor = 4,
    StreamSession = 5
}

internal enum ZLinkDispatchMessageKind
{
    Request = 0,
    Send = 1,
    Publish = 2,
    Response = 3,
    Error = 4,
    ActorRequest = 5,
    ActorSend = 6
}

internal enum ZLinkDispatchErrorReason
{
    HandlerMissing = 0,
    PayloadDecodeFailed = 1,
    HandlerException = 2,
    InvalidFrame = 3,
    ReplyPathMissing = 4,
    UnexpectedReply = 5
}

internal enum ZLinkDispatchErrorAction
{
    ReplyError = 0,
    Drop = 1,
    FailCaller = 2
}
