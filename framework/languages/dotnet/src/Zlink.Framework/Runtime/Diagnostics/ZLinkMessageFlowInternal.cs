namespace Zlink.Framework.Contracts.Dispatch;

internal enum ZLinkMessageFlowOutcome
{
    Received = 0,
    Dispatched = 1,
    Replied = 2,
    Dropped = 3,
    Sent = 4,
    ReplyReceived = 5,
    Admitted = 6,
    Completed = 7,
    Backpressured = 8
}

internal enum ZLinkMessageFlowResult
{
    Succeeded = 0,
    Failed = 1,
    Backpressured = 2,
    Dropped = 3,
    Cancelled = 4,
    Shutdown = 5
}

internal enum ZLinkMessageFlowReason
{
    Backpressure = 0,
    StaleTarget = 1,
    TargetClosed = 2,
    Shutdown = 3,
    LocationUnavailable = 4,
    ActivationRejected = 5,
    ActivationTimeout = 6
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
    string? MeshName = null,
    string? ChannelRouteKind = null,
    string? TargetRid = null,
    string? ServerRid = null,
    string? InstanceSpotType = null,
    string? ActivationState = null,
    double? DurationSeconds = null,
    ulong? SourceMeshGeneration = null,
    ZLinkMessageFlowResult? Result = null,
    ZLinkMessageFlowReason? Reason = null)
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
    StreamSession = 5,
    Node = 6,
    InstanceSpot = 7,
    ActorRelocation = 8,
    ClassicFanout = 9
}

internal enum ZLinkDispatchMessageKind
{
    Request = 0,
    Send = 1,
    Publish = 2,
    Response = 3,
    Error = 4,
    ActorRequest = 5,
    ActorSend = 6,
    Control = 7
}

internal enum ZLinkDispatchErrorReason
{
    HandlerMissing = 0,
    PayloadDecodeFailed = 1,
    HandlerException = 2,
    InvalidFrame = 3,
    ReplyPathMissing = 4,
    UnexpectedReply = 5,
    Backpressure = 6,
    StaleTarget = 7,
    Shutdown = 8
}

internal enum ZLinkDispatchErrorAction
{
    ReplyError = 0,
    Drop = 1,
    FailCaller = 2
}
