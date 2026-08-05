using Systems.Zlink;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Service;

internal enum MeshNodeState { Created = 1, Started, PartialReady, Ready, Draining, Stopped, Error }
internal enum MeshPeerSource { Manual = 1, Discovery, Mixed }
internal enum MeshPeerState
{
    Configured = 1,
    Connecting,
    Admitted,
    Draining,
    Closed,
    Error,
    NotRequired
}

internal sealed record MeshNodeStatus(
    MeshNodeState State, RoutingId RoutingId, string MeshName, string LocalEndpoint,
    ulong LifecycleGeneration, ulong DescriptorRevision, uint ChannelCount,
    uint ConfiguredPeerCount, uint AdmittedPeerCount, uint DrainingPeerCount,
    ulong PendingApplicationMessages, ulong PendingInfrastructureMessages,
    ulong PendingBytes, int LastError, ulong LastChangedMs);

internal sealed record MeshNodePeer(
    ulong ConnectionIntentId, MeshPeerSource Source, MeshPeerState State,
    RoutingId RoutingId, ulong LifecycleGeneration, ulong DescriptorRevision,
    string Endpoint, uint ChannelCount, int LastError, ulong LastChangedMs)
{
    internal ZLinkMeshNodeObjectRole ObjectRole { get; init; }
}

internal sealed record MeshPeerChannel(string Name, uint Weight);
internal readonly record struct MeshOperationId(ulong High, ulong Low);

[Flags]
internal enum MeshReadyDomains : uint
{
    None = 0,
    Application = 1,
    Infrastructure = 2,
    All = Application | Infrastructure
}

internal enum MeshOwnerKind { Node = 1, Spot, Actor }
internal enum MeshRecordKind
{
    NodeSend = 1, NodeRequest, ChannelSend, ChannelRequest, SpotSend,
    SpotRequest, SpotMulticast, SpotControl, ActorSend, ActorRequest,
    Completion, SendReady, InstanceSpotActivation
}

internal enum MeshOperationKind
{
    NodeRequest = 1, ChannelRequest, SpotRequest, ActorRequest, ActorLookup,
    ActorDestroy, ActorJoin, ActorLeave, StreamBind, StreamUnbind, StreamClose,
    InstanceSpotRequest, UserSpotCreate, UserSpotClose, ActorCreate
}

internal enum ActorLifecycleKind { Created = 1, Joined, Left, Disconnected, Destroyed }
internal enum ActorJoinResult { Accepted = 0, Rejected = 1 }
internal enum MeshDestinationKind { Node = 1, Channel, Spot, Actor, BoundSession }

internal abstract record MeshRecordPayload;
internal sealed record ActorControlRecord(
    ActorLifecycleKind Kind, ActorRef PreviousActor, ActorRef CurrentActor,
    string PreviousSpotId, string CurrentSpotId,
    ulong PreviousSpotGeneration, ulong CurrentSpotGeneration,
    ulong PreviousMembershipEpoch, ulong CurrentMembershipEpoch,
    int ResultCode) : MeshRecordPayload;

internal sealed record ActorLocation(
    ActorRef Actor, string SpotId, ulong SpotGeneration, ulong MembershipEpoch);

internal sealed record ActorJoinCompletion(
    ActorJoinResult JoinResult, ActorRef Actor, ActorLocation Location) : MeshRecordPayload;

internal sealed record MeshSendReadyData(
    MeshDestinationKind DestinationKind, RoutingId TargetNodeRid,
    string TargetSpotId, ActorRef TargetActor,
    string? ChannelName) : MeshRecordPayload;

internal readonly record struct ObjectReservationFence(
    string ReservationId,
    string ExpectedStoreVersion,
    ulong ObjectGeneration,
    ulong AuthorityOwnerGeneration,
    RoutingId TargetNodeRid,
    ulong TargetNodeGeneration,
    string TargetOwnerId,
    ulong TargetOwnerLeaseGeneration,
    uint PendingCapacityDelta);

internal readonly record struct UserSpotCloseFence(
    string SpotId,
    ulong ObjectGeneration,
    RoutingId TargetNodeRid,
    ulong TargetNodeGeneration,
    ulong AuthorityOwnerGeneration,
    string ExpectedStoreVersion);

internal readonly record struct UserSpotCreateOperation(
    ulong Correlation,
    MeshOperationId OperationId,
    RoutingId SourceNodeRid,
    ulong SourceNodeGeneration,
    string SpotId,
    string StableType,
    ObjectReservationFence Reservation,
    ulong DeadlineUnixMs);

internal readonly record struct UserSpotCloseOperation(
    ulong Correlation,
    MeshOperationId OperationId,
    RoutingId SourceNodeRid,
    ulong SourceNodeGeneration,
    UserSpotCloseFence Target,
    ulong DeadlineUnixMs);

internal enum UserSpotCreateResult : byte
{
    Existing = 1,
    Created = 2,
    Rejected = 3
}

internal sealed record UserSpotCreateCompletion(
    UserSpotCreateResult Result,
    string SpotId,
    ulong ObjectGeneration) : MeshRecordPayload;

internal sealed record UserSpotCloseCompletion(bool Closed) : MeshRecordPayload;

internal readonly record struct ActorCreateOperation(
    ulong Correlation,
    MeshOperationId OperationId,
    RoutingId SourceNodeRid,
    ulong SourceNodeGeneration,
    string ActorId,
    string StableType,
    ObjectReservationFence Reservation,
    ulong DeadlineUnixMs);

internal enum ActorCreateResult : byte
{
    Existing = 1,
    Created = 2,
    Rejected = 3
}

internal sealed record ActorCreateCompletion(
    ActorCreateResult Result,
    ActorRef Actor) : MeshRecordPayload;

internal sealed record ActorCreateOperationTerminal(
    RequestResult Result,
    ServiceWireConstants.FrameworkErrorCode FailureCode,
    ActorCreateCompletion? Completion = null,
    IReadOnlyList<ReadOnlyMemory<byte>>? ReplyParts = null);

internal interface IActorCreateOperationTarget
{
    ValueTask<ActorCreateOperationTerminal> CreateAsync(
        ActorCreateOperation operation,
        CancellationToken cancellationToken);
}

internal readonly record struct ActorDestroyOperation(
    ulong Correlation,
    ActorRef Actor,
    RoutingId TargetNodeRid,
    ulong TargetNodeGeneration,
    ulong AuthorityOwnerGeneration);

internal sealed record ActorDestroyCompletion(bool Destroyed) : MeshRecordPayload;

internal sealed record ActorDestroyOperationTerminal(
    RequestResult Result,
    ServiceWireConstants.FrameworkErrorCode FailureCode,
    ActorDestroyCompletion? Completion = null);

internal interface IActorDestroyOperationTarget
{
    ValueTask<ActorDestroyOperationTerminal> DestroyAsync(
        ActorDestroyOperation operation,
        CancellationToken cancellationToken);
}

internal readonly record struct ActorMessageFollowIngress(
    RoutingId SourceNodeRid,
    ulong SourceNodeGeneration,
    string SourceSpotId,
    ActorRef TargetActor,
    MeshOperationId OperationId,
    ulong ReplyRouteId,
    ulong TargetNodeGeneration,
    ulong AuthorityOwnerGeneration,
    ulong OwnerLeaseGeneration,
    byte MessageFollowHopCount,
    ulong DeadlineUnixMs,
    ReadOnlyMemory<byte> ApplicationMetadata,
    IReadOnlyList<Message> Parts,
    Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? Reply)
{
    // A stale Actor route is admitted by the follow target before the payload is
    // materialized. The adapter decodes this envelope only after it accepts the
    // follow, while the existing Parts property remains available to internal
    // callers that already own materialized messages.
    internal ReadOnlyMemory<byte> EncodedPayload { get; init; }

    // The received metadata message remains owned by the synchronous ingress
    // call. Copy it only after admission so a rejected stale route does not
    // allocate an application-metadata snapshot.
    internal Message? ApplicationMetadataSource { get; init; }
}

internal interface IActorMessageFollowIngressTarget
{
    bool TryFollow(ActorMessageFollowIngress ingress);
}

internal readonly record struct InstanceSpotActivationTarget(
    string MeshName,
    RoutingId TargetNodeRid,
    ulong TargetNodeGeneration,
    string TargetSpotId,
    string StableType,
    string DescriptorVersion);

internal readonly record struct InstanceSpotIntentAddress(
    string MeshName,
    string InstanceSpotType,
    string SpotId);

internal readonly record struct InstanceSpotActivationOperation(
    InstanceSpotActivationTarget Target,
    RoutingId SourceNodeRid,
    ulong SourceNodeGeneration,
    string SourceSpotId,
    MeshOperationId OperationId,
    bool IsRequest,
    ulong ReplyRouteId,
    ulong DeadlineUnixMs);

internal sealed record InstanceSpotActivationTerminal(
    RequestResult Result,
    ServiceWireConstants.FrameworkErrorCode FailureCode,
    IReadOnlyList<ReadOnlyMemory<byte>> ReplyParts,
    bool Forwarded = false);

internal interface IInstanceSpotActivationTarget
{
    ValueTask<InstanceSpotActivationTerminal> ActivateAsync(
        InstanceSpotActivationOperation operation,
        ReadOnlyMemory<byte>? metadata,
        IReadOnlyList<ReadOnlyMemory<byte>> payload,
        CancellationToken cancellationToken);
}

internal sealed record UserSpotOperationTerminal(
    RequestResult Result,
    ServiceWireConstants.FrameworkErrorCode FailureCode,
    MeshRecordPayload? Completion = null,
    IReadOnlyList<ReadOnlyMemory<byte>>? ReplyParts = null);

internal interface IUserSpotOperationTarget
{
    ValueTask<UserSpotOperationTerminal> CreateAsync(
        UserSpotCreateOperation operation,
        CancellationToken cancellationToken);

    ValueTask<UserSpotOperationTerminal> CloseAsync(
        UserSpotCloseOperation operation,
        CancellationToken cancellationToken);
}

internal interface IRelocationReplyRelayTarget
{
    // The target owns payload for every completion path. It either transfers
    // the messages to the pending operation or disposes them before returning.
    ValueTask<ZLinkServiceWireCodec.ReplyRelayAckRecord?> RelayAsync(
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        IReadOnlyList<Message> payload,
        CancellationToken cancellationToken);
}

internal enum ZLinkRelocationReplyCompletionState : byte
{
    NotFound = 0,
    TerminalReceived = 1,
    AlreadyTerminal = 2
}

internal readonly record struct ZLinkRelocationReplyCompletion(
    ZLinkRelocationReplyCompletionState State,
    ZLinkServiceWireCodec.RequestSourceFence RequestSource);

internal interface ICanonicalRelocationReservationTarget
{
    ValueTask<ZLinkServiceWireCodec.RelocationReadyRecord> OfferAsync(
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        RoutingId authenticatedSourceNodeRid,
        CancellationToken cancellationToken);

    ValueTask<ZLinkServiceWireCodec.RelocationReservedRecord> AcceptAsync(
        ZLinkServiceWireCodec.RelocationReadyRecord acceptance,
        RoutingId authenticatedSourceNodeRid,
        CancellationToken cancellationToken);

    ValueTask<ZLinkServiceWireCodec.RelocationAckRecord> StageDataAsync(
        ZLinkServiceWireCodec.RelocationDataRecord data,
        RoutingId authenticatedSourceNodeRid,
        CancellationToken cancellationToken);

    bool TryCreateSealRequest(
        ZLinkServiceWireCodec.RelocationWireId relocationId,
        ulong targetAttemptGeneration,
        out ZLinkServiceWireCodec.RelocationSealRecord seal);

    ValueTask AcceptSealResponseAsync(
        ZLinkServiceWireCodec.RelocationSealRecord seal,
        RoutingId authenticatedSourceNodeRid,
        CancellationToken cancellationToken);

    ValueTask CompleteAsync(
        ZLinkServiceWireCodec.RelocationCompleteRecord complete,
        RoutingId authenticatedSourceNodeRid,
        CancellationToken cancellationToken);
}

[Flags]
internal enum MeshMonitorEventMask : ulong
{
    None = 0,
    StateChanged = 1UL << 0,
    PeerConnecting = 1UL << 1,
    PeerAdmitted = 1UL << 2,
    PeerDraining = 1UL << 3,
    PeerClosed = 1UL << 4,
    PeerRejected = 1UL << 5,
    ChannelChanged = 1UL << 6,
    MessageSubmitted = 1UL << 7,
    Backpressured = 1UL << 8,
    OperationCompleted = 1UL << 9,
    ProtocolError = 1UL << 10,
    ClaimRevoked = 1UL << 11,
    PeerNotRequired = 1UL << 12,
    All = (1UL << 13) - 1
}

internal enum MeshMonitorEventKind
{
    StateChanged = 1, PeerConnecting, PeerAdmitted, PeerDraining, PeerClosed,
    PeerRejected, ChannelChanged, MessageSubmitted, Backpressured,
    OperationCompleted, ProtocolError, ClaimRevoked, PeerNotRequired
}

internal sealed record MeshMonitorEvent(
    MeshMonitorEventKind Kind, ulong TimestampMs, ulong MeshLifecycleGeneration,
    ulong MeshDescriptorRevision, MeshNodeState MeshState, RoutingId PeerRid,
    ulong PeerLifecycleGeneration, ulong PeerDescriptorRevision,
    MeshOwnerKind OwnerKind, string SpotId, ActorRef Actor,
    string ChannelName, MeshOperationId OperationId,
    int ResultCode, int FailureErrno);

internal sealed record MeshMonitorStatus(
    MeshNodeState State, ulong PeerAdmitted, ulong PeerRejected,
    ulong SubmittedMessages, ulong CompletedOperations, ulong ProtocolErrors,
    ulong BackpressuredSubmits, ulong LastSequence);

internal interface IMeshNodeMonitor : IDisposable, IAsyncDisposable
{
    MeshMonitorStatus Status();
    MeshMonitorEvent? Recv(RecvFlags flags = RecvFlags.None);
}

internal readonly record struct MeshReadyRecord(
    MeshOwnerKind OwnerKind, MeshReadyDomains Domain,
    string SpotId, ActorRef Actor);

internal sealed class MeshReadyBatch : IDisposable
{
    private readonly List<(MeshReadyRecord Record, MeshClaim Claim)> _entries = new();
    public int Count => _entries.Count;
    public MeshReadyRecord this[int index] => _entries[index].Record;
    public MeshClaim TakeClaim(int index) => _entries[index].Claim;
    internal void Add(MeshReadyRecord record, MeshClaim claim) =>
        _entries.Add((record, claim));
    internal void Reset()
    {
        foreach (var (_, claim) in _entries)
            claim.Dispose();
        _entries.Clear();
    }
    public void Dispose() => Reset();
}

internal sealed class MeshReceiveBatch : IDisposable
{
    private readonly List<(MeshReceiveRecord Record, IReadOnlyList<Message> Parts)> _entries = new();
    internal int MaximumRecords { get; set; } = int.MaxValue;
    internal long MaximumBytes { get; set; } = long.MaxValue;
    internal long StartedAt { get; set; }
    internal long Bytes { get; private set; }
    public int Count => _entries.Count;
    public MeshReceiveRecord this[int index] => _entries[index].Record;
    public IReadOnlyList<Message> RetainMessage(int index) =>
        _entries[index].Parts.Select(Message.From).ToArray();
    internal bool CanAdd(long bytes)
    {
        if (Count == 0) return true;
        if (Count >= MaximumRecords || Bytes >= MaximumBytes) return false;
        if (StartedAt != 0
            && (System.Diagnostics.Stopwatch.GetTimestamp() - StartedAt)
                * 1000L / System.Diagnostics.Stopwatch.Frequency
                >= Zlink.Framework.Runtime.Dispatch.ZLinkReceiveBatchBudget.MaximumMilliseconds)
            return false;
        return bytes <= MaximumBytes - Math.Min(Bytes, MaximumBytes);
    }

    internal void Add(MeshReceiveRecord record, IReadOnlyList<Message> parts)
    {
        _entries.Add((record, parts));
        Bytes = checked(Bytes + parts.Sum(static part => Math.Max(part.Size, 0)));
    }
    public void Reset()
    {
        foreach (var (record, parts) in _entries)
        {
            foreach (var part in parts)
                part.Dispose();
            record.InboundDispatchLease?.Dispose();
        }
        _entries.Clear();
        Bytes = 0;
    }

    internal ZLinkInboundDispatchLease? TakeInboundDispatchLease(int index)
    {
        var entry = _entries[index];
        var lease = entry.Record.InboundDispatchLease;
        entry.Record.InboundDispatchLease = null;
        _entries[index] = entry;
        return lease;
    }

    public void Dispose() => Reset();
}

internal sealed class MeshClaim : IDisposable
{
    internal Func<MeshReceiveBatch, RecvFlags, bool>? Receiver { get; init; }
    internal Action? Releaser { get; init; }
    private int _disposed;
    public bool Receive(MeshReceiveBatch batch, RecvFlags flags = RecvFlags.None) =>
        Volatile.Read(ref _disposed) == 0 && (Receiver?.Invoke(batch, flags) ?? false);
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) == 0)
            Releaser?.Invoke();
    }
}

internal struct MeshReceiveRecord
{
    private readonly Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? _reply;
    private readonly Func<ActorJoinResult, IReadOnlyList<Message>, SendFlags, SubmitResult>?
        _joinReply;
    internal MeshReceiveRecord(
        MeshRecordKind kind, MeshReadyDomains domain, RoutingId sourceNodeRid,
        string sourceSpotId, ulong sourceBindingGeneration, ActorRef sourceActor,
        MeshOperationId operationId, MeshOperationKind operationKind,
        string? channelName, string? topic, byte[]? applicationMetadata,
        int partOffset, int partCount, int terminalResult, int failureErrno,
        MeshRecordPayload? kindData,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? reply = null,
        Func<ActorJoinResult, IReadOnlyList<Message>, SendFlags, SubmitResult>?
            joinReply = null,
        ulong targetNodeGeneration = 0,
        ulong authorityOwnerGeneration = 0,
        ulong ownerLeaseGeneration = 0,
        byte messageFollowHopCount = 0,
        ulong replyRouteId = 0,
        ulong deadlineUnixMs = 0)
    {
        Kind = kind; Domain = domain; SourceNodeRid = sourceNodeRid;
        SourceSpotId = sourceSpotId; SourceBindingGeneration = sourceBindingGeneration;
        SourceActor = sourceActor; OperationId = operationId; OperationKind = operationKind;
        ChannelName = channelName; Topic = topic; ApplicationMetadata = applicationMetadata;
        PartOffset = partOffset; PartCount = partCount; TerminalResult = terminalResult;
        FailureErrno = failureErrno; KindData = kindData; _reply = reply;
        _joinReply = joinReply;
        TargetNodeGeneration = targetNodeGeneration;
        AuthorityOwnerGeneration = authorityOwnerGeneration;
        OwnerLeaseGeneration = ownerLeaseGeneration;
        MessageFollowHopCount = messageFollowHopCount;
        ReplyRouteId = replyRouteId;
        DeadlineUnixMs = deadlineUnixMs;
    }
    public MeshRecordKind Kind { get; }
    public MeshReadyDomains Domain { get; }
    public RoutingId SourceNodeRid { get; }
    public string SourceSpotId { get; }
    public ulong SourceBindingGeneration { get; }
    public ActorRef SourceActor { get; }
    public MeshOperationId OperationId { get; }
    public MeshOperationKind OperationKind { get; }
    public string? ChannelName { get; }
    public string? Topic { get; }
    public byte[]? ApplicationMetadata { get; }
    public int PartOffset { get; }
    public int PartCount { get; }
    public int TerminalResult { get; }
    public int FailureErrno { get; }
    public ulong TargetNodeGeneration { get; }
    public ulong AuthorityOwnerGeneration { get; }
    public ulong OwnerLeaseGeneration { get; }
    public byte MessageFollowHopCount { get; }
    public ulong ReplyRouteId { get; }
    public ulong DeadlineUnixMs { get; }
    public MeshRecordPayload? KindData { get; }
    internal ZLinkInboundDispatchLease? InboundDispatchLease { get; set; }

    internal bool RequiresApplicationDispatchLease =>
        Domain == MeshReadyDomains.Application
        && (Kind is MeshRecordKind.NodeSend
            or MeshRecordKind.NodeRequest
            or MeshRecordKind.ChannelSend
            or MeshRecordKind.ChannelRequest
            or MeshRecordKind.SpotSend
            or MeshRecordKind.SpotRequest
            or MeshRecordKind.SpotMulticast
            or MeshRecordKind.ActorSend
            or MeshRecordKind.ActorRequest
            || Kind == MeshRecordKind.SpotControl
               && OperationKind == MeshOperationKind.ActorJoin);

    public ActorControlRecord? ActorControl => KindData as ActorControlRecord;
    public ActorJoinCompletion? JoinCompletion => KindData as ActorJoinCompletion;
    public UserSpotCreateCompletion? UserSpotCreateCompletion =>
        KindData as UserSpotCreateCompletion;
    public UserSpotCloseCompletion? UserSpotCloseCompletion =>
        KindData as UserSpotCloseCompletion;
    public ActorCreateCompletion? ActorCreateCompletion =>
        KindData as ActorCreateCompletion;
    public ActorDestroyCompletion? ActorDestroyCompletion =>
        KindData as ActorDestroyCompletion;
    public MeshSendReadyData? SendReady => KindData as MeshSendReadyData;
    internal Func<IReadOnlyList<Message>, SendFlags, SubmitResult>?
        CaptureReplyRoute() => _reply;
    public SubmitResult Reply(IReadOnlyList<Message> parts, SendFlags flags = SendFlags.None) =>
        _reply?.Invoke(parts, flags) ?? SubmitResult.Terminated;
    public SubmitResult ReplyJoin(
        ActorJoinResult result,
        IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None) =>
        _joinReply?.Invoke(result, parts, flags) ?? Reply(parts, flags);

    internal static MeshReceiveRecord CompletionFailure(
        MeshOperationId operationId,
        RequestResult result) =>
        CompletionFailure(
            operationId,
            MeshOperationKind.NodeRequest,
            result,
            failureErrno: 0);

    internal static MeshReceiveRecord CompletionFailure(
        MeshOperationId operationId,
        MeshOperationKind operationKind,
        RequestResult result,
        int failureErrno = 0) =>
        new(
            MeshRecordKind.Completion,
            MeshReadyDomains.Infrastructure,
            default,
            string.Empty,
            0,
            default,
            operationId,
            operationKind,
            null,
            null,
            null,
            0,
            0,
            (int) result,
            failureErrno,
            null);
}

internal interface IMeshNode : IDisposable, IAsyncDisposable
{
    void SetInboundDispatchBudget(ZLinkInboundDispatchBudget budget) { }

    ValueTask ForceStopAsync(CancellationToken cancellationToken);
    RoutingId RoutingId { get; }
    MeshOperationId AllocateOperationId();
    long MaxMessageSize { get; set; }
    ulong RouterHighWaterMark { get; set; }
    ulong MailboxMessageBudget { get; set; }
    ulong MailboxByteBudget { get; set; }
    TimeSpan? SendTimeout { get; set; }
    void SetRoutingId(RoutingId routingId);
    void SetObjectRole(ZLinkMeshNodeObjectRole objectRole);
    void SetBind(string endpoint);
    void SetAdvertisedEndpoint(string endpoint);
    void Start();
    ulong ConnectPeer(
        string endpoint,
        RoutingId? expectedRid = null,
        string expectedSecurityIdentity = "none");
    void SetPeerExpectation(
        RoutingId peerRid,
        string endpoint,
        string expectedSecurityIdentity,
        ulong expectedLifecycleGeneration);
    void RemovePeerExpectation(RoutingId peerRid, string endpoint);
    void RemovePeerConnection(ulong connectionIntentId);
    bool RemovePeerConnectionIfNotAdmitted(ulong connectionIntentId);
    void DisconnectPeer(RoutingId peerRid, ulong lifecycleGeneration = 0);
    void AddChannel(string channelName);
    void SetChannelWeight(string channelName, uint weight);

    void PublishDraining();
    MeshNodeStatus Status();
    MeshNodePeer[] Peers();
    MeshPeerChannel[] PeerChannels(RoutingId peerRid, ulong lifecycleGeneration);
    IMeshNodeMonitor OpenMonitor(MeshMonitorEventMask events = MeshMonitorEventMask.All);
    void SetReadyHandler(Func<MeshReadyDomains, MeshReadyDomains> handler);
    void SetCompletionOverflowHandler(
        Action<MeshReceiveRecord, IReadOnlyList<Message>> handler) { }
    bool DrainReady(MeshReadyDomains domains, MeshReadyBatch batch, RecvFlags flags = RecvFlags.None);
    ISpot CreateSpot();
    ISpot EntrySpot();
    ISpot GetOrCreateSpot(string spotId, out bool created);
    ISpot GetOrCreateReservedSpot(
        string spotId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        out bool created);
    ActorRef CreateActor(string actorId, IReadOnlyList<Message>? creationParts = null, TimeSpan timeout = default);
    ActorRef CreateReservedActor(
        string actorId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        IReadOnlyList<Message>? creationParts = null,
        TimeSpan timeout = default);
    void SetActorAuthority(
        ActorRef actor,
        ulong authorityOwnerGeneration);
    bool ActorLookup(string actorId, out ActorLocation location);
    MeshOperationId DestroyActor(ActorRef actor, TimeSpan timeout = default);
    MeshOperationId JoinSpot(ActorRef actor, RoutingId targetNodeRid, string targetSpotId,
        ulong targetSpotGeneration, IReadOnlyList<Message>? creationParts = null, TimeSpan timeout = default);
    MeshOperationId JoinEntrySpot(ActorRef actor, RoutingId targetNodeRid,
        IReadOnlyList<Message>? creationParts = null, TimeSpan timeout = default);
    SubmitResult SendToNode(RoutingId targetRid, IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None, ReadOnlyMemory<byte> metadata = default);
    SubmitResult RequestToNode(RoutingId targetRid, IReadOnlyList<Message> parts,
        out MeshOperationId operationId, TimeSpan timeout = default,
        SendFlags flags = SendFlags.None, ReadOnlyMemory<byte> metadata = default);
    SubmitResult RequestToNode(RoutingId targetRid, IReadOnlyList<Message> parts,
        RequestCallback callback, TimeSpan timeout = default,
        SendFlags flags = SendFlags.None, ReadOnlyMemory<byte> metadata = default);
    SubmitResult SendToActor(ActorRef actor, IReadOnlyList<Message> parts, SendFlags flags = SendFlags.None);
    SubmitResult RequestToActor(ActorRef actor, IReadOnlyList<Message> parts,
        out MeshOperationId operationId, TimeSpan timeout = default);
    SubmitResult SendBoundSession(ActorRef actor, IReadOnlyList<Message> parts, SendFlags flags = SendFlags.None);
    MeshOperationId CloseBoundSession(ActorRef actor, ulong expectedBindingGeneration, TimeSpan timeout = default);
    void SetUserSpotOperationTarget(IUserSpotOperationTarget target);
    void SetActorCreateOperationTarget(IActorCreateOperationTarget target);
    void SetActorDestroyOperationTarget(IActorDestroyOperationTarget target);
    void SetActorMessageFollowIngressTarget(
        IActorMessageFollowIngressTarget target);
    void SetInstanceSpotActivationTarget(IInstanceSpotActivationTarget target);
    void SetRelocationReplyRelayTarget(IRelocationReplyRelayTarget target);
    void SetCanonicalRelocationReservationTarget(
        ICanonicalRelocationReservationTarget target);
    ValueTask<ZLinkServiceWireCodec.ReplyRelayAckRecord> RelayRelocationReplyAsync(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        ZLinkServiceWireCodec.RequestSourceFence expectedSource,
        IReadOnlyList<Message> payload,
        TimeSpan timeout,
        CancellationToken cancellationToken);
    ValueTask<ZLinkServiceWireCodec.RelocationReservedRecord>
        ReserveCanonicalRelocationAsync(
            RoutingId targetNodeRid,
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
            TimeSpan timeout,
            CancellationToken cancellationToken);
    ValueTask StageCanonicalRelocationAsync(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
        IReadOnlyList<ZLinkServiceWireCodec.RelocationDataRecord> data,
        TimeSpan timeout,
        CancellationToken cancellationToken);
    ValueTask CompleteCanonicalRelocationAsync(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.RelocationCompleteRecord complete,
        CancellationToken cancellationToken);
    SubmitResult ActivateInstanceSpot(
        InstanceSpotActivationTarget target,
        string sourceSpotId,
        IReadOnlyList<Message> parts,
        bool request,
        out MeshOperationId operationId,
        ulong deadlineUnixMs,
        TimeSpan timeout = default,
        SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default);
    SubmitResult CreateUserSpot(
        RoutingId targetNodeRid,
        string spotId,
        string stableType,
        ObjectReservationFence reservation,
        ulong deadlineUnixMs,
        out MeshOperationId operationId,
        TimeSpan timeout = default);
    SubmitResult CloseUserSpot(
        RoutingId targetNodeRid,
        UserSpotCloseFence target,
        ulong deadlineUnixMs,
        out MeshOperationId operationId,
        TimeSpan timeout = default);
    SubmitResult CreateActorRemote(
        RoutingId targetNodeRid,
        string actorId,
        string stableType,
        ObjectReservationFence reservation,
        ulong deadlineUnixMs,
        out MeshOperationId operationId,
        TimeSpan timeout = default);
    SubmitResult DestroyActorRemote(
        ActorRef actor,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        out MeshOperationId operationId,
        TimeSpan timeout = default);
    IStreamSessionService CreateStreamSessionService(IStreamSocket stream);
}

internal interface ISpot : IDisposable, IAsyncDisposable
{
    RoutingId RoutingId { get; }
    ulong LifecycleGeneration { get; }
    void SetRoutingId(RoutingId routingId);
    SpotStatus Status();
    void SetSubscription(string channelName, string topic);
    SubmitResult SendToChannel(string channelName, IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None, ReadOnlyMemory<byte> metadata = default);
    SubmitResult RequestToChannel(string channelName, IReadOnlyList<Message> parts,
        out MeshOperationId operationId, TimeSpan timeout = default,
        SendFlags flags = SendFlags.None, ReadOnlyMemory<byte> metadata = default);
    SubmitResult RequestToChannel(string channelName, IReadOnlyList<Message> parts,
        RequestCallback callback, TimeSpan timeout = default,
        SendFlags flags = SendFlags.None, ReadOnlyMemory<byte> metadata = default);
    void Publish(string channelName, string topic,
        IReadOnlyList<Message> parts, SendFlags flags = SendFlags.None,
        ReadOnlyMemory<byte> metadata = default);
    SubmitResult SendToSpot(RoutingId targetNodeRid, string targetSpotId,
        ulong targetSpotGeneration, IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None, ReadOnlyMemory<byte> metadata = default);
    SubmitResult RequestToSpot(RoutingId targetNodeRid, string targetSpotId,
        ulong targetSpotGeneration, IReadOnlyList<Message> parts,
        out MeshOperationId operationId, TimeSpan timeout = default,
        SendFlags flags = SendFlags.None, ReadOnlyMemory<byte> metadata = default);
}

internal readonly record struct SpotStatus(ulong LifecycleGeneration);

internal sealed record StreamSessionBinding(
    RoutingId SessionRid, ActorRef Actor, ulong BindingGeneration,
    ulong MembershipEpoch);

internal interface IStreamSessionService : IDisposable, IAsyncDisposable
{
    void Start();
    SubmitResult BindActor(RoutingId sessionRid, ActorRef actor,
        out MeshOperationId operationId, TimeSpan timeout = default);
    SubmitResult UnbindActor(RoutingId sessionRid, ActorRef actor,
        ulong expectedBindingGeneration, out MeshOperationId operationId,
        TimeSpan timeout = default);
    StreamSessionBinding[] Bindings(RoutingId sessionRid);
    SubmitResult SendToActor(RoutingId sessionRid, ActorRef actor,
        IReadOnlyList<Message> parts, SendFlags flags = SendFlags.None);
}
