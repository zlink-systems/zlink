namespace Zlink.Framework.Runtime.Backend.Contracts;

internal interface IZLinkBackendSpotNode : IAsyncDisposable
{
    void SetInboundDispatchBudget(ZLinkInboundDispatchBudget budget) { }

    ValueTask ForceStopAsync(CancellationToken cancellationToken) => DisposeAsync();

    RoutingId RoutingId { get; }

    void SetRoutingId(RoutingId routingId);

    void SetObjectRole(ZLinkMeshNodeObjectRole objectRole);

    void SetPublisherRoutingId(RoutingId routingId);

    void SetSubscriberRoutingId(RoutingId routingId);

    void SetRouterBind(string endpoint);

    void SetRouterAdvertisedEndpoint(string endpoint);

    void SetPubBind(string endpoint);

    // Adds a logical channel membership. Per spec 21-mesh-node §3 and the
    // IMeshNode contract, routing id + bind + at least one channel must be
    // configured before Start; call this before Start.
    void AddChannel(string channelName);

    // Sets a channel's load-balancing weight. Startup wiring calls this before
    // Start; the live runtime path (IZLinkRouteMeshRuntimeOptions, S8-02A) reuses
    // it after start (descriptor-revision bump, spec 21 §4).
    void SetChannelWeight(string channelName, uint weight);

    void PublishDraining();

    // Applies the Core MeshNode receive cap. Framework 0 means no limit and
    // is translated by the backend to Core's -1 sentinel.
    void SetMaxMessageSize(long value);

    void SetRouterHighWaterMark(ulong value);

    void SetRouterSendTimeout(TimeSpan? value);

    void SetMailboxBudgets(ulong messageBudget, ulong byteBudget);

    // Starts the node explicitly at the host-startup point after routing id,
    // bind and channels are applied (spec 21 §3). Idempotent.
    void Start();

    void ApplyRoleConfig(
        IZLinkSpotPublisherConfig? publisher,
        IZLinkSpotSubscriberConfig? subscriber);

    void OnSendReady(Action handler);

    void ConnectPeer(string endpoint);

    void ConnectPeer(
        RoutingId peerRid,
        string endpoint,
        string expectedSecurityIdentity = "none");

    void SetPeerExpectation(
        RoutingId peerRid,
        string endpoint,
        string expectedSecurityIdentity,
        ulong expectedLifecycleGeneration);

    void RemovePeerExpectation(RoutingId peerRid, string endpoint);

    void DisconnectPeer(string endpoint);

    // Removes a matching peer only while admission has not completed. This is
    // used when discovery removes a non-initiating target; an already admitted
    // transport remains subject to its own liveness and lifecycle rules.
    bool DisconnectPeerBeforeAdmission(
        RoutingId peerRid,
        string endpoint,
        ulong lifecycleGeneration)
    {
        return false;
    }

    // Retires an admitted peer lifetime by (RID, lifecycle generation). Core
    // queues a successor admission of the same RID behind this explicit
    // predecessor disconnect on every member that admitted the old lifetime.
    void DisconnectPeerLifetime(RoutingId peerRid, ulong lifecycleGeneration);

    IZLinkBackendSpot CreateSpot();

    IZLinkBackendSpot GetOrCreateSpot(string spotId, out bool created);

    IZLinkBackendSpot GetOrCreateReservedSpot(
        string spotId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        out bool created);

    void SetLocalActorAuthority(
        ZLinkBackendActorRef actor,
        ulong authorityOwnerGeneration);

    ZLinkSpotNodeStatus Status();

    IReadOnlyList<ZLinkSpotNodePeerEntry> Peers();

    // Raw MeshNode status/peer snapshots (rid, generations, admission state)
    // backing the IZLinkRouteMeshRuntime monitoring surface (spec 50 §2).
    MeshNodeStatus MeshStatus();

    MeshOperationId AllocateOperationId();

    IReadOnlyList<MeshNodePeer> MeshPeers();

    IReadOnlyList<MeshPeerChannel> MeshPeerChannels(
        RoutingId peerRid,
        ulong lifecycleGeneration);

    IMeshNodeMonitor OpenMeshMonitor(
        MeshMonitorEventMask events = MeshMonitorEventMask.All);

    IReadOnlyList<ZLinkSpotNodeSubjectEntry> Subjects();

    IZLinkBackendSpot EntrySpot();

    // Node-addressed one-way send on the router plane (NodeSend record on the
    // target). Carries framework-internal packets such as the remote-session
    // push relay; the target's node route dispatcher decodes the envelope.
    SubmitResult SendToNode(
        RoutingId targetNodeRid,
        IReadOnlyList<Message> parts,
        SendFlags flags);

    SubmitResult SendToNode(
        RoutingId targetNodeRid,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata)
    {
        if (!metadata.IsEmpty)
            throw new NotSupportedException("This MeshNode backend does not support node metadata.");
        return SendToNode(targetNodeRid, parts, flags);
    }

    bool RequestToNode(
        RoutingId targetNodeRid,
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan timeout,
        ReadOnlyMemory<byte> metadata = default)
    {
        throw new NotSupportedException("This MeshNode backend does not support node requests.");
    }

    ZLinkBackendActorRef CreateActor(string actorId, Message createRequest);

    ZLinkBackendActorRef CreateReservedActor(
        string actorId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        Message createRequest) =>
        throw new NotSupportedException(
            "This MeshNode backend does not support reservation-fenced Actor creation.");

    ZLinkBackendActorRef? ActorLookup(string actorId);

    bool JoinActor(
        ZLinkBackendActorRef actor,
        RoutingId destNodeRid,
        string destSpotId,
        Message message,
        RequestCallback callback,
        TimeSpan? timeout);

    bool JoinActor(
        ZLinkBackendActorRef actor,
        RoutingId destNodeRid,
        string destSpotId,
        IReadOnlyList<Message> parts,
        ActorJoinCallback callback,
        TimeSpan? timeout);

    bool JoinActorEntrySpot(
        ZLinkBackendActorRef actor,
        RoutingId destNodeRid,
        Message request,
        ActorJoinEntrySpotCallback callback,
        TimeSpan? timeout);

    ValueTask DestroyActorAsync(
        ZLinkBackendActorRef actor,
        TimeSpan timeout,
        CancellationToken cancellationToken);

    bool SendActorBoundSession(
        ZLinkBackendActorRef actor,
        IReadOnlyList<Message> parts,
        SendFlags flags);

    SubmitResult SendToActor(
        ZLinkBackendActorRef actor,
        IReadOnlyList<Message> parts,
        SendFlags flags);

    ValueTask<IReadOnlyList<Message>> RequestToActorAsync(
        ZLinkBackendActorRef actor,
        IReadOnlyList<Message> parts,
        TimeSpan? timeout,
        CancellationToken cancellationToken);

    bool ReplyActorNoBind(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ulong requestId,
        uint flags,
        IReadOnlyList<Message> parts);

    bool ForwardActorBoundSessionPart(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Message message,
        bool hasMore,
        SendFlags flags);

    void CloseActorBoundSession(
        ZLinkBackendActorRef actor,
        TimeSpan timeout,
        CancellationToken cancellationToken);

    // Framework service command 47/48 target. Startup installs the production
    // Spot catalog + Location Store adapter before the MeshNode begins
    // receiving traffic.
    void SetUserSpotOperationTarget(IUserSpotOperationTarget target) =>
        throw new NotSupportedException(
            "This MeshNode backend does not support User Spot service operations.");

    void SetActorCreateOperationTarget(IActorCreateOperationTarget target) =>
        throw new NotSupportedException(
            "This MeshNode backend does not support Actor create service operations.");

    void SetActorDestroyOperationTarget(IActorDestroyOperationTarget target) =>
        throw new NotSupportedException(
            "This MeshNode backend does not support Actor destroy service operations.");

    void SetInstanceSpotActivationTarget(IInstanceSpotActivationTarget target) =>
        throw new NotSupportedException(
            "This MeshNode backend does not support Instance Spot activation.");

    ValueTask<IReadOnlyList<Message>> ActivateInstanceSpotAsync(
        InstanceSpotActivationTarget target,
        string sourceSpotId,
        IReadOnlyList<Message> parts,
        bool request,
        ulong deadlineUnixMs,
        TimeSpan timeout,
        ReadOnlyMemory<byte> metadata,
        CancellationToken cancellationToken) =>
        throw new NotSupportedException(
            "This MeshNode backend does not support Instance Spot activation.");

    ValueTask<InstanceSpotActivationTerminal> ForwardInstanceSpotActivationAsync(
        InstanceSpotActivationOperation operation,
        IReadOnlyList<ReadOnlyMemory<byte>> parts,
        ReadOnlyMemory<byte>? metadata,
        CancellationToken cancellationToken) =>
        throw new NotSupportedException(
            "This MeshNode backend does not support Instance Spot activation forwarding.");

    ValueTask<(UserSpotCreateCompletion Completion, IReadOnlyList<Message> Reply)>
        CreateUserSpotAsync(
            RoutingId targetNodeRid,
            string spotId,
            string stableType,
            ObjectReservationFence reservation,
            ulong deadlineUnixMs,
            TimeSpan timeout,
            CancellationToken cancellationToken) =>
        throw new NotSupportedException();

    ValueTask<(ActorCreateCompletion Completion, IReadOnlyList<Message> Reply)>
        CreateActorRemoteAsync(
            RoutingId targetNodeRid,
            string actorId,
            string stableType,
            ObjectReservationFence reservation,
            ulong deadlineUnixMs,
            TimeSpan timeout,
            CancellationToken cancellationToken) =>
        throw new NotSupportedException();

    ValueTask<UserSpotCloseCompletion> CloseUserSpotAsync(
        RoutingId targetNodeRid,
        UserSpotCloseFence target,
        ulong deadlineUnixMs,
        TimeSpan timeout,
        CancellationToken cancellationToken) =>
        throw new NotSupportedException();

    ValueTask<bool> DestroyActorRemoteAsync(
        ZLinkBackendActorRef actor,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        TimeSpan timeout,
        CancellationToken cancellationToken) =>
        throw new NotSupportedException();

    // Registers the handler the node dispatch pump invokes for node-addressed
    // (NodeSend/NodeRequest) and channel-addressed (ChannelSend/ChannelRequest)
    // records, so the MeshNode builder's registered route/channel handlers receiver
    // inbound traffic. Requests reply through the record's held reply token.
    void OnNodeRoute(Action<ZLinkBackendRouteReceived> handler);
}

internal interface IZLinkBackendRelocationReplyRelay
{
    void SetRelocationReplyRelayTarget(IRelocationReplyRelayTarget target);

    ZLinkRelocationReplyCompletion TryCompleteRelocationReply(
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        IReadOnlyList<Message> payload);

    ValueTask<ZLinkServiceWireCodec.ReplyRelayAckRecord> RelayRelocationReplyAsync(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        ZLinkServiceWireCodec.RequestSourceFence expectedSource,
        IReadOnlyList<Message> payload,
        TimeSpan timeout,
        CancellationToken cancellationToken);
}

internal interface IZLinkBackendCanonicalRelocationReservation
{
    void SetCanonicalRelocationReservationTarget(
        ICanonicalRelocationReservationTarget target);

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

    void CancelCanonicalRelocation(
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.RelocationWireId relocationId,
        ulong targetAttemptGeneration,
        ZLinkServiceWireCodec.RelocationCoordinatorFence coordinator);
}

internal interface IZLinkBackendSpot : IAsyncDisposable
{
    RoutingId RoutingId { get; }

    // Core lifecycle generation of this spot activation — the value peers
    // must present on spot-addressed submits, published through the spot's
    // location row.
    ulong LifecycleGeneration { get; }

    void SetRoutingId(RoutingId routingId);

    void SetSubscription(string channelName, string topic);

    ZLinkBackendSubscribeMessage? Subscribe(RecvFlags flags);

    ZLinkBackendRouteReceived? RecvRoute(RecvFlags flags);

    void OnDispatchEvent(Action<ZLinkBackendSpotDispatchInfo> handler);

    void OnSendReady(Action handler);

    //  Submit surfaces return the binding SubmitResult (not a flattened bool)
    //  so the exact call contract can report Backpressured, TargetNotFound and
    //  RouteNotConnected distinctly; `metadata` is the canonical application
    //  metadata frame (05-route-mesh §6), empty when the call set none.
    bool RequestToChannel(
        string channelName,
        Message message,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout,
        ReadOnlyMemory<byte> metadata);

    bool RequestToChannel(
        string channelName,
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout,
        ReadOnlyMemory<byte> metadata);

    SubmitResult SendToChannel(
        string channelName,
        Message message,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata);

    SubmitResult SendToChannel(
        string channelName,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata);

    void Publish(
        string channelName,
        string topic,
        Message message,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata);

    void Publish(
        string channelName,
        string topic,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata);

    //  `spotGeneration` is the target Spot lifecycle generation from the
    //  resolved handle snapshot; the Core contract rejects 0 (03-spot §5).
    SubmitResult SendToSpot(
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        Message message,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata);

    SubmitResult SendToSpot(
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata);

    bool RequestToSpot(
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        Message message,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout,
        ReadOnlyMemory<byte> metadata);

    bool RequestToSpot(
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout,
        ReadOnlyMemory<byte> metadata);

    ZLinkBackendActorJoinRequest? RecvActorJoin(RecvFlags flags);

    ZLinkBackendSpotActorLifecycleEvent? RecvActorLifecycle(RecvFlags flags);

    void ReplyActorJoin(
        ZLinkBackendActorJoinRequest request,
        int joinResultCode,
        Message reply);

    void ReplyActorJoin(
        ZLinkBackendActorJoinRequest request,
        int joinResultCode,
        IReadOnlyList<Message> parts);
}

internal interface IZLinkBackendSpotMessageFollower
{
    SubmitResult MessageFollowSendToSpot(
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        MeshOperationId operationId,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        byte messageFollowHopCount,
        IReadOnlyList<Message> parts,
        SendFlags flags,
        ReadOnlyMemory<byte> metadata);

    bool MessageFollowRequestToSpot(
        RoutingId targetRid,
        string spotId,
        ulong spotGeneration,
        MeshOperationId operationId,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        byte messageFollowHopCount,
        ulong deadlineUnixMs,
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan? timeout,
        ReadOnlyMemory<byte> metadata);
}
