using Zlink.Framework.Runtime.Backend.DotNet.Mappings;
using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.Runtime.Host;

internal sealed class ZLinkActorBoundSessionCoordinator
{
    private const int MaxConcurrentOutboundProofs = 4_096;
    private readonly ZLinkActorBoundSessionRegistry _boundSessions;
    private readonly ZLinkSessionActorBindingTable _sessionBindings;
    private readonly ZLinkRemoteRelayFrameAssembler _remoteFrames;
    private readonly ZLinkBoundedRemoteRequestAdmission _remoteRequestAdmission = new();
    private readonly Dictionary<RemoteRequestKey, PendingRemoteRequest>
        _pendingRemoteRequests = new();
    private readonly object _outboundProofGate = new();
    private readonly Dictionary<ZLinkSessionOutboundTenure,
        PendingOutboundProof> _pendingOutboundProofs = new();
    private readonly Func<string, ZLinkActorRuntimeState> _getState;
    private readonly Func<IZLinkBackendSpotNode?> _getNode;
    private readonly Func<string, IZLinkBackendSpotNode?> _getNodeForMesh;
    private readonly ZLinkFrameworkRegistration _registration;
    private readonly Func<CancellationToken> _getShutdownToken;
    private readonly Func<IZLinkLocationRepository?> _resolveAuthorityStore;
    private CancellationTokenSource _canonicalRouteApplicationCancellation;
    private long _applicationEpoch = 1;
    private bool _applicationEpochOpen = true;
    private long _bindingGeneration;

    public ZLinkActorBoundSessionCoordinator(
        Func<string, ZLinkActorRuntimeState> getState,
        Func<IZLinkBackendSpotNode?> getNode,
        Func<string, IZLinkBackendSpotNode?> getNodeForMesh,
        ZLinkFrameworkRegistration registration,
        Func<CancellationToken> getShutdownToken,
        Func<IZLinkLocationRepository?>? resolveAuthorityStore = null)
    {
        _getState = getState;
        _getNode = getNode;
        _getNodeForMesh = getNodeForMesh;
        _registration = registration;
        _getShutdownToken = getShutdownToken;
        _resolveAuthorityStore = resolveAuthorityStore
                                 ?? registration.Locations.ResolveStore;
        _canonicalRouteApplicationCancellation = new CancellationTokenSource();
        _sessionBindings = new ZLinkSessionActorBindingTable(
            registration.DefaultRequestTimeout + registration.DefaultRequestTimeout,
            registration.Locations.SessionRelocationSealTimeoutAtStartup);
        _boundSessions = new ZLinkActorBoundSessionRegistry(UnbindActorSession);
        _remoteFrames = new ZLinkRemoteRelayFrameAssembler(
            registration.DefaultRequestTimeout,
            getShutdownToken);
    }

    /// <summary>Set by the runtime after construction: relays an encoded push
    /// frame to a remote session node as (actorId, sessionNodeRid, sessionRid,
    /// frame). Core's bound-session send is node-local, so a session bound on
    /// another node is reached through this framework-internal route packet
    /// (spec 31 §6 keeps the session route inside the framework).</summary>
    public Func<string, ZLinkActorBoundSession, byte[], bool>? RemotePushRelay { get; set; }

    public Func<string, ZLinkActorBoundSession, byte[], CancellationToken,
        ValueTask<ZLinkOneWaySubmitResult>>? RemotePushRelayAsync { get; set; }

    /// <summary>Set by the runtime after construction: relays a session frame
    /// to a bound actor that migrated to another node as (actorRef,
    /// sessionNodeRid, sessionRid, headerBytes, bodyBytes).</summary>
    public Func<string?, ZLinkBackendActorRef, ulong, ulong, ulong,
        RoutingId, RoutingId, ZLinkBackendActorRouteContext,
        ulong, ZLinkServiceWireCodec.RequestSourceFence?,
        ReadOnlyMemory<byte>, byte[], byte[], bool>? RemoteFrameRelay { get; set; }

    public enum RemotePushDelivery
    {
        Delivered,
        Retained,
        Backpressured,
        NoBinding,
        WrongSession
    }

    internal async ValueTask<RemotePushDelivery> AdmitRemoteSessionFrameAsync(
        ZLinkRemoteSessionPushRelay identity,
        byte[] frame,
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken)
    {
        var admission = await PrepareRemoteSessionFrameAdmissionAsync(
                identity,
                frame,
                sourceNodeRid,
                cancellationToken)
            .ConfigureAwait(false);
        return await CompleteOutboundAdmissionAsync(
                admission,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask<RemotePushDelivery>
        AdmitRemoteSessionFrameOneWayAsync(
            ZLinkRemoteSessionPushRelay identity,
            byte[] frame,
            RoutingId sourceNodeRid,
            CancellationToken cancellationToken)
    {
        var admission = await PrepareRemoteSessionFrameAdmissionAsync(
                identity,
                frame,
                sourceNodeRid,
                cancellationToken)
            .ConfigureAwait(false);
        if (admission.Kind == ZLinkSessionOutboundAdmissionKind.Retained)
            return RemotePushDelivery.Retained;
        return await CompleteOutboundAdmissionAsync(
                admission,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkSessionOutboundAdmission>
        PrepareRemoteSessionFrameAdmissionAsync(
            ZLinkRemoteSessionPushRelay identity,
            byte[] frame,
            RoutingId sourceNodeRid,
            CancellationToken cancellationToken)
    {
        var targetNodeRid = RoutingId.FromHex(identity.TargetNodeRid);
        if (sourceNodeRid != targetNodeRid)
            return new ZLinkSessionOutboundAdmission(
                ZLinkSessionOutboundAdmissionKind.WrongSession);
        var sessionRid = RoutingId.FromHex(identity.SessionRid);
        var tenure = new ZLinkSessionOutboundTenure(
            identity.ActorId,
            identity.ObjectGeneration,
            identity.MeshName,
            targetNodeRid,
            identity.TargetNodeGeneration,
            identity.AuthorityOwnerGeneration,
            identity.OwnerLeaseGeneration,
            identity.BindingToken,
            identity.BindingGeneration,
            identity.SessionOwnerNodeGeneration,
            sessionRid);
        var admission = _sessionBindings.AdmitOutbound(
            tenure,
            firstProof: null,
            frame);
        if (admission.Kind == ZLinkSessionOutboundAdmissionKind.ProofRequired)
        {
            var proofLease = await ResolveOutboundProofAsync(
                    tenure,
                    cancellationToken)
                .ConfigureAwait(false);
            await proofLease.Pending.WaitForTurnAsync(proofLease.Ticket)
                .ConfigureAwait(false);
            try
            {
                admission = _sessionBindings.AdmitOutbound(
                    tenure,
                    proofLease.Proof,
                    frame);
            }
            finally
            {
                ReleaseOutboundProofLease(proofLease);
            }
        }
        return admission;
    }

    private async ValueTask<OutboundProofLease>
        ResolveOutboundProofAsync(
            ZLinkSessionOutboundTenure tenure,
            CancellationToken cancellationToken)
    {
        if (_sessionBindings.TryGetMemoizedOutboundProof(
                tenure,
                out var memoized))
            return new OutboundProofLease(
                memoized,
                PendingOutboundProof.Completed,
                0);

        PendingOutboundProof pending;
        var ownsResolution = false;
        long ticket;
        lock (_outboundProofGate)
        {
            if (!_applicationEpochOpen)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "Session outbound proof generation is resetting.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            if (_pendingOutboundProofs.TryGetValue(tenure, out pending!))
            {
                if (pending.Epoch != _applicationEpoch)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "Session outbound proof belongs to a closed generation.",
                        ZLinkRetryAdvice.RetryAfterBackoff);
            }
            else
            {
                if (_pendingOutboundProofs.Count >= MaxConcurrentOutboundProofs)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Rejected,
                        "Concurrent session outbound proof capacity is exhausted.",
                        ZLinkRetryAdvice.RetryAfterBackoff);
                pending = new PendingOutboundProof(
                    _applicationEpoch,
                    _canonicalRouteApplicationCancellation.Token);
                _pendingOutboundProofs.Add(tenure, pending);
                ownsResolution = true;
            }
            ticket = pending.RegisterTicket();
        }

        if (ownsResolution)
            await ResolveOutboundProofOwnerAsync(tenure, pending)
                .ConfigureAwait(false);
        var proof = await pending.Completion.Task.ConfigureAwait(false);
        return new OutboundProofLease(proof, pending, ticket);
    }

    private void ReleaseOutboundProofLease(OutboundProofLease lease)
    {
        if (ReferenceEquals(lease.Pending, PendingOutboundProof.Completed))
            return;
        lease.Pending.CompleteTurn(lease.Ticket);
        lock (_outboundProofGate)
        {
            if (lease.Pending.IsDrained
                && _pendingOutboundProofs.TryGetValue(
                    lease.Proof.Tenure,
                    out var current)
                && ReferenceEquals(current, lease.Pending))
                _pendingOutboundProofs.Remove(lease.Proof.Tenure);
        }
    }

    private async ValueTask ResolveOutboundProofOwnerAsync(
        ZLinkSessionOutboundTenure tenure,
        PendingOutboundProof pending)
    {
        try
        {
            var store = _resolveAuthorityStore()
                        ?? throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Unavailable,
                            "Remote session outbound tenure proof requires a Location Store.",
                            ZLinkRetryAdvice.RetryAfterBackoff);
            var authorityRead = await store.ReadAuthorityAsync(
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                        tenure.ActorId),
                    pending.CancellationToken)
                .ConfigureAwait(false);
            if (authorityRead is not ZLinkAuthorityReadResult.Found found)
                throw new InvalidDataException(
                    "Session outbound tenure does not match current Actor authority.");
            var actorAuthorityPayload = found.Snapshot.Payload;
            if (ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    actorAuthorityPayload.Span,
                    out var canonical))
                actorAuthorityPayload = canonical.SteadyAuthorityPayload;
            if (found.Snapshot.ObjectGeneration
                   != tenure.ObjectGeneration
                || found.Snapshot.AuthorityOwnerGeneration
                   != tenure.AuthorityOwnerGeneration
                || found.Snapshot.OwnerLeaseGeneration <= 0
                || checked((ulong)found.Snapshot.OwnerLeaseGeneration)
                   != tenure.OwnerLeaseGeneration
                || found.Snapshot.Allocation.ObjectKind
                   != ZLinkPlacementObjectKind.Actor
                || found.Snapshot.Allocation.Descriptor.Rid
                   != tenure.TargetNodeRid
                || !string.Equals(
                    found.Snapshot.Allocation.Descriptor.MeshName,
                    tenure.MeshName,
                    StringComparison.Ordinal)
                || found.Snapshot.Allocation.DescriptorLifecycleGeneration
                   != tenure.TargetNodeGeneration
                || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                    actorAuthorityPayload.Span,
                    out var authority)
                || authority.State != ZLinkActorAuthorityState.Ready
                || !string.Equals(
                    authority.ActorId,
                    tenure.ActorId,
                    StringComparison.Ordinal)
                || !string.Equals(
                    authority.OwnerId,
                    found.Snapshot.OwnerId,
                    StringComparison.Ordinal)
                || authority.OwnerLeaseGeneration
                   != tenure.OwnerLeaseGeneration
                || !string.Equals(
                    authority.MeshName,
                    tenure.MeshName,
                    StringComparison.Ordinal)
                || authority.NodeRid != tenure.TargetNodeRid
                || authority.NodeGeneration
                   != tenure.TargetNodeGeneration)
                throw new InvalidDataException(
                    "Session outbound tenure does not match current Actor authority.");
            var proof = new ZLinkSessionOutboundTenureProof(
                tenure,
                found.Snapshot.OwnerId);

            lock (_outboundProofGate)
            {
                if (pending.Retired
                    || !_applicationEpochOpen
                    || pending.Epoch != _applicationEpoch
                    || !_pendingOutboundProofs.TryGetValue(
                        tenure,
                        out var current)
                    || !ReferenceEquals(current, pending))
                    return;
                pending.Completion.TrySetResult(proof);
            }
        }
        catch (Exception exception)
        {
            lock (_outboundProofGate)
            {
                if (pending.Retired)
                    return;
                if (_pendingOutboundProofs.TryGetValue(
                        tenure,
                        out var current)
                    && ReferenceEquals(current, pending))
                    _pendingOutboundProofs.Remove(tenure);
                pending.Completion.TrySetException(exception);
            }
        }
    }

    private static async ValueTask<RemotePushDelivery>
        CompleteOutboundAdmissionAsync(
            ZLinkSessionOutboundAdmission admission,
            CancellationToken cancellationToken)
    {
        switch (admission.Kind)
        {
            case ZLinkSessionOutboundAdmissionKind.Immediate:
                return MapOutboundDelivery(
                    admission.Capability!.Settle(deliver: true));
            case ZLinkSessionOutboundAdmissionKind.Retained:
                return MapOutboundDelivery(
                    await admission.Capability!.Completion
                        .WaitAsync(cancellationToken)
                        .ConfigureAwait(false));
            case ZLinkSessionOutboundAdmissionKind.Backpressured:
                return RemotePushDelivery.Backpressured;
            case ZLinkSessionOutboundAdmissionKind.NoBinding:
                return RemotePushDelivery.NoBinding;
            default:
                return RemotePushDelivery.WrongSession;
        }
    }

    private static RemotePushDelivery MapOutboundDelivery(
        ZLinkSessionOutboundDelivery delivery) =>
        delivery switch
        {
            ZLinkSessionOutboundDelivery.Delivered
                => RemotePushDelivery.Delivered,
            ZLinkSessionOutboundDelivery.Backpressured
                => RemotePushDelivery.Backpressured,
            _ => RemotePushDelivery.WrongSession
        };

    public bool TryClaimRemoteSessionReply(
        string actorId,
        ulong requestId,
        uint flags,
        string replyCapability,
        RoutingId sourceNodeRid,
        RoutingId responderNodeRid,
        out RemoteReplyClaim claim)
    {
        PendingRemoteRequest? pending;
        RemoteRequestKey pendingKey = default;
        var actorRequestMatch = false;
        var capabilityMatch = false;
        lock (_pendingRemoteRequests)
        {
            pending = null;
            foreach (var entry in _pendingRemoteRequests)
            {
                if (!string.Equals(
                        entry.Key.ActorId,
                        actorId,
                        StringComparison.Ordinal)
                    || entry.Key.RequestId != requestId)
                    continue;
                actorRequestMatch = true;
                if (!string.Equals(
                        entry.Value.ReplyCapability,
                        replyCapability,
                        StringComparison.Ordinal))
                    continue;
                capabilityMatch = true;
                pendingKey = entry.Key;
                pending = entry.Value;
                break;
            }
            var claimed = pending?.Claimed ?? false;
            var flagsMatch = flags == ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind;
            var responderMatch = sourceNodeRid == responderNodeRid;
            var objectMatch = pending is not null
                              && pending.Binding.ObjectGeneration
                                 == pendingKey.ObjectGeneration;
            var bindingMatch = pending is not null
                               && string.Equals(
                                   pending.Binding.BindingToken,
                                   pendingKey.BindingToken,
                                   StringComparison.Ordinal);
            if (pending is null
                || claimed
                || !flagsMatch
                || !responderMatch
                || !objectMatch
                || !bindingMatch)
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"remote_session_reply_claim_refused actor={actorId} "
                    + $"request_id={requestId} pending_count={_pendingRemoteRequests.Count} "
                    + $"actor_request_match={actorRequestMatch} "
                    + $"capability_match={capabilityMatch} claimed={claimed} "
                    + $"flags={flags} flags_match={flagsMatch} "
                    + $"source_node={sourceNodeRid} responder_node={responderNodeRid} "
                    + $"responder_match={responderMatch} object_match={objectMatch} "
                    + $"binding_match={bindingMatch} capability={replyCapability}");
                claim = null!;
                return false;
            }

            // Validation and ownership transfer are one atomic step. Keep the
            // claimed entry indexed until terminal cleanup so request-id reuse
            // cannot overtake the reply that owns the completion.
            pending.Claimed = true;
        }

        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"remote_session_reply_claimed actor={actorId} "
            + $"request_id={requestId} object={pendingKey.ObjectGeneration} "
            + $"binding={pendingKey.BindingToken}");

        claim = new RemoteReplyClaim(
            frame => DeliverClaimedRemoteSessionReply(
                actorId,
                pending.Binding,
                frame),
            () => CompleteRemoteSessionRequest(
                pendingKey,
                pending,
                allowClaimed: true));
        return true;
    }

    private RemotePushDelivery DeliverClaimedRemoteSessionReply(
        string actorId,
        ZLinkSessionBindingEntry expected,
        byte[] frame)
    {
        if (!_sessionBindings.TryGet(
                actorId,
                expected.BindingToken,
                out ZLinkSessionBindingEntry entry))
            return _sessionBindings.TryGetByActorId(actorId, out _)
                ? RemotePushDelivery.WrongSession
                : RemotePushDelivery.NoBinding;
        if (!ReferenceEquals(entry.Context, expected.Context)
            || !ReferenceEquals(entry.ActorRef, expected.ActorRef)
            || entry.ObjectGeneration != expected.ObjectGeneration
            || entry.BindingGeneration != expected.BindingGeneration
            || entry.SessionOwnerNodeRid != expected.SessionOwnerNodeRid
            || entry.SessionOwnerNodeGeneration != expected.SessionOwnerNodeGeneration
            || !string.Equals(
                entry.SessionOwnerId,
                expected.SessionOwnerId,
                StringComparison.Ordinal)
            || entry.SessionOwnerLeaseGeneration
            != expected.SessionOwnerLeaseGeneration
            || entry.Context.RoutingId != expected.Context.RoutingId)
            return RemotePushDelivery.WrongSession;

        using var message = Message.From(frame);
        return entry.Context.Write(message)
            ? RemotePushDelivery.Delivered
            : RemotePushDelivery.Backpressured;
    }

    public ulong NextBindingGeneration()
        => checked((ulong)Interlocked.Increment(ref _bindingGeneration));

    public ZLinkSessionBindingEntry[] BindSessionActor(
        string actorId,
        ZLinkSessionContext context,
        string bindingToken,
        ZLinkSessionActor actorRef,
        ulong bindingGeneration,
        ZLinkSessionBindingRoute route,
        ulong sessionOwnerNodeGeneration,
        RoutingId sessionOwnerNodeRid = default,
        string sessionOwnerId = "",
        ulong sessionOwnerLeaseGeneration = 0)
    {
        var actorKey = ZLinkActorId.FromBoundary(actorId, nameof(actorId));
        var replaced = _sessionBindings.Bind(
            actorKey,
            context,
            bindingToken,
            actorRef,
            bindingGeneration,
            route,
            sessionOwnerNodeGeneration,
            sessionOwnerNodeRid,
            sessionOwnerId,
            sessionOwnerLeaseGeneration);
        CompleteReplacedBindingRequests(actorId, replaced);
        return replaced;
    }

    private void CompleteReplacedBindingRequests(
        string actorId,
        IReadOnlyList<ZLinkSessionBindingEntry> replaced)
    {
        if (replaced.Count == 0) return;

        List<PendingRemoteRequest>? stale = null;
        lock (_pendingRemoteRequests)
        {
            var keys = _pendingRemoteRequests
                .Where(entry =>
                    string.Equals(
                        entry.Key.ActorId,
                        actorId,
                        StringComparison.Ordinal)
                    && replaced.Any(binding =>
                        SameBindingIdentity(entry.Value.Binding, binding)))
                .Select(static entry => entry.Key)
                .ToArray();
            foreach (var key in keys)
            {
                var pending = _pendingRemoteRequests[key];
                _pendingRemoteRequests.Remove(key);
                _remoteRequestAdmission.Release(
                    ZLinkSessionBindingKey.FromBoundary(
                        actorId,
                        pending.Binding.BindingToken));
                (stale ??= []).Add(pending);
            }
        }

        if (stale is null) return;
        foreach (var pending in stale)
            pending.Dispose();
    }

    private static bool SameBindingIdentity(
        ZLinkSessionBindingEntry left,
        ZLinkSessionBindingEntry right) =>
        string.Equals(
            left.BindingToken,
            right.BindingToken,
            StringComparison.Ordinal)
        && left.BindingGeneration == right.BindingGeneration
        && left.ObjectGeneration == right.ObjectGeneration
        && left.SessionOwnerNodeGeneration == right.SessionOwnerNodeGeneration;

    public bool TryAcceptSessionFrame(
        string actorId,
        string bindingToken,
        out ulong acceptedHighWater)
        => _sessionBindings.TryAccept(
            actorId,
            bindingToken,
            out acceptedHighWater);

    public ValueTask<bool> WaitForSessionRouteAvailableAsync(
        string actorId,
        string bindingToken,
        CancellationToken cancellationToken)
        => _sessionBindings.WaitForRouteAvailableAsync(
            actorId,
            bindingToken,
            cancellationToken);

    public ZLinkSessionRouteCommitResult CommitSessionRoute(
        ZLinkSessionRouteCommit request)
        => _sessionBindings.CommitRoute(request);

    public ValueTask<ZLinkSessionRouteSealResult> SealSessionRouteAsync(
        ZLinkSessionRouteSeal request,
        CancellationToken cancellationToken)
        => _sessionBindings.SealRouteAsync(request, cancellationToken);

    internal ValueTask<
        ZLinkServiceWireCodec.SessionRelocationSealedRecord>
        SealCanonicalSessionRouteAsync(
            ZLinkServiceWireCodec.SessionRelocationSealRecord request,
            CancellationToken cancellationToken) =>
        _sessionBindings.SealCanonicalRouteAsync(request, cancellationToken);

    internal bool RouteCanonicalSession(
            ZLinkServiceWireCodec.SessionRelocationRouteRecord request,
            ZLinkSessionRelocationAuthenticatedRoute authenticatedRoute)
    {
        lock (_outboundProofGate)
        {
            RequireOpenApplicationEpoch();
            return _sessionBindings.RouteCanonical(request, authenticatedRoute);
        }
    }

    internal async ValueTask
        RouteCanonicalSessionAsync(
            ZLinkServiceWireCodec.SessionRelocationRouteRecord request,
            ZLinkSessionRelocationAuthenticatedRoute authenticatedCandidate,
            CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var authenticated = authenticatedCandidate;
        if (request.Route.Action
                == ZLinkServiceWireCodec.SessionRelocationRouteAction.Commit
            && authenticated.OwnerLeaseGeneration == 0)
        {
            var store = _resolveAuthorityStore()
                        ?? throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Unavailable,
                            "Remote command 44 requires an Authority Store proof.",
                            ZLinkRetryAdvice.RetryAfterBackoff);
            var authorityRead = await store.ReadAuthorityAsync(
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                        request.Actor.ActorId),
                    cancellationToken)
                .ConfigureAwait(false);
            if (authorityRead is not ZLinkAuthorityReadResult.Found found)
                throw new InvalidDataException(
                    "Remote command 44 target authority is unavailable.");
            var actorAuthorityPayload = found.Snapshot.Payload;
            if (ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    actorAuthorityPayload.Span,
                    out var canonical))
                actorAuthorityPayload = canonical.SteadyAuthorityPayload;
            if (found.Snapshot.ObjectGeneration
                    != request.Actor.ObjectGeneration
                || found.Snapshot.AuthorityOwnerGeneration
                   != request.Route.TargetAuthorityOwnerGeneration
                || found.Snapshot.OwnerLeaseGeneration <= 0
                || found.Snapshot.Allocation.ObjectKind
                   != ZLinkPlacementObjectKind.Actor
                || found.Snapshot.Allocation.Descriptor.Rid
                   != authenticated.NodeRid
                || found.Snapshot.Allocation.DescriptorLifecycleGeneration
                   != authenticated.NodeGeneration
                || !string.Equals(
                    found.Snapshot.Allocation.Descriptor.MeshName,
                    authenticated.MeshName,
                    StringComparison.Ordinal)
                || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                    actorAuthorityPayload.Span,
                    out var actorAuthority)
                || actorAuthority.State != ZLinkActorAuthorityState.Ready
                || actorAuthority.NodeRid != authenticated.NodeRid
                || actorAuthority.NodeGeneration != authenticated.NodeGeneration
                || !string.Equals(
                    actorAuthority.OwnerId,
                    found.Snapshot.OwnerId,
                    StringComparison.Ordinal)
                || actorAuthority.OwnerLeaseGeneration
                   != checked((ulong)found.Snapshot.OwnerLeaseGeneration))
                throw new InvalidDataException(
                    "Remote command 44 target authority proof changed.");
            authenticated = authenticated with
            {
                OwnerLeaseGeneration = checked(
                    (ulong)found.Snapshot.OwnerLeaseGeneration)
            };
        }
        if (!RouteCanonicalSession(request, authenticated))
            throw new InvalidDataException(
                "Remote command 44 did not match the active session relocation seal.");
    }

    private void RequireOpenApplicationEpoch()
    {
        if (!_applicationEpochOpen)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Session route application generation is resetting.",
                ZLinkRetryAdvice.RetryAfterBackoff);
    }

    public void CompleteAcceptedSessionFrame(
        string actorId,
        string bindingToken)
        => _sessionBindings.CompleteAccepted(actorId, bindingToken);

    public string TrackRemoteSessionRequest(
        string actorId,
        ulong requestId,
        string bindingToken)
    {
        if (!_sessionBindings.TryGet(
                actorId,
                bindingToken,
                out ZLinkSessionBindingEntry binding))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actorId}' session binding changed before request tracking.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        var pending = new PendingRemoteRequest(
            binding,
            Guid.NewGuid().ToString("N"),
            new CancellationTokenSource());
        var key = new RemoteRequestKey(
            actorId,
            binding.ObjectGeneration,
            binding.BindingToken,
            requestId);
        var admissionKey = ZLinkSessionBindingKey.FromBoundary(
            actorId,
            bindingToken);
        lock (_pendingRemoteRequests)
        {
            if (!_remoteRequestAdmission.TryAcquire(admissionKey))
            {
                pending.Dispose();
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    $"Actor '{actorId}' remote session request capacity is exhausted.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            }
            if (!_pendingRemoteRequests.TryAdd(key, pending))
            {
                _remoteRequestAdmission.Release(admissionKey);
                pending.Dispose();
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ProtocolError,
                    $"Actor '{actorId}' remote session request '{requestId}' is already pending.");
            }
        }
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"remote_session_request_tracked actor={actorId} "
            + $"request_id={requestId} object={binding.ObjectGeneration} "
            + $"binding={binding.BindingToken} capability={pending.ReplyCapability}");
        _ = ExpireRemoteSessionRequestAsync(key, pending);
        return pending.ReplyCapability;
    }

    public void CompleteRemoteSessionRequest(
        string actorId,
        ulong objectGeneration,
        string bindingToken,
        ulong requestId)
        => CompleteRemoteSessionRequest(
            new RemoteRequestKey(
                actorId,
                objectGeneration,
                bindingToken,
                requestId),
            expected: null,
            allowClaimed: true);

    private void CompleteRemoteSessionRequest(
        RemoteRequestKey key,
        PendingRemoteRequest? expected,
        bool allowClaimed)
    {
        PendingRemoteRequest? pending;
        lock (_pendingRemoteRequests)
        {
            if (!_pendingRemoteRequests.TryGetValue(key, out pending)
                || (expected is not null && !ReferenceEquals(pending, expected))
                || (!allowClaimed && pending.Claimed))
                return;
            _pendingRemoteRequests.Remove(key);
            _remoteRequestAdmission.Release(
                ZLinkSessionBindingKey.FromBoundary(
                    key.ActorId,
                    pending.Binding.BindingToken));
        }
        if (pending is not null)
        {
            pending.Dispose();
            _sessionBindings.CompleteAccepted(
                key.ActorId,
                pending.Binding.BindingToken);
        }
    }

    private async Task ExpireRemoteSessionRequestAsync(
        RemoteRequestKey key,
        PendingRemoteRequest pending)
    {
        try
        {
            await Task.Delay(
                    _registration.DefaultRequestTimeout,
                    pending.Cancellation.Token)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) { }
        CompleteRemoteSessionRequest(
            key,
            pending,
            allowClaimed: false);
    }

    public bool AbortSessionRouteSeal(ZLinkSessionRouteSeal request)
        => _sessionBindings.AbortRouteSeal(request);

    public bool UnsealCommittedSessionRoute(ZLinkSessionRouteCommit request)
        => _sessionBindings.UnsealCommittedRoute(request);

    public bool TryGetSessionBinding(
        string actorId,
        string bindingToken,
        out ZLinkSessionBindingEntry entry)
        => _sessionBindings.TryGet(actorId, bindingToken, out entry);

    public bool TryGetSessionRoute(
        string actorId,
        string bindingToken,
        ZLinkSessionActor actorRef,
        out ZLinkSessionBindingRoute route)
        => _sessionBindings.TryGetRoute(
            actorId,
            bindingToken,
            actorRef,
            out route);

    public bool TryGetSessionBindingByActorId(
        string actorId,
        out ZLinkSessionBindingEntry entry) =>
        _sessionBindings.TryGetEntryByActorId(actorId, out entry);

    public bool TryGetExactRetiredSessionBinding(
        string actorId,
        RoutingId sessionOwnerNodeRid,
        RoutingId sessionRid,
        ulong sessionOwnerNodeGeneration,
        string sessionOwnerId,
        ulong sessionOwnerLeaseGeneration,
        ulong bindingGeneration,
        out ZLinkSessionBindingEntry entry) =>
        TryGetExactRetiredSessionBinding(
            ZLinkActorId.FromBoundary(actorId, nameof(actorId)),
            sessionOwnerNodeRid,
            sessionRid,
            sessionOwnerNodeGeneration,
            sessionOwnerId,
            sessionOwnerLeaseGeneration,
            bindingGeneration,
            out entry);

    internal bool TryGetExactRetiredSessionBinding(
        ZLinkActorId actorId,
        RoutingId sessionOwnerNodeRid,
        RoutingId sessionRid,
        ulong sessionOwnerNodeGeneration,
        string sessionOwnerId,
        ulong sessionOwnerLeaseGeneration,
        ulong bindingGeneration,
        out ZLinkSessionBindingEntry entry) =>
        _sessionBindings.TryGetExactRetiredBinding(
            actorId,
            sessionOwnerNodeRid,
            sessionRid,
            sessionOwnerNodeGeneration,
            sessionOwnerId,
            sessionOwnerLeaseGeneration,
            bindingGeneration,
            out entry);

    public IReadOnlyCollection<IZLinkSessionActor> SnapshotSessionActors(
        ZLinkSessionContext context) =>
        _sessionBindings.SnapshotActors(context);

    public ZLinkSessionActor? FindSessionActor(
        ZLinkSessionContext context,
        string actorId) =>
        _sessionBindings.FindActor(context, actorId);

    public void UnbindSessionActor(string actorId, ZLinkSessionContext context, string bindingToken)
    {
        _sessionBindings.Unbind(actorId, context, bindingToken);
    }

    public bool TryGetSessionActorContext(string actorId, string bindingToken, out ZLinkSessionContext context) =>
        _sessionBindings.TryGet(actorId, bindingToken, out context);

    public bool TryGetSessionActorContext(string actorId, out ZLinkSessionContext context) =>
        _sessionBindings.TryGetByActorId(actorId, out context);

    public ZLinkActorBoundSession? BindActorSession(
        string actorId,
        RoutingId? sessionNodeRid,
        RoutingId sessionRid,
        string bindingToken,
        ulong bindingGeneration = 1,
        ulong objectGeneration = 0,
        ulong authorityOwnerGeneration = 0,
        string meshName = "",
        ulong targetNodeGeneration = 1,
        ulong ownerLeaseGeneration = 0,
        ulong sessionOwnerNodeGeneration = 1,
        ulong acceptedHighWater = 0,
        string sessionOwnerId = "",
        ulong sessionOwnerLeaseGeneration = 0)
    {
        //  Recorded at bind time and read again when a relocation seals the
        //  route; printing both ends shows if they disagree.
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"bind_session actor={actorId} session_node={sessionNodeRid} session_rid={sessionRid}");
        var previous = _getState(actorId).BindSession(
            sessionNodeRid,
            sessionRid,
            bindingToken,
            bindingGeneration,
            objectGeneration,
            authorityOwnerGeneration,
            ZLinkMeshName.FromBoundary(meshName, nameof(meshName)),
            targetNodeGeneration,
            ownerLeaseGeneration,
            sessionOwnerNodeGeneration,
            acceptedHighWater,
            sessionOwnerId,
            sessionOwnerLeaseGeneration);
        _boundSessions.Register(actorId, sessionRid, bindingToken);
        return previous;
    }

    public ZLinkActorSessionReplacementAttempt BeginActorSessionReplacement(
        string actorId,
        RoutingId? sessionNodeRid,
        RoutingId sessionRid,
        string bindingToken,
        ulong bindingGeneration,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        string meshName,
        ulong targetNodeGeneration,
        ulong ownerLeaseGeneration,
        ulong sessionOwnerNodeGeneration,
        ulong acceptedHighWater,
        string sessionOwnerId = "",
        ulong sessionOwnerLeaseGeneration = 0,
        ZLinkActorPreviousBindingFence? previousFence = null)
    {
        return _getState(actorId).BeginSessionReplacement(
            sessionNodeRid,
            sessionRid,
            bindingToken,
            bindingGeneration,
            objectGeneration,
            authorityOwnerGeneration,
            ZLinkMeshName.FromBoundary(meshName, nameof(meshName)),
            targetNodeGeneration,
            ownerLeaseGeneration,
            sessionOwnerNodeGeneration,
            acceptedHighWater,
            sessionOwnerId,
            sessionOwnerLeaseGeneration,
            previousFence);
    }

    public void CompleteActorSessionReplacement(
        string actorId,
        ZLinkActorSessionReplacementAttempt attempt)
    {
        _getState(actorId).CompleteSessionReplacement(attempt);
    }

    public void PublishActorSessionReplacement(
        string actorId,
        ZLinkActorSessionReplacementAttempt attempt)
    {
        _getState(actorId).PublishSessionReplacement(attempt);
        _boundSessions.Register(
            actorId,
            attempt.Replacement.SessionRid,
            attempt.Replacement.BindingToken);
    }

    public void AbortActorSessionReplacement(
        string actorId,
        ZLinkActorSessionReplacementAttempt attempt,
        Exception failure)
    {
        _getState(actorId).AbortSessionReplacement(attempt, failure);
    }

    public void TombstoneSessionActorBinding(
        string actorId,
        RoutingId sessionRid,
        string bindingToken,
        ulong bindingGeneration,
        ulong sessionOwnerNodeGeneration,
        ZLinkSessionBindingRoute actorRoute)
    {
        _sessionBindings.Tombstone(
            actorId,
            sessionRid,
            bindingToken,
            bindingGeneration,
            sessionOwnerNodeGeneration,
            actorRoute);
    }

    public void UnbindActorSession(string actorId, string bindingToken)
    {
        _getState(actorId).UnbindSession(bindingToken);
        _boundSessions.Unregister(actorId, bindingToken);
    }

    public void RetireMigratedActorSession(string actorId, string bindingToken)
    {
        // The source actor no longer owns disconnect cleanup after a move, but
        // it must retain the exact binding fence while Message Follow can
        // forward delayed frames to the target actor.
        _boundSessions.Unregister(actorId, bindingToken);
    }

    public void TombstoneActorSession(
        string actorId,
        ZLinkActorBoundSession expected)
    {
        _getState(actorId).TombstoneSession(expected);
        _boundSessions.Unregister(actorId, expected.BindingToken);
    }

    public void RemoveActorSessionBinding(string actorId, string bindingToken)
    {
        if (TryGetSessionActorContext(actorId, bindingToken, out var context))
            UnbindSessionActor(actorId, context, bindingToken);
        UnbindActorSession(actorId, bindingToken);
    }

    public void CleanupActorSessionsForSession(RoutingId sessionRid) => _boundSessions.Cleanup(sessionRid);

    public bool TryGetActorBoundSession(string actorId, out ZLinkActorBoundSession session) =>
        _getState(actorId).TryGetBoundSession(out session);

    public void ResetGeneration()
    {
        PendingOutboundProof[] outboundProofs;
        CancellationTokenSource routeApplicationCancellation;
        lock (_outboundProofGate)
        {
            _applicationEpochOpen = false;
            _applicationEpoch = checked(_applicationEpoch + 1);
            outboundProofs = _pendingOutboundProofs.Values.ToArray();
            foreach (var proof in outboundProofs)
                proof.Retired = true;
            _pendingOutboundProofs.Clear();
            routeApplicationCancellation =
                _canonicalRouteApplicationCancellation;
        }
        Exception? routeCancellationFailure = null;
        try
        {
            routeApplicationCancellation.Cancel();
        }
        catch (Exception exception)
        {
            routeCancellationFailure = exception;
        }
        finally
        {
            foreach (var proof in outboundProofs)
                proof.Completion.TrySetCanceled(
                    routeApplicationCancellation.Token);
            routeApplicationCancellation.Dispose();
        }

        PendingRemoteRequest[] pending;
        lock (_pendingRemoteRequests)
        {
            pending = _pendingRemoteRequests.Values.ToArray();
            _pendingRemoteRequests.Clear();
            _remoteRequestAdmission.Clear();
        }
        foreach (var request in pending)
        {
            request.Dispose();
            _sessionBindings.CompleteAccepted(
                request.Binding.ActorRef.ActorId,
                request.Binding.BindingToken);
        }
        _sessionBindings.ResetGeneration();
        _boundSessions.Clear();
        _remoteFrames.Clear();
        Interlocked.Exchange(ref _bindingGeneration, 0);
        lock (_outboundProofGate)
        {
            _canonicalRouteApplicationCancellation =
                new CancellationTokenSource();
            _applicationEpochOpen = true;
        }
        if (routeCancellationFailure is not null)
            throw new AggregateException(
                "Session route proof cancellation failed during generation reset.",
                routeCancellationFailure);
    }

    private sealed class PendingRemoteRequest(
        ZLinkSessionBindingEntry binding,
        string replyCapability,
        CancellationTokenSource cancellation) : IDisposable
    {
        internal ZLinkSessionBindingEntry Binding { get; } = binding;
        internal string ReplyCapability { get; } = replyCapability;
        internal CancellationTokenSource Cancellation { get; } = cancellation;
        internal bool Claimed { get; set; }

        public void Dispose()
        {
            Cancellation.Cancel();
            Cancellation.Dispose();
        }
    }

    private readonly record struct RemoteRequestKey(
        string ActorId,
        ulong ObjectGeneration,
        string BindingToken,
        ulong RequestId);

    private sealed class PendingOutboundProof(
        long epoch,
        CancellationToken cancellationToken)
    {
        private readonly object _turnGate = new();
        private readonly Dictionary<long, TaskCompletionSource> _turns = [];
        private long _nextTicket;
        private long _servingTicket;

        internal static PendingOutboundProof Completed { get; } =
            new(0, CancellationToken.None);
        internal long Epoch { get; } = epoch;
        internal CancellationToken CancellationToken { get; } =
            cancellationToken;
        internal TaskCompletionSource<ZLinkSessionOutboundTenureProof>
            Completion { get; } = new(
                TaskCreationOptions.RunContinuationsAsynchronously);
        internal bool Retired { get; set; }

        internal long RegisterTicket()
        {
            lock (_turnGate)
                return _nextTicket++;
        }

        internal Task WaitForTurnAsync(long ticket)
        {
            lock (_turnGate)
            {
                if (ticket == _servingTicket)
                    return Task.CompletedTask;
                if (!_turns.TryGetValue(ticket, out var turn))
                {
                    turn = new TaskCompletionSource(
                        TaskCreationOptions.RunContinuationsAsynchronously);
                    _turns.Add(ticket, turn);
                }
                return turn.Task;
            }
        }

        internal void CompleteTurn(long ticket)
        {
            TaskCompletionSource? next = null;
            lock (_turnGate)
            {
                if (ticket != _servingTicket)
                    throw new InvalidOperationException(
                        "Session outbound proof tickets completed out of order.");
                _servingTicket++;
                if (_turns.Remove(_servingTicket, out var waiting))
                    next = waiting;
            }
            next?.TrySetResult();
        }

        internal bool IsDrained
        {
            get
            {
                lock (_turnGate)
                    return _servingTicket == _nextTicket;
            }
        }
    }

    private readonly record struct OutboundProofLease(
        ZLinkSessionOutboundTenureProof Proof,
        PendingOutboundProof Pending,
        long Ticket);

    internal sealed class RemoteReplyClaim(
        Func<byte[], RemotePushDelivery> deliver,
        Action completePending) : IDisposable
    {
        private int _completed;

        internal RemotePushDelivery Deliver(byte[] frame) => deliver(frame);

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _completed, 1) == 0)
                completePending();
        }
    }

    public bool Send(string actorId, IReadOnlyList<Message> parts, SendFlags flags)
    {
        var state = _getState(actorId);
        if (state.TryGetBoundSessionForOutbound(out var session))
        {
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"bound_session_send actor={actorId} session_node={session.SessionNodeRid} "
                + $"session_rid={session.SessionRid} binding={session.BindingToken} "
                + $"parts={parts.Count}");
            if (TryGetSessionActorContext(actorId, session.BindingToken, out _))
            {
                if (parts.Count != 1)
                    throw new InvalidOperationException("A local actor bound-session send requires one encoded stream frame.");
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"bound_session_send_local actor={actorId}");
                var targetNodeRid = state.NativeActorRef?.NodeRid;
                if (targetNodeRid is null
                    && _sessionBindings.TryGet(
                        actorId,
                        session.BindingToken,
                        out ZLinkSessionBindingEntry currentBinding))
                    targetNodeRid = currentBinding.Route.Ref.NodeRid;
                if (targetNodeRid is null)
                    throw Error(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{actorId}' does not have an accepted route.",
                        ZLinkRetryAdvice.DoNotRetry);
                var tenure = new ZLinkSessionOutboundTenure(
                    actorId,
                    session.ObjectGeneration,
                    session.MeshName.Value,
                    targetNodeRid.Value,
                    session.TargetNodeGeneration,
                    session.AuthorityOwnerGeneration,
                    session.OwnerLeaseGeneration,
                    session.BindingToken,
                    session.BindingGeneration,
                    session.SessionOwnerNodeGeneration,
                    session.SessionRid);
                var admission = _sessionBindings.AdmitOutbound(
                    tenure,
                    new ZLinkSessionOutboundTenureProof(
                        tenure,
                        targetNodeRid.Value.ToHex()),
                    ConcatParts(parts));
                return admission.Kind switch
                {
                    ZLinkSessionOutboundAdmissionKind.Immediate
                        => admission.Capability!.Settle(deliver: true)
                           == ZLinkSessionOutboundDelivery.Delivered,
                    ZLinkSessionOutboundAdmissionKind.Retained => true,
                    _ => false
                };
            }
            if (TryRelayRemotePush(actorId, session, ConcatParts(parts)))
            {
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"bound_session_send_remote_submitted actor={actorId}");
                return true;
            }
            if (!ZLinkActorBoundSessionBindingToken.IsNative(session.BindingToken))
                throw Error(ZLinkFrameworkErrorKind.InvalidOperation,
                    $"Actor '{actorId}' no longer has the selected local session binding.", ZLinkRetryAdvice.RetryAfterBackoff);
            var nativeActorRef = state.NativeActorRef
                                 ?? throw Error(
                                     ZLinkFrameworkErrorKind.NotFound,
                                     $"Actor '{actorId}' does not have a native Actor ref.",
                                     ZLinkRetryAdvice.DoNotRetry);
            return RequireNodeForMesh(
                    session.MeshName.Value,
                    "Actor bound session send requires its stored Mesh route.")
                .SendActorBoundSession(nativeActorRef, parts, flags);
        }

        var actorRef = state.NativeActorRef
                       ?? throw Error(ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorId}' does not have a native Actor ref.",
                           ZLinkRetryAdvice.DoNotRetry);
        return RequireNode("Actor bound session send requires a router-capable SpotNode.")
            .SendActorBoundSession(actorRef, parts, flags);
    }

    public bool SendIfBoundTo(
        string actorId,
        string expectedBindingToken,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        var state = _getState(actorId);
        return state.TryUseBoundSession(
            expectedBindingToken,
            _ => Send(actorId, parts, flags));
    }

    public ValueTask<ZLinkOneWaySubmitResult> SendIfBoundToAsync(
        string actorId,
        string expectedBindingToken,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken)
    {
        var state = _getState(actorId);
        if (!state.TryGetBoundSessionForOutbound(out var session)
            || !string.Equals(
                session.BindingToken,
                expectedBindingToken,
                StringComparison.Ordinal))
        {
            return ValueTask.FromResult(new ZLinkOneWaySubmitResult(
                ZLinkOneWaySubmitStatus.Submitted));
        }
        return SendCurrentAsync(
            state,
            actorId,
            session,
            parts,
            cancellationToken);
    }

    private ValueTask<ZLinkOneWaySubmitResult> SendCurrentAsync(
        ZLinkActorRuntimeState state,
        string actorId,
        ZLinkActorBoundSession session,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken)
    {
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"bound_session_send_async actor={actorId} session_node={session.SessionNodeRid} "
            + $"session_rid={session.SessionRid} binding={session.BindingToken} "
            + $"binding_gen={session.BindingGeneration} parts={parts.Count}");
        if (TryGetSessionActorContext(actorId, session.BindingToken, out _))
        {
            if (parts.Count != 1)
                throw new InvalidOperationException(
                    "A local actor bound-session send requires one encoded stream frame.");
            var targetNodeRid = state.NativeActorRef?.NodeRid;
            if (targetNodeRid is null
                && _sessionBindings.TryGet(
                    actorId,
                    session.BindingToken,
                    out ZLinkSessionBindingEntry currentBinding))
                targetNodeRid = currentBinding.Route.Ref.NodeRid;
            if (targetNodeRid is null)
                throw Error(
                    ZLinkFrameworkErrorKind.NotFound,
                    $"Actor '{actorId}' does not have an accepted route.",
                    ZLinkRetryAdvice.DoNotRetry);
            var tenure = new ZLinkSessionOutboundTenure(
                actorId,
                session.ObjectGeneration,
                session.MeshName.Value,
                targetNodeRid.Value,
                session.TargetNodeGeneration,
                session.AuthorityOwnerGeneration,
                session.OwnerLeaseGeneration,
                session.BindingToken,
                session.BindingGeneration,
                session.SessionOwnerNodeGeneration,
                session.SessionRid);
            var admission = _sessionBindings.AdmitOutbound(
                tenure,
                new ZLinkSessionOutboundTenureProof(
                    tenure,
                    targetNodeRid.Value.ToHex()),
                ConcatParts(parts));
            var status = admission.Kind switch
            {
                ZLinkSessionOutboundAdmissionKind.Immediate
                    when admission.Capability!.Settle(deliver: true)
                         == ZLinkSessionOutboundDelivery.Delivered
                    => ZLinkOneWaySubmitStatus.Submitted,
                ZLinkSessionOutboundAdmissionKind.Retained
                    => ZLinkOneWaySubmitStatus.Submitted,
                ZLinkSessionOutboundAdmissionKind.Backpressured
                    => ZLinkOneWaySubmitStatus.Backpressured,
                _ => ZLinkOneWaySubmitStatus.TargetNotFound
            };
            return ValueTask.FromResult(new ZLinkOneWaySubmitResult(status));
        }

        if (RemotePushRelayAsync is { } relay
            && session.SessionNodeRid is { } sessionNodeRid
            && !sessionNodeRid.IsEmpty
            && _getNodeForMesh(session.MeshName.Value) is { } localNode
            && !sessionNodeRid.Equals(localNode.RoutingId))
            return relay(
                actorId,
                session,
                ConcatParts(parts),
                cancellationToken);

        if (!ZLinkActorBoundSessionBindingToken.IsNative(session.BindingToken))
            throw Error(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actorId}' no longer has the selected local session binding.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        var nativeActorRef = state.NativeActorRef
                             ?? throw Error(
                                 ZLinkFrameworkErrorKind.NotFound,
                                 $"Actor '{actorId}' does not have a native Actor ref.",
                                 ZLinkRetryAdvice.DoNotRetry);
        var node = RequireNodeForMesh(
            session.MeshName.Value,
            "Actor bound session send requires its stored Mesh route.");
        return SendNativeBoundSessionAsync(
            node,
            nativeActorRef,
            session.BindingGeneration,
            parts,
            cancellationToken);
    }

    private static async ValueTask<ZLinkOneWaySubmitResult>
        SendNativeBoundSessionAsync(
            IZLinkBackendSpotNode node,
            ZLinkBackendActorRef actor,
            ulong expectedBindingGeneration,
            IReadOnlyList<Message> parts,
            CancellationToken cancellationToken)
    {
        try
        {
            await node.SendActorBoundSessionAsync(
                    actor,
                    expectedBindingGeneration,
                    parts,
                    cancellationToken)
                .ConfigureAwait(false);
            return new ZLinkOneWaySubmitResult(
                ZLinkOneWaySubmitStatus.Submitted);
        }
        catch (ZlinkSubmitException failure)
        {
            return failure.Result switch
            {
                ZlinkSubmitException.ErrorCode.NotConnected =>
                    new ZLinkOneWaySubmitResult(
                        ZLinkOneWaySubmitStatus.RouteNotConnected),
                ZlinkSubmitException.ErrorCode.NotFound =>
                    new ZLinkOneWaySubmitResult(
                        ZLinkOneWaySubmitStatus.TargetNotFound),
                ZlinkSubmitException.ErrorCode.Terminated =>
                    new ZLinkOneWaySubmitResult(
                        ZLinkOneWaySubmitStatus.Shutdown),
                ZlinkSubmitException.ErrorCode.Backpressured =>
                    new ZLinkOneWaySubmitResult(
                        ZLinkOneWaySubmitStatus.Backpressured),
                _ => throw ZLinkRequestFailureMapper.CreateSubmitException(
                    failure,
                    "Actor bound-session send")
            };
        }
        catch (ObjectDisposedException)
        {
            return new ZLinkOneWaySubmitResult(
                ZLinkOneWaySubmitStatus.Shutdown);
        }
    }

    public bool ReplyNoBind(ZLinkBackendActorRef actor, RoutingId sourceNodeRid, RoutingId sourceSessionRid,
        ulong requestId, uint flags, IReadOnlyList<Message> parts) =>
        RequireNode("Actor no-bind reply requires a router-capable SpotNode.")
            .ReplyActorNoBind(actor, sourceNodeRid, sourceSessionRid, requestId, flags, parts);

    public bool ForwardPart(ZLinkBackendActorRef actorRef, RoutingId sourceNodeRid, RoutingId sourceSessionRid,
        Message message, bool hasMore, SendFlags flags, string? meshName = null,
        IZLinkBackendSpotNode? selectedNode = null,
        ulong targetNodeGeneration = 0,
        ulong authorityOwnerGeneration = 0,
        ulong ownerLeaseGeneration = 0,
        ZLinkBackendActorRouteContext routeContext = default,
        ulong sourceNodeGeneration = 0,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource = null,
        ReadOnlyMemory<byte> applicationMetadata = default)
    {
        var routeNode = selectedNode ?? _getNode();
        //  Three outcomes below all return a bool the caller mostly ignores;
        //  name which branch a frame took so a vanished frame is traceable.
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"forward_part actor={actorRef.ActorId} target_node={actorRef.NodeRid} "
            + $"local_node={(routeNode is null ? "none" : routeNode.RoutingId.ToString())} "
            + $"has_relay={RemoteFrameRelay is not null} has_more={hasMore}");

        // A bound actor that migrated to another node cannot be reached
        // through the local bound-session send; buffer the parts and relay
        // the frame to the actor's owner node, which dispatches it through
        // its actor pipeline (replies come back on the push relay).
        if (RemoteFrameRelay is { } frameRelay
            && !actorRef.NodeRid.IsEmpty
            && routeNode is { } frameLocalNode
            && !actorRef.NodeRid.Equals(frameLocalNode.RoutingId))
        {
            // Locally relayed frames carry no session identity; the actor's
            // bound-session state (or the local session-side binding registry
            // after a source-side migration cleared the actor-side state)
            // names the session this frame belongs to.
            if (!routeContext.IsDirectRoute && sourceSessionRid.IsEmpty)
            {
                if (_getState(actorRef.ActorId).TryGetBoundSession(out var forwardSession)
                    && !forwardSession.SessionRid.IsEmpty)
                {
                    sourceSessionRid = forwardSession.SessionRid;
                    if (sourceNodeRid.IsEmpty && forwardSession.SessionNodeRid is { } forwardNode)
                        sourceNodeRid = forwardNode;
                }
                else if (_sessionBindings.TryGetByActorId(actorRef.ActorId, out var forwardContext)
                         && forwardContext.RoutingId is { } forwardRid)
                {
                    sourceSessionRid = forwardRid;
                }
            }

            var frameKey = RelayFrameKey(
                routeKind: 1,
                actorRef,
                bindingIdentity: string.Empty,
                sourceNodeRid,
                sourceNodeGeneration,
                sourceSessionRid,
                requestSource,
                routeContext);
            if (!_remoteFrames.TryAppend(
                    frameKey,
                    message.ToArray(),
                    hasMore,
                    out var completed))
                return false;
            if (completed is null)
                return true;

            if (completed.Parts.Count < 2)
            {
                _remoteFrames.Reject(completed);
                return false;
            }
            var header = completed.Parts[0];
            var frameBody = ConcatParts(completed.Parts, 1);
            if (!frameRelay(
                    meshName,
                    actorRef,
                    targetNodeGeneration,
                    authorityOwnerGeneration,
                    ownerLeaseGeneration,
                    sourceNodeRid,
                    sourceSessionRid,
                    routeContext,
                    sourceNodeGeneration,
                    requestSource,
                    applicationMetadata,
                    header,
                    frameBody))
            {
                // The assembler accepts both runtime retry forms: a terminal-
                // only retry reuses the prefix, while a new prefix replaces it.
                _remoteFrames.Reject(completed);
                return false;
            }
            _remoteFrames.Commit(completed);
            return true;
        }

        // A session on another node cannot be reached through the local
        // bound-session send; buffer the parts and relay the coalesced frame
        // to the session's node instead.
        if (RemotePushRelay is { } relay
            && !sourceNodeRid.IsEmpty
            && routeNode is { } localNode
            && !sourceNodeRid.Equals(localNode.RoutingId))
        {
            if (!_getState(actorRef.ActorId)
                    .TryGetBoundSession(out var session))
                return false;
            var frameKey = RelayFrameKey(
                routeKind: 2,
                actorRef,
                session.BindingToken,
                sourceNodeRid,
                sourceNodeGeneration,
                sourceSessionRid,
                requestSource,
                routeContext);
            if (!_remoteFrames.TryAppend(
                    frameKey,
                    message.ToArray(),
                    hasMore,
                    out var completed))
                return false;
            if (completed is null)
                return true;
            var frame = ConcatParts(completed.Parts, 0);
            if (!relay(actorRef.ActorId, session, frame))
            {
                _remoteFrames.Reject(completed);
                return false;
            }
            _remoteFrames.Commit(completed);
            return true;
        }

        return (routeNode
                ?? throw Error(
                    ZLinkFrameworkErrorKind.NotFound,
                    "Actor session forward requires a router-capable SpotNode.",
                    ZLinkRetryAdvice.DoNotRetry))
            .ForwardActorBoundSessionPart(actorRef, sourceNodeRid, sourceSessionRid, message, hasMore, flags);
    }

    private bool TryRelayRemotePush(string actorId, ZLinkActorBoundSession session, byte[] frame)
    {
        if (RemotePushRelay is not { } relay
            || session.SessionNodeRid is not { } sessionNodeRid
            || sessionNodeRid.IsEmpty)
        {
            return false;
        }
        if (_getNodeForMesh(session.MeshName.Value) is not { } localNode
            || sessionNodeRid.Equals(localNode.RoutingId))
        {
            return false;
        }
        var submitted = relay(actorId, session, frame);
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"session_push_relay actor={actorId} source_node={localNode.RoutingId} "
            + $"target_node={sessionNodeRid} submitted={submitted} bytes={frame.Length}");
        return submitted;
    }

    private static byte[] ConcatParts(IReadOnlyList<Message> parts)
    {
        if (parts.Count == 1) return parts[0].ToArray();
        var buffers = new byte[parts.Count][];
        var total = 0;
        for (var i = 0; i < parts.Count; i++)
        {
            buffers[i] = parts[i].ToArray();
            total += buffers[i].Length;
        }

        var frame = new byte[total];
        var offset = 0;
        foreach (var buffer in buffers)
        {
            buffer.CopyTo(frame, offset);
            offset += buffer.Length;
        }

        return frame;
    }

    private static byte[] ConcatParts(
        IReadOnlyList<byte[]> parts,
        int start)
    {
        var total = 0;
        for (var i = start; i < parts.Count; i++)
            total = checked(total + parts[i].Length);
        var frame = new byte[total];
        var offset = 0;
        for (var i = start; i < parts.Count; i++)
        {
            parts[i].CopyTo(frame, offset);
            offset += parts[i].Length;
        }
        return frame;
    }

    private static ZLinkRemoteRelayFrameKey RelayFrameKey(
        byte routeKind,
        ZLinkBackendActorRef actor,
        string bindingIdentity,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        RoutingId sourceSessionRid,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource,
        ZLinkBackendActorRouteContext route) => new(
            routeKind,
            actor.ActorId,
            actor.Generation,
            bindingIdentity,
            sourceNodeRid.IsEmpty ? string.Empty : sourceNodeRid.ToHex(),
            sourceNodeGeneration,
            sourceSessionRid.IsEmpty ? string.Empty : sourceSessionRid.ToHex(),
            requestSource?.OwnerId ?? string.Empty,
            requestSource?.LeaseGeneration ?? 0,
            requestSource is { } source
                ? source.NodeRid.ToHex()
                : string.Empty,
            requestSource?.NodeGeneration ?? 0,
            route.OperationId.High,
            route.OperationId.Low,
            route.ReplyRequestId,
            route.TargetNodeGeneration,
            route.AuthorityOwnerGeneration,
            route.OwnerLeaseGeneration);

    public ValueTask NotifyRemoteDisconnectedAsync(
        ZLinkSessionBindingEntry binding,
        IZLinkBackendSpotNode node,
        ZLinkServiceWireCodec.RequestSourceFence localRequestSource,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var actorRef = binding.Route.Ref.ToBackend();
        if (binding.Context.RoutingId is not { } sourceSessionRid)
        {
            node.CloseActorBoundSession(actorRef, _registration.DefaultRequestTimeout, cancellationToken);
            return ValueTask.CompletedTask;
        }
        var sourceNodeRid = node.RoutingId;
        // A disconnect is a one-way bound-session frame, but it can be
        // captured by an Actor handoff before the source route is released.
        // Give it the same immutable operation identity as every other
        // accepted frame so canonical journaling can preserve its ordering
        // and authority fences.
        var operationId = node.AllocateOperationId();

        var header = new ZlinkStreamHeader(ZlinkStreamMessageKind.Send, ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None, null, ZLinkRemoteActorJoinPackets.SessionDisconnectedPacketName,
            ZlinkStreamMetadata.Empty);
        using var headerPart = Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span);
        using var bodyPart = Message.From(
            ZLinkActorBoundSessionRelay.EncodeSessionDisconnected(
                binding.BindingToken,
                binding.BindingGeneration,
                binding.SessionOwnerNodeGeneration));
        //  A relayed session frame has to name the session it belongs to and
        //  the node it came from, and the relay validates that on the way out.
        //  Without it the disconnect never leaves this node.
        var routeContext = new ZLinkBackendActorRouteContext(
            operationId,
            0,
            binding.TargetNodeGeneration,
            binding.AuthorityOwnerGeneration,
            binding.OwnerLeaseGeneration,
            IsBoundSessionRoute: true);
        var applicationMetadata = ZLinkActorBoundSessionHandoffMetadata.Encode(
            new ZLinkActorBoundSessionHandoffFence(
                actorRef.ActorId,
                actorRef.Generation,
                sourceSessionRid,
                binding.BindingToken,
                binding.BindingGeneration,
                //  The fence rejects a zero sequence, and a disconnect carries
                //  no frame of its own, so it names the last one it accepted.
                binding.AcceptedHighWater == 0 ? 1 : binding.AcceptedHighWater));
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"disconnect_route_prepared actor={actorRef.ActorId} "
            + $"operation={operationId.High:x16}{operationId.Low:x16} "
            + $"target_node={actorRef.NodeRid} source_node={sourceNodeRid}");
        // The disconnect frame takes the same route as any session frame to
        // this actor: ForwardPart relays it to the actor's owner node when the
        // actor is remote and writes the native bound session when it is local.
        if (!ForwardPart(
                actorRef, sourceNodeRid, sourceSessionRid, headerPart, true,
                SendFlags.DontWait, binding.MeshName, node,
                binding.TargetNodeGeneration,
                binding.AuthorityOwnerGeneration,
                binding.OwnerLeaseGeneration,
                routeContext,
                localRequestSource.NodeGeneration,
                localRequestSource,
                applicationMetadata))
            throw new InvalidOperationException("Actor session disconnect header forward failed.");
        if (!ForwardPart(
                actorRef, sourceNodeRid, sourceSessionRid, bodyPart, false,
                SendFlags.DontWait, binding.MeshName, node,
                binding.TargetNodeGeneration,
                binding.AuthorityOwnerGeneration,
                binding.OwnerLeaseGeneration,
                routeContext,
                localRequestSource.NodeGeneration,
                localRequestSource,
                applicationMetadata))
            throw new InvalidOperationException("Actor session disconnect body forward failed.");
        return ValueTask.CompletedTask;
    }

    public ValueTask CloseAsync(string actorId, CancellationToken cancellationToken)
    {
        var state = _getState(actorId);
        var actorRef = state.NativeActorRef
                       ?? throw Error(ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorId}' does not have a native Actor ref.",
                           ZLinkRetryAdvice.DoNotRetry);
        if (!state.TryGetBoundSession(out var session))
            throw Error(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actorId}' does not have a bound session.",
                ZLinkRetryAdvice.DoNotRetry);
        RequireNodeForMesh(
                session.MeshName.Value,
                "Actor bound session close requires its stored Mesh route.")
            .CloseActorBoundSession(actorRef, _registration.DefaultRequestTimeout, cancellationToken);
        return ValueTask.CompletedTask;
    }

    private IZLinkBackendSpotNode RequireNodeForMesh(
        string meshName,
        string message,
        ZLinkFrameworkErrorKind kind = ZLinkFrameworkErrorKind.InvalidOperation) =>
        _getNodeForMesh(meshName) ??
        throw Error(kind, message, ZLinkRetryAdvice.DoNotRetry);

    private IZLinkBackendSpotNode RequireNode(string message,
        ZLinkFrameworkErrorKind kind = ZLinkFrameworkErrorKind.InvalidOperation) =>
        _getNode() ?? throw Error(kind, message, ZLinkRetryAdvice.DoNotRetry);

    private static ZLinkFrameworkException Error(
        ZLinkFrameworkErrorKind kind,
        string message,
        ZLinkRetryAdvice retryAdvice) =>
        new(kind, message, retryAdvice);
}
