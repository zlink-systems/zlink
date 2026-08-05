namespace Zlink.Framework.Runtime.Host;

internal sealed class ZLinkActorBoundSessionCoordinator
{
    private readonly ZLinkActorBoundSessionRegistry _boundSessions;
    private readonly ZLinkSessionActorBindingTable _sessionBindings;
    private readonly ZLinkRemoteRelayFrameAssembler _remoteFrames;
    private readonly ZLinkBoundedRemoteRequestAdmission _remoteRequestAdmission = new();
    private readonly Dictionary<RemoteRequestKey, PendingRemoteRequest>
        _pendingRemoteRequests = new();
    private readonly Func<string, ZLinkActorRuntimeState> _getState;
    private readonly Func<IZLinkBackendSpotNode?> _getNode;
    private readonly Func<string, IZLinkBackendSpotNode?> _getNodeForMesh;
    private readonly ZLinkFrameworkRegistration _registration;
    private readonly Func<CancellationToken> _getShutdownToken;
    private long _bindingGeneration;

    public ZLinkActorBoundSessionCoordinator(
        Func<string, ZLinkActorRuntimeState> getState,
        Func<IZLinkBackendSpotNode?> getNode,
        Func<string, IZLinkBackendSpotNode?> getNodeForMesh,
        ZLinkFrameworkRegistration registration,
        Func<CancellationToken> getShutdownToken)
    {
        _getState = getState;
        _getNode = getNode;
        _getNodeForMesh = getNodeForMesh;
        _registration = registration;
        _getShutdownToken = getShutdownToken;
        _sessionBindings = new ZLinkSessionActorBindingTable(
            registration.DefaultRequestTimeout + registration.DefaultRequestTimeout);
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
        Backpressured,
        // No session-actor binding right now — transient during a rebind
        // (release→bind gap), so callers may retry briefly.
        NoBinding,
        // Bound to a different session: a definite new binding; late pushes
        // must not apply to it (spec 31 §6) — dropped.
        WrongSession
    }

    /// <summary>Session-node delivery for a relayed remote push: writes the
    /// frame to the still-bound local session. A binding or session-rid miss
    /// is a stale push racing rebind/disconnect and is dropped (spec 31 §6);
    /// a failed write is backpressure the caller may retry.</summary>
    public RemotePushDelivery DeliverLocalSessionFrame(
        ZLinkRemoteSessionPushRelay identity,
        byte[] frame,
        RoutingId sourceNodeRid)
    {
        if (!_sessionBindings.TryGet(
                identity.ActorId,
                identity.BindingToken,
                out ZLinkSessionBindingEntry entry))
        {
            return _sessionBindings.TryGetByActorId(identity.ActorId, out _)
                ? RemotePushDelivery.WrongSession
                : RemotePushDelivery.NoBinding;
        }
        var targetNodeRid = RoutingId.FromHex(identity.TargetNodeRid);
        var matchesCommittedRoute = entry.Route.MatchesFence(
            identity.ActorId,
            identity.ObjectGeneration,
            identity.AuthorityOwnerGeneration,
            identity.MeshName,
            identity.TargetNodeGeneration,
            identity.OwnerLeaseGeneration);
        var matchesSealedTargetRoute =
            entry.RelocationHandoffId is not null
            && sourceNodeRid == targetNodeRid
            // CommitRoute can install the target route before Unseal clears
            // the handoff. A target push carrying the pre-commit identity is
            // valid during that interval even when the route now names the
            // target node.
            // AuthorityOwnerGeneration is scoped to the owning node lifecycle;
            // the target owner may therefore have a lower value than the
            // source owner. The active handoff and a different target fence
            // identify the provisional route during the transition.
            && entry.ObjectGeneration == identity.ObjectGeneration
            && identity.AuthorityOwnerGeneration > 0
            && (identity.AuthorityOwnerGeneration != entry.AuthorityOwnerGeneration
                || identity.TargetNodeGeneration != entry.TargetNodeGeneration
                || identity.OwnerLeaseGeneration != entry.OwnerLeaseGeneration)
            && !string.IsNullOrWhiteSpace(identity.MeshName)
            && identity.TargetNodeGeneration > 0
            && identity.OwnerLeaseGeneration > 0;
        if (entry.BindingGeneration != identity.BindingGeneration
            || (!matchesCommittedRoute && !matchesSealedTargetRoute)
            || entry.SessionOwnerNodeGeneration != identity.SessionOwnerNodeGeneration
            || (matchesCommittedRoute && entry.Route.Ref.NodeRid != targetNodeRid)
            || entry.Context.RoutingId is not { } boundRid
            || !boundRid.Equals(RoutingId.FromHex(identity.SessionRid)))
        {
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"session_push_refused actor={identity.ActorId} "
                + $"binding={entry.BindingGeneration == identity.BindingGeneration} "
                + $"committed={matchesCommittedRoute} sealed={matchesSealedTargetRoute} "
                + $"handoff={entry.RelocationHandoffId is not null} "
                + $"source_matches={sourceNodeRid == targetNodeRid} "
                + $"object={entry.ObjectGeneration == identity.ObjectGeneration} "
                + $"authority={entry.AuthorityOwnerGeneration}/{identity.AuthorityOwnerGeneration} "
                + $"mesh={string.Equals(entry.MeshName, identity.MeshName, StringComparison.Ordinal)} "
                + $"target_generation={identity.TargetNodeGeneration} lease={identity.OwnerLeaseGeneration} "
                + $"session_owner={entry.SessionOwnerNodeGeneration == identity.SessionOwnerNodeGeneration} "
                + $"route_node={entry.Route.Ref.NodeRid.ToHex()} "
                + $"target_node={identity.TargetNodeRid} "
                + $"session_rid={entry.Context.RoutingId?.ToHex() ?? "none"} "
                + $"identity_session_rid={identity.SessionRid}");
            return RemotePushDelivery.WrongSession;
        }
        using var message = Message.From(frame);
        var written = entry.Context.Write(message);
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"session_push_deliver actor={identity.ActorId} source_node={sourceNodeRid} "
            + $"target_node={identity.TargetNodeRid} written={written} bytes={frame.Length}");
        return written ? RemotePushDelivery.Delivered : RemotePushDelivery.Backpressured;
    }

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
            || entry.BindingGeneration != expected.BindingGeneration
            || entry.SessionOwnerNodeGeneration != expected.SessionOwnerNodeGeneration
            || entry.Route != expected.Route
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
        ulong sessionOwnerNodeGeneration)
    {
        var replaced = _sessionBindings.Bind(
            actorId,
            context,
            bindingToken,
            actorRef,
            bindingGeneration,
            route,
            sessionOwnerNodeGeneration);
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
                    actorId,
                    pending.Binding.BindingToken);
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
        lock (_pendingRemoteRequests)
        {
            if (!_remoteRequestAdmission.TryAcquire(actorId, bindingToken))
            {
                pending.Dispose();
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    $"Actor '{actorId}' remote session request capacity is exhausted.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            }
            if (!_pendingRemoteRequests.TryAdd(key, pending))
            {
                _remoteRequestAdmission.Release(actorId, bindingToken);
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
                key.ActorId,
                pending.Binding.BindingToken);
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
        ulong acceptedHighWater = 0)
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
            meshName,
            targetNodeGeneration,
            ownerLeaseGeneration,
            sessionOwnerNodeGeneration,
            acceptedHighWater);
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
        ZLinkActorPreviousBindingFence? previousFence = null)
    {
        return _getState(actorId).BeginSessionReplacement(
            sessionNodeRid,
            sessionRid,
            bindingToken,
            bindingGeneration,
            objectGeneration,
            authorityOwnerGeneration,
            meshName,
            targetNodeGeneration,
            ownerLeaseGeneration,
            sessionOwnerNodeGeneration,
            acceptedHighWater,
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

    public void MarkPreviousActorSessionBindingTombstoned(
        string actorId,
        ZLinkActorSessionReplacementAttempt attempt)
    {
        _getState(actorId).MarkPreviousSessionBindingTombstoned(attempt);
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
            if (TryGetSessionActorContext(actorId, session.BindingToken, out var context))
            {
                if (parts.Count != 1)
                    throw new InvalidOperationException("A local actor bound-session send requires one encoded stream frame.");
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"bound_session_send_local actor={actorId}");
                return context.Write(parts[0]);
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
                    session.MeshName,
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

    public ZLinkAsyncSubmitter CreateSubmitter(string meshName)
    {
        var node = RequireNodeForMesh(
            meshName,
            "Actor bound session send requires its stored Mesh route.");
        return new ZLinkAsyncSubmitter(node.OnSendReady, _registration.DefaultSocketSendTimeout, _getShutdownToken());
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
        if (_getNodeForMesh(session.MeshName) is not { } localNode
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
                session.MeshName,
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
