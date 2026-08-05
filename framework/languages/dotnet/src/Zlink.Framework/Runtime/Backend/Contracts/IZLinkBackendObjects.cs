using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Backend.Contracts;

internal enum ZLinkBackendSpotDispatchEvent
{
    Internal = 0,
    RouteReadable = 1,
    ChannelReplyReadable = 2,
    ActorJoinReadable = 3,
    ActorReadable = 4,
    SubscribeReadable = 5,
    ActorLifecycleReadable = 6
}

internal enum ZLinkBackendActorLifecycleEventKind
{
    Joined = 1,
    Left = 2,
    Disconnected = 3
}

internal readonly record struct ZLinkBackendActorRef(
    RoutingId NodeRid,
    string ActorId,
    ulong Generation);

internal interface IZLinkBackendAuthorityObserver
{
    void SetLocalOwnerLeaseGeneration(ulong ownerLeaseGeneration);

    void ObserveActorAuthority(
        ZLinkBackendActorRef actor,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration);

    void ObserveSpotAuthority(
        RoutingId nodeRid,
        string spotId,
        ulong objectGeneration,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration);
}

internal interface IZLinkBackendRequestSourceFenceObserver
{
    void SetLocalRequestSourceFence(
        ZLinkServiceWireCodec.RequestSourceFence source);

    void ObserveRequestSourceFence(
        ZLinkServiceWireCodec.RequestSourceFence source);
}

internal interface IZLinkBackendLocalActorAuthorityReader
{
    bool TryGetLocalActorAuthority(
        ZLinkBackendActorRef actor,
        out ulong authorityOwnerGeneration,
        out ulong ownerLeaseGeneration);
}

internal readonly record struct ZLinkBackendActorJoinResult(
    RequestResult Result,
    int JoinResultCode,
    ZLinkBackendActorRef Actor,
    string JoinedSpotId,
    ulong JoinEpoch,
    uint Flags);

internal readonly record struct ZLinkBackendActorJoinEntrySpotResult(
    RequestResult Result,
    int JoinResultCode,
    ZLinkBackendActorRef Actor,
    RoutingId TargetNodeRid,
    string JoinedSpotId,
    ulong JoinEpoch,
    uint Flags);

internal delegate void ActorJoinCallback(
    ZLinkBackendActorJoinResult result,
    IReadOnlyList<Message> parts);

internal delegate void ActorJoinEntrySpotCallback(
    ZLinkBackendActorJoinEntrySpotResult result,
    IReadOnlyList<Message> parts);

internal readonly record struct ZLinkBackendSpotActorLifecycleInfo(
    ZLinkBackendActorRef? PreviousActor,
    ZLinkBackendActorRef? CurrentActor,
    string? PreviousSpotId,
    string? CurrentSpotId,
    ulong JoinEpoch,
    uint Flags);

internal readonly record struct ZLinkBackendSpotActorLifecycleEvent(
    ZLinkBackendActorLifecycleEventKind Kind,
    ZLinkBackendSpotActorLifecycleInfo Info);

internal readonly record struct ZLinkBackendActorRouteContext(
    MeshOperationId OperationId,
    byte MessageFollowHopCount,
    ulong TargetNodeGeneration,
    ulong AuthorityOwnerGeneration,
    ulong OwnerLeaseGeneration,
    ulong ReplyRequestId = 0,
    uint ReplyFlags = 0,
    string? ReplyCapability = null,
    ulong DeadlineUnixMs = 0,
    bool IsBoundSessionRoute = false)
{
    internal bool IsDirectRoute => OperationId != default && !IsBoundSessionRoute;
}

internal sealed record ZLinkBackendActorPart(
    ZLinkBackendActorRef Actor,
    RoutingId SourceNodeRid,
    RoutingId SourceSessionRid,
    ulong RequestId,
    uint Flags,
    Message Message,
    bool More,
    ZLinkBackendActorRef? ReplyActor = null,
    ZLinkBackendActorRouteContext RouteContext = default,
    ulong SourceNodeGeneration = 0,
    ZLinkServiceWireCodec.RequestSourceFence? RequestSource = null,
    Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? DirectReply = null,
    ReadOnlyMemory<byte> ApplicationMetadata = default);

internal interface IZLinkBackendActorMessageFollowIngress
{
    void SetActorMessageFollowIngressAdmission(
        Func<ActorMessageFollowIngress, bool> admission);

    void SetActorMessageFollowIngressHandler(
        Func<IReadOnlyList<ZLinkBackendActorPart>, bool> handler);
}

internal interface IZLinkBackendMessageFollowNotifications
{
    void SetMessageFollowNotificationHandler(
        Action<RoutingId, ZLinkServiceWireCodec.MessageFollowRecord> handler);

    bool TrySendMessageFollowNotification(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.MessageFollowRecord record);
}

internal class ZLinkBackendActorJoinRequest(
    ZLinkBackendActorRef sourceActor,
    ZLinkBackendActorRef targetActor,
    RoutingId sourceNodeRid,
    string targetSpotId,
    ulong joinEpoch,
    Message message,
    IReadOnlyList<Message> parts)
{
    public ZLinkBackendActorRef SourceActor { get; } = sourceActor;

    public ZLinkBackendActorRef TargetActor { get; } = targetActor;

    public RoutingId SourceNodeRid { get; } = sourceNodeRid;

    public string TargetSpotId { get; } = targetSpotId;

    public ulong JoinEpoch { get; } = joinEpoch;

    public Message Message { get; } = message;

    public IReadOnlyList<Message> Parts { get; } = parts;

    internal ZLinkInboundDispatchLease? DispatchLease { get; set; }
}

internal readonly record struct ZLinkBackendSpotDispatchInfo(
    ZLinkBackendSpotDispatchEvent Event,
    Action? DrainChannelReply = null,
    IReadOnlyList<ZLinkBackendActorPart>? ActorParts = null,
    IReadOnlyList<ZLinkBackendRouteReceived>? RoutedMessages = null,
    ZLinkInboundDispatchLease? ActorDispatchLease = null);

internal readonly record struct ZLinkBackendSocketMonitorEvent(
    ZLinkSocketNativeEventType NativeEvent,
    RoutingId? RoutingId,
    string LocalAddr,
    string RemoteAddr,
    uint Value);

internal interface IZLinkBackendContext : IAsyncDisposable
{
    void Shutdown();
}
