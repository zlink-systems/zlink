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

// The canonical command-28 transport is deliberately narrower than the
// relocation workflow.  Its candidate contains only the schema body and the
// optional application payload; transfer and completion identities remain in
// the host's local relocation bookkeeping.
internal readonly record struct ZLinkBackendCanonicalActorJoinRequest(
    ZLinkBackendActorRef Actor,
    ulong ActorNodeGeneration,
    ulong ActorAuthorityOwnerGeneration,
    ulong ActorOwnerLeaseGeneration,
    bool Entry,
    RoutingId TargetNodeRid,
    string TargetSpotId,
    ulong TargetSpotGeneration,
    ulong TargetNodeGeneration,
    ulong TargetAuthorityOwnerGeneration,
    ulong TargetOwnerLeaseGeneration,
    string PacketName,
    string ContentType,
    ReadOnlyMemory<byte> ApplicationPayload);

internal interface IZLinkBackendCanonicalActorJoin
{
    // False is an ordinary capability/authority fallback decision.  The
    // caller retains the established JSON admission path in that case.
    bool CanRequestCanonicalActorJoin(
        ZLinkBackendCanonicalActorJoinRequest request);

    bool RequestCanonicalActorJoin(
        ZLinkBackendCanonicalActorJoinRequest request,
        ActorJoinCallback callback,
        TimeSpan? timeout,
        out ulong correlation);
}

internal readonly record struct ZLinkBackendActorJoinResult(
    RequestResult Result,
    int JoinResultCode,
    ZLinkBackendActorRef Actor,
    string JoinedSpotId,
    ulong JoinEpoch,
    uint Flags,
    //  Fine failure code (ServiceWireConstants.FrameworkErrorCode) from the join
    //  reply header, 0 (None) on success or when the reply carried no fine code.
    //  Defaulted so existing/test constructors need not thread it.
    int FailureErrno = 0,
    ulong JoinedSpotGeneration = 0,
    string ReplyContentType = "");

internal readonly record struct ZLinkBackendActorJoinEntrySpotResult(
    RequestResult Result,
    int JoinResultCode,
    ZLinkBackendActorRef Actor,
    RoutingId TargetNodeRid,
    string JoinedSpotId,
    ulong JoinEpoch,
    uint Flags,
    int FailureErrno = 0);

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
    ZLinkBackendSpotActorLifecycleInfo Info,
    ZLinkApplicationJobQueueLease? ApplicationJobAdmission = null);

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

internal interface IZLinkBackendBoundSessionReplacementNotifications
{
    void SetBoundSessionReplacedNotificationHandler(
        Action<RoutingId, ZLinkServiceWireCodec.BoundSessionReplacedRecord> handler);

    bool TrySendBoundSessionReplacedNotification(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.BoundSessionReplacedRecord record);
}

internal class ZLinkBackendActorJoinRequest(
    ZLinkBackendActorRef sourceActor,
    ZLinkBackendActorRef targetActor,
    RoutingId sourceNodeRid,
    string targetSpotId,
    ulong joinEpoch,
    Message message,
    IReadOnlyList<Message> parts,
    ZLinkCanonicalActorJoin? canonical = null) : IDisposable
{
    private IDisposable? _payloadOwner;
    public ZLinkBackendActorRef SourceActor { get; } = sourceActor;

    public ZLinkBackendActorRef TargetActor { get; } = targetActor;

    public RoutingId SourceNodeRid { get; } = sourceNodeRid;

    public string TargetSpotId { get; } = targetSpotId;

    public ulong JoinEpoch { get; } = joinEpoch;

    public Message Message { get; } = message;

    public IReadOnlyList<Message> Parts { get; } = parts;

    // Present only when the raw transport recognized schema command 28 with
    // the generated decoder.  Private relocation records share command 28;
    // they deliberately leave this null and continue through their legacy
    // lifecycle path.
    internal ZLinkCanonicalActorJoin? Canonical { get; } = canonical;

    internal void AttachPayloadOwner(IDisposable payloadOwner)
    {
        ArgumentNullException.ThrowIfNull(payloadOwner);
        if (Interlocked.CompareExchange(ref _payloadOwner, payloadOwner, null) is not null)
            throw new InvalidOperationException("Actor join already owns its payload envelope.");
    }

    internal ZLinkApplicationJobQueueLease? ApplicationJobAdmission =>
        (_payloadOwner as ZLinkApplicationJobQueueRecordOwner)?.Admission;

    public void Dispose()
    {
        try
        {
            ZLinkMessageParts.DisposeAll(Parts);
        }
        finally
        {
            Interlocked.Exchange(ref _payloadOwner, null)?.Dispose();
        }
    }
}

// The generated command-28 decoder is the wire authority.  This is only the
// framework-owned adapter used by Store admission and typed Spot dispatch.
internal sealed record ZLinkCanonicalActorJoin(
    ZLinkServiceWireCodec.ActorJoinRequestRecord Request,
    ZLinkApplicationPayloadEnvelope? Payload);

internal readonly record struct ZLinkBackendSpotDispatchInfo(
    ZLinkBackendSpotDispatchEvent Event,
    Action? DrainChannelReply = null,
    IReadOnlyList<ZLinkBackendActorPart>? ActorParts = null,
    IReadOnlyList<ZLinkBackendRouteReceived>? RoutedMessages = null,
    IDisposable? ActorPayloadOwner = null);

internal readonly record struct ZLinkBackendSocketMonitorEvent(
    ZLinkSocketNativeEventType NativeEvent,
    RoutingId? RoutingId,
    string LocalAddr,
    string RemoteAddr,
    ulong Value);
