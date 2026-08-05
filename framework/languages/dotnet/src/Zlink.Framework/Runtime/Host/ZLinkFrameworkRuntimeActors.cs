using Microsoft.Extensions.DependencyInjection;
using System.Runtime.ExceptionServices;

namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    private readonly ZLinkActorMessageFollower _actorMessageFollower;
    private readonly System.Collections.Concurrent.ConcurrentDictionary<
        Guid, byte> _publishedActorRecoveryWatches = new();

    internal ZLinkActorMessageFollower ActorMessageFollower
        => _actorMessageFollower;

    internal ValueTask<ZLinkSessionRouteSealReply> RequestSessionRouteSealAsync(
        string meshName,
        RoutingId sessionOwnerNode,
        ZLinkSessionRouteSealRequest request,
        CancellationToken cancellationToken) =>
        RequestSessionRouteControlAsync<
            ZLinkSessionRouteSealRequest,
            ZLinkSessionRouteSealReply>(
            meshName,
            sessionOwnerNode,
            request,
            cancellationToken);

    internal ValueTask<ZLinkSessionRouteSealReply> RequestSessionRouteAbortAsync(
        string meshName,
        RoutingId sessionOwnerNode,
        ZLinkSessionRouteAbortRequest request,
        CancellationToken cancellationToken) =>
        RequestSessionRouteControlAsync<
            ZLinkSessionRouteAbortRequest,
            ZLinkSessionRouteSealReply>(
            meshName,
            sessionOwnerNode,
            request,
            cancellationToken);

    internal ValueTask<ZLinkSessionRouteCommitReply> RequestSessionRouteCommitAsync(
        string meshName,
        RoutingId sessionOwnerNode,
        ZLinkSessionRouteCommitRequest request,
        CancellationToken cancellationToken) =>
        RequestSessionRouteControlAsync<
            ZLinkSessionRouteCommitRequest,
            ZLinkSessionRouteCommitReply>(
            meshName,
            sessionOwnerNode,
            request,
            cancellationToken);

    internal ValueTask<ZLinkSessionRouteCommitReply> RequestSessionRouteUnsealAsync(
        string meshName,
        RoutingId sessionOwnerNode,
        ZLinkSessionRouteUnsealRequest request,
        CancellationToken cancellationToken) =>
        RequestSessionRouteControlAsync<
            ZLinkSessionRouteUnsealRequest,
            ZLinkSessionRouteCommitReply>(
            meshName,
            sessionOwnerNode,
            request,
            cancellationToken);

    private async ValueTask<TReply> RequestSessionRouteControlAsync<TRequest, TReply>(
        string meshName,
        RoutingId sessionOwnerNode,
        TRequest request,
        CancellationToken cancellationToken)
    {
        var timeout = Registration.DefaultRequestTimeout;
        //  Traced at the send side; the receiving node's handler traces its own
        //  entry, so a stall shows which side never moved.
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"route_control_sent target={sessionOwnerNode} type={typeof(TRequest).Name}");
        var packetName = ZLinkMessageNameResolver.ResolveFromMessage(request);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken);
        deadline.CancelAfter(timeout);
        var retryDelay = TimeSpan.FromMilliseconds(10);
        while (true)
        {
            var header = ZLinkClientCallCodec.CreateEnvelope(
                ZLinkMessageKind.Request,
                meshName,
                packetName,
                timeout);
            var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(
                header,
                request,
                Registration.Codecs);
            try
            {
                var reply = await GetMeshNodeRuntime(meshName)
                    .RequestToNodeAsync(
                        sessionOwnerNode,
                        parts,
                        timeout,
                        deadline.Token)
                    .ConfigureAwait(false);
                return ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<TReply>(
                    reply,
                    "Session route control reply is empty.",
                    $"Session route control request '{packetName}' failed.",
                    Registration.Codecs);
            }
            catch (ZLinkFrameworkException error)
                when (error.Kind == ZLinkFrameworkErrorKind.Unavailable
                      && error.RetryAdvice == ZLinkRetryAdvice.RetryAfterBackoff
                      && !deadline.IsCancellationRequested)
            {
                await Task.Delay(retryDelay, deadline.Token)
                    .ConfigureAwait(false);
                retryDelay = TimeSpan.FromMilliseconds(
                    Math.Min(retryDelay.TotalMilliseconds * 2, 100));
            }
            catch (OperationCanceledException)
                when (!cancellationToken.IsCancellationRequested
                      && deadline.IsCancellationRequested)
            {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    $"Session route control request '{packetName}' did not reach its owner before the deadline.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            }
        }
    }

    internal ValueTask<ZLinkActorJoinResult> JoinActorAsync(
        string spotId,
        IZLinkActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken = default)
    {
        return JoinActorAsync(spotId, actor, request, operationId: null, cancellationToken);
    }

    internal ValueTask<ZLinkActorJoinResult> JoinActorAsync(
        string spotId,
        IZLinkActor actor,
        ZLinkMessage request,
        ZLinkActorJoinOperationId? operationId,
        CancellationToken cancellationToken = default,
        DateTimeOffset? absoluteDeadline = null)
    {
        _drainAdmission.RequireSpotAdmission();
        return _actors.JoinActorAsync(
            spotId,
            actor,
            request,
            operationId,
            cancellationToken,
            absoluteDeadline);
    }

    internal ValueTask<ZLinkActorJoinResult> JoinActorAsync(
        string spotId,
        ActorRef actor,
        ZLinkMessage request,
        CancellationToken cancellationToken = default)
    {
        var managedActor = ResolveOwnedActorRef(actor);
        return JoinActorAsync(spotId, managedActor, request, cancellationToken);
    }

    internal ValueTask<ZLinkActorJoinResult> JoinActorEntrySpotAsync(
        IZLinkActor actor,
        ZLinkMessage request,
        ZLinkActorJoinOperationId? operationId,
        CancellationToken cancellationToken = default,
        DateTimeOffset? absoluteDeadline = null)
    {
        _drainAdmission.RequireSpotAdmission();
        return _actors.JoinActorEntrySpotAsync(
            actor,
            request,
            operationId,
            cancellationToken,
            absoluteDeadline);
    }

    internal ValueTask<ZLinkActorJoinResult> JoinActorEntrySpotAsync(
        RoutingId spotNodeRid,
        IZLinkActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken = default)
    {
        _drainAdmission.RequireSpotAdmission();
        return _actors.JoinActorEntrySpotAsync(
            spotNodeRid,
            actor,
            request,
            cancellationToken);
    }

    internal ValueTask<ZLinkActorJoinResult> JoinActorEntrySpotAsync(
        RoutingId spotNodeRid,
        ActorRef actor,
        ZLinkMessage request,
        CancellationToken cancellationToken = default)
    {
        var managedActor = ResolveOwnedActorRef(actor);
        return JoinActorEntrySpotAsync(spotNodeRid, managedActor, request, cancellationToken);
    }

    internal ValueTask<ZLinkActorDrainResult> DrainActorsAsync(
        DateTimeOffset absoluteDeadline,
        CancellationToken cancellationToken) =>
        _actorDrainCoordinator.DrainAsync(
            _relocationTargetSelection,
            absoluteDeadline,
            cancellationToken);

    internal async ValueTask<ZLinkFrameworkRelocationReason?> PreflightRetireAsync(
        ZLinkFrameworkRelocationMode mode,
        long targetApplicationVersion,
        CancellationToken cancellationToken)
    {
        if (HasUnsupportedManualTopology(Registration))
            return ZLinkFrameworkRelocationReason.ManualTopologyUnsupported;

        var selection = new ZLinkRelocationTargetSelection(
            mode,
            targetApplicationVersion);
        _relocationTargetSelection = selection;
        var preflightStage = "initial";
        try
        {
            while (true)
            {
                //  Spec 28: relocation seals new admission while an already
                //  accepted application turn may finish at its Spot boundary.
                //  Waiting for the host-wide operation count here would make an
                //  accepted turn wait for the relocation marker that can only be
                //  published after this preflight returns.
                preflightStage = "initial_actor_handoff_wait";
                await WaitForAcceptedActorHandoffsAsync(cancellationToken)
                    .ConfigureAwait(false);

                var operationBaseline = SnapshotOperationAdmissions();
                var actorBaseline = DrainAdmission.SnapshotActorAdmissions();
                var handoffBaseline = _actorHandoffAdmissions.SnapshotDrain();
                if (actorBaseline.ActiveCount != 0
                    || !handoffBaseline.IsSafe)
                    continue;

                var plan = new ZLinkRetirePreflightPlan();
                ZLinkFrameworkRelocationReason? targetBlocker = null;
                preflightStage = "spot_preflight";
                foreach (var node in GetOrStartState().SpotNodes.Values)
                {
                    var blocker = await node.Catalog.PreflightRetireAsync(
                            plan,
                            selection,
                            cancellationToken)
                        .ConfigureAwait(false);
                    if (blocker is not null)
                    {
                        targetBlocker = blocker;
                        break;
                    }
                }

                if (targetBlocker is null)
                {
                    preflightStage = "actor_handoff_wait";
                    await WaitForAcceptedActorHandoffsAsync(cancellationToken)
                        .ConfigureAwait(false);
                    preflightStage = "actor_preflight";
                    targetBlocker = await _actorDrainCoordinator.PreflightAsync(
                            plan,
                            selection,
                            cancellationToken)
                        .ConfigureAwait(false);
                }

                if (targetBlocker is null)
                {
                    preflightStage = "relocation_fence";
                    var fence = await TryBeginRelocationAdmissionFenceAsync(
                            operationBaseline,
                            actorBaseline,
                            handoffBaseline,
                            cancellationToken)
                        .ConfigureAwait(false);
                    if (fence is not null)
                        return null;
                    continue;
                }
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"relocation_preflight_blocker mode={mode} "
                    + $"target_version={targetApplicationVersion} "
                    + $"reason={targetBlocker}");
                if (targetBlocker != ZLinkFrameworkRelocationReason.TargetUnavailable)
                    return targetBlocker;
                preflightStage = "target_wait";
                if (!HasExactAutomaticRouteMeshPeerReadiness())
                {
                    if (await WaitForTargetAvailabilityAsync(
                            Registration.Locations.Options.PollingInterval,
                            cancellationToken)
                        .ConfigureAwait(false) is { } unavailable)
                        return unavailable;
                    continue;
                }

                if (await WaitForTargetAvailabilityAsync(
                        Registration.Locations.Options.PollingInterval,
                        cancellationToken)
                    .ConfigureAwait(false) is { } unavailableAfterReadiness)
                    return unavailableAfterReadiness;
            }
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"relocation_preflight_cancelled mode={mode} "
                + $"target_version={targetApplicationVersion} "
                + $"stage={preflightStage}");
            throw;
        }
    }

    // A target lookup already established TargetUnavailable before this wait.
    // Preserve that typed blocker when the target-wait deadline expires; the
    // maintenance owner still replaces it with ShutdownRequested when shutdown
    // cancelled the shared relocation operation.
    internal static async ValueTask<ZLinkFrameworkRelocationReason?>
        WaitForTargetAvailabilityAsync(
            TimeSpan pollingInterval,
            CancellationToken cancellationToken)
    {
        try
        {
            await Task.Delay(pollingInterval, cancellationToken)
                .ConfigureAwait(false);
            return null;
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
            return ZLinkFrameworkRelocationReason.TargetUnavailable;
        }
    }

    internal async ValueTask<bool> PublishRetiringAsync(
        CancellationToken cancellationToken)
    {
        var fence = CaptureRelocationAdmissionFence();
        try
        {
            var published = _autoConnect is null
                            || await _autoConnect.MarkRetiringAsync(cancellationToken)
                                .ConfigureAwait(false);
            if (published)
                _drainAdmission.BeginDrain(ZLinkDrainOwner.Relocation);
            else if (fence is { } expectedFence)
                TryReopenRetireAdmissionsAfterRollback(expectedFence);
            return published;
        }
        catch (ZLinkRetiringPublicationRollbackException)
        {
            _drainAdmission.BeginDrain(ZLinkDrainOwner.Relocation);
            _drainAdmission.Seal();
            throw;
        }
        catch
        {
            if (fence is { } expectedFence)
                TryReopenRetireAdmissionsAfterRollback(expectedFence);
            throw;
        }
    }

    internal static bool HasUnsupportedManualTopology(
        ZLinkFrameworkRegistration registration)
    {
        if (registration.SpotNodes.Values.Any(static node =>
                node.Router?.AcquisitionMode == ZLinkPeerAcquisitionMode.Manual))
            return true;

        if (registration.Channels.Values.Any(static channel =>
                channel.Client?.AcquisitionMode == ZLinkPeerAcquisitionMode.Manual
                || channel.Subscriber?.AcquisitionMode == ZLinkPeerAcquisitionMode.Manual))
            return true;

        return registration.Channels.Values.Any(channel =>
            channel.Publisher is not null && !registration.Locations.Enabled);
    }

    internal bool HasExactAutomaticRouteMeshPeerReadiness()
    {
        var localNodeRids = Registration.SpotNodes.Values
            .Select(static node => node.EffectiveRoutingId)
            .ToHashSet();
        foreach (var registration in Registration.SpotNodes.Values)
        {
            if (registration.Router?.AcquisitionMode != ZLinkPeerAcquisitionMode.AutoConnect)
                continue;

            var meshName = registration.SpotMeshChannelName ?? registration.SpotNodeName;
            var descriptors = _topologyQuery?.GetCompleteRouteMeshPeers(meshName);
            if (descriptors is null) return false;

            var corePeers = GetMeshNodeRuntime(meshName).Node.MeshPeers();
            if (!HasExactPeerReadiness(
                    descriptors,
                    corePeers,
                    localNodeRids))
                return false;
        }

        return true;
    }

    internal static bool HasExactPeerReadiness(
        IReadOnlyList<ZLinkRouteMeshPeerIdentity> descriptors,
        IReadOnlyList<MeshNodePeer> corePeers,
        IReadOnlySet<RoutingId> localNodeRids)
    {
        var eligible = descriptors
            .Where(descriptor => !descriptor.Draining
                                 && !localNodeRids.Contains(descriptor.NodeRid))
            .ToArray();
        return eligible.Length > 0
               && eligible.All(descriptor => corePeers.Any(peer =>
                peer.RoutingId == descriptor.NodeRid
                && peer.LifecycleGeneration == descriptor.LifecycleGeneration
                && peer.State == MeshPeerState.Admitted));
    }

    internal static async ValueTask<bool> WaitForExactPeerReadinessAsync(
        Func<bool> isReady,
        TimeSpan pollingInterval,
        CancellationToken cancellationToken)
    {
        try
        {
            while (!isReady())
                await Task.Delay(pollingInterval, cancellationToken)
                    .ConfigureAwait(false);
            return true;
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            return false;
        }
    }

    internal static string? ResolveActorDrainMeshName(
        ZLinkFrameworkRegistration registration,
        string actorType)
    {
        return ZLinkActorDrainCoordinator.ResolveMeshName(registration, actorType);
    }

    private IZLinkActor ResolveOwnedActorRef(ActorRef actor)
    {
        if (!TryGetCreatedActorState(actor.ActorId, out var state)
            || state.Actor is not { } managedActor
            || state.NativeActorRef is not { } nativeRef
            || nativeRef.NodeRid != actor.NodeRid
            || nativeRef.Generation != actor.ObjectGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor ref '{actor.ActorId}' is not owned by this runtime.");

        return managedActor;
    }


    internal ValueTask DestroyActorAsync(
        RoutingId entrySpotNodeRid,
        IZLinkActor actor,
        CancellationToken cancellationToken = default)
    {
        return _actorSessionManager.DestroyActorAsync(entrySpotNodeRid, actor, cancellationToken);
    }

    internal async ValueTask<ZLinkRemoteActorJoinReply> JoinRoutedActorAsync(
        string spotId,
        ZLinkRemoteActorJoinRequest request,
        CancellationToken cancellationToken = default)
    {
        var authorityStore = Registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Cross-node Actor relocation requires an Authority Store.");
        var relocationStore = Registration.Locations.ResolveRelocationStore()
                              ?? throw new ZLinkConfigurationException(
                                  "Cross-node Actor relocation requires a Relocation Store.");
        var target = ResolveActorHandoffTarget(spotId)
                     ?? throw new InvalidOperationException(
                         $"Actor handoff target '{spotId}' is not active.");
        await ValidateActorRelocationTargetAsync(
                request,
                target,
                authorityStore,
                cancellationToken)
            .ConfigureAwait(false);
        ZLinkRelocationEnvelope durableEnvelope;
        try
        {
            durableEnvelope =
                await new ZLinkRelocationPublicationCoordinator(
                        authorityStore,
                        relocationStore)
                    .ReadPreparedAsync(
                        ZLinkActorRelocationRoot.Reference(request),
                        cancellationToken)
                    .ConfigureAwait(false);
        }
        catch (ZLinkRelocationDataLostException error)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                error.Message,
                retryAdvice: ZLinkRetryAdvice.DoNotRetry,
                error);
        }
        var durable = ZLinkActorRelocationRoot.Load(
            request,
            durableEnvelope);
        var currentAuthority = await authorityStore.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(request.ActorId),
                cancellationToken)
            .ConfigureAwait(false);
        if (currentAuthority is ZLinkAuthorityReadResult.Found current
            && ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                current.Snapshot.Payload.Span,
                out var currentPublication)
            && (!string.Equals(
                    currentPublication.Reference,
                    request.RelocationReference,
                    StringComparison.Ordinal)
                || currentPublication.ChecksumCrc32c
                != request.RelocationChecksumCrc32c))
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"handoff_root_mismatch actor={request.ActorId} handoff={request.HandoffId} "
                + $"request_ref={request.RelocationReference} request_crc={request.RelocationChecksumCrc32c} "
                + $"current_ref={currentPublication.Reference} current_crc={currentPublication.ChecksumCrc32c} "
                + $"current_owner={current.Snapshot.OwnerId} current_store={current.Snapshot.StoreVersion}");
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                $"Actor '{request.ActorId}' authority references another relocation root.",
                retryAdvice: ZLinkRetryAdvice.DoNotRetry);
        }
        request = ZLinkActorRelocationRoot.WithDurableFrames(
            request,
            durable);
        var inboundPayloadBytes =
            ZLinkRemoteActorJoinPackets.MeasureRelocationPayloadBytes(request);
        inboundPayloadBytes = checked(
            inboundPayloadBytes
            + ZLinkRelocationEnvelopeCodec.MeasureEncodedLength(durableEnvelope));
        var actorState = GetOrCreateActorState(request.ActorId);
        if (actorState.Handoff.IsQuarantined(request.HandoffId))
        {
            await _actorSessionManager.RollbackTransferredActorAsync(
                    request.ActorId,
                    CancellationToken.None)
                .ConfigureAwait(false);
            throw new ZLinkActorHandoffRejectedException(
                $"Actor '{request.ActorId}' handoff failed and its quarantined rollback was reconciled.");
        }

        if (_actorHandoffAdmissions.TryGetJoinOutcome(request, spotId, out var terminalReply))
            return terminalReply;

        ZLinkActorRelocationRegistry.TryResolve(
            Registration,
            request.ActorType,
            target.NodeRid,
            out var relocation);
        if (relocation is null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"Actor type '{request.ActorType}' relocation policy is not registered on the target node.");
        ZLinkRelocationPermitPool.ZLinkRelocationPermitLease relocationPermit = default;
        using (relocationPermit)
        {
            var ownsImport = false;
            var createdTransferredActor = false;
            var authorityCommitted = actorState.Handoff.IsAuthorityCommitted(request.HandoffId);
            ZLinkRelocationCapacityFence? capacityFence = null;
            try
            {
                if (!actorState.Handoff.IsKnown(request.HandoffId))
                    capacityFence = _actorHandoffAdmissions.BeginCommit(
                        request,
                        spotId,
                        inboundPayloadBytes);
                var import = await actorState.ExecuteHandoffTransitionAsync(
                        () =>
                        {
                            var owned = actorState.Handoff.Import(request, out var preparation);
                            return (Owned: owned, Preparation: preparation);
                        },
                        cancellationToken)
                    .ConfigureAwait(false);
                if (!import.Owned)
                {
                    // A duplicate admission request only observes the
                    // already published preparation. It must not run the
                    // target lifecycle before source cutover; that work belongs
                    // to the separate completion request.
                    return await import.Preparation.WaitAsync(cancellationToken)
                        .ConfigureAwait(false);
                }
                ownsImport = true;

                await _actorSessionManager.PrepareForTransferredActivationAsync(
                        actorState,
                        cancellationToken)
                    .ConfigureAwait(false);
                // Hosting handoff: the source node still owns the location row, so
                // the local claim may fence it out with Takeover. This path does not
                // call the Entry Spot create callback; transfer materialization is
                // not a new application-level actor creation.
                var creation = await _actorSessionManager.RelocateAndBindActorAsync(
                        request.ActorId,
                        request.ActorType,
                        relocation,
                        ZLinkActorRelocationRegistry.ValidateIncomingPayload(
                            relocation,
                            request.ActorType,
                            request.RelocationContentType,
                            durable.Participant.ApplicationState),
                        request.ActorGeneration,
                        request.ActorAuthorityOwnerGeneration,
                        ZLinkActorClaimMode.StagedRelocation,
                        publishActorRef: false,
                        cancellationToken)
                    .ConfigureAwait(false);
                createdTransferredActor = creation.Created;
                var actorId = request.ActorId;
                var actorRef = actorState.NativeActorRef
                               ?? throw new ZLinkFrameworkException(
                                   ZLinkFrameworkErrorKind.NotFound,
                                   $"Actor '{actorId}' does not have a native Actor ref.");
                if (actorRef.Generation != request.ActorGeneration)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.InvalidOperation,
                        $"Actor '{actorId}' target generation changed during handoff.");
                var boundRoute = ZLinkRemoteActorJoinPackets.DecodeBoundSessionRoute(request);
                if (boundRoute.HasRouteCoordinates && !boundRoute.IsBound)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.ProtocolError,
                        $"Actor '{actorId}' relocation session route has incomplete fencing identity.");
                actorState.StageRelocationSessionRoute(
                    request.HandoffId,
                    boundRoute);
                await PrepareTransferredActorTargetAsync(
                        target,
                        creation.Actor,
                        actorState,
                        cancellationToken)
                    .ConfigureAwait(false);
                // The authority CAS is the visibility boundary. The target
                // lifecycle callback is retryable post-commit work and cannot
                // turn the move back into a source-side rejection.
                var committedAuthority =
                    await PublishTransferredActorAuthorityAsync(
                        actorState,
                        target,
                        request.HandoffId,
                        request.ActorGeneration,
                        ZLinkActorRelocationRoot.Reference(request),
                        durableEnvelope,
                        capacityFence,
                        request.TargetAuthorityOwnerGeneration,
                        cancellationToken)
                    .ConfigureAwait(false);
                actorState.MarkRelocationSessionAuthorityCommitted(
                    request.HandoffId,
                    actorRef,
                    committedAuthority.AuthorityOwnerGeneration,
                    committedAuthority.MeshName,
                    committedAuthority.NodeGeneration,
                    committedAuthority.OwnerLeaseGeneration);
                authorityCommitted = true;
                SchedulePublishedActorRelocationRecovery(durableEnvelope);
                var reply = ZLinkRemoteActorJoinPackets.CreateJoinReply(true, actorRef);
                _actorHandoffAdmissions.RecordJoinOutcome(
                    request,
                    spotId,
                    reply,
                    Registration.DefaultRequestTimeout);
                actorState.Handoff.AcceptCommittedPreparation(
                    request.HandoffId,
                    reply);
                return reply;
            }
            catch (Exception commitFailure)
            {
                if (authorityCommitted
                    || actorState.Handoff.IsAuthorityCommitted(request.HandoffId))
                    throw;

                //  The reply carries only "rejected", so without this the commit
                //  failure that caused it never reaches any log.
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"handoff_commit_failed actor={request.ActorId} spot={spotId} "
                    + $"{commitFailure}");
                var rejected = CreateRejectedHandoffReply(request.ActorId);
                _actorHandoffAdmissions.RejectPreparedJoinOutcome(request, spotId, rejected);
                if (ownsImport)
                    actorState.Handoff.RejectPreparation(request.HandoffId, rejected);
                actorState.AbortRelocationSessionRoute(request.HandoffId);
                try
                {
                    if (!ownsImport)
                    {
                        // A conflicting transaction owns the actor handoff state.
                    }
                    else if (createdTransferredActor)
                    {
                        actorState.Handoff.Quarantine(request.HandoffId);
                        await RollbackPreparedTransferredActorAsync(actorState, CancellationToken.None)
                            .ConfigureAwait(false);
                    }
                    else
                    {
                        actorState.Handoff.AbortImport(request.HandoffId);
                    }
                }
                catch (Exception rollbackFailure)
                {
                    actorState.Handoff.Quarantine(request.HandoffId);
                    throw new AggregateException(commitFailure, rollbackFailure);
                }
                finally
                {
                    await _actorHandoffAdmissions.AbortAsync(
                            request.HandoffId,
                            CancellationToken.None)
                        .ConfigureAwait(false);
                }

                throw new ZLinkActorHandoffRejectedException(
                    $"Actor '{request.ActorId}' handoff commit was rejected.",
                    commitFailure);
            }
        }
    }

    internal async ValueTask CompleteRoutedActorHandoffAsync(
        string spotId,
        ZLinkRemoteActorHandoffCompletionRequest request,
        CancellationToken cancellationToken)
    {
        var authorityStore = Registration.Locations.ResolveStore()
                             ?? throw new ZLinkConfigurationException(
                                 "Actor relocation completion requires an Authority Store.");
        var relocationStore = Registration.Locations.ResolveRelocationStore()
                              ?? throw new ZLinkConfigurationException(
                                  "Actor relocation completion requires a Relocation Store.");
        var ownsRecordedCompletion = _actorHandoffAdmissions.TryBeginCompletion(
            request,
            spotId);
        // Completion state is process-local. A restarted target does not
        // reconstruct the Actor callback or replay frames from a durable
        // completion journal; the object remains unavailable until an
        // explicit application operation recreates it.
        if (!ownsRecordedCompletion) return;
        var publishedRead = await authorityStore.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(request.ActorId),
                cancellationToken)
            .ConfigureAwait(false);
        if (publishedRead is not ZLinkAuthorityReadResult.Found publishedFound
            || !ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                publishedFound.Snapshot.Payload.Span,
                out var publishedManifest))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                $"Actor '{request.ActorId}' relocation reference is not published.",
                retryAdvice: ZLinkRetryAdvice.DoNotRetry);
        var publishedReference = publishedManifest.Reference;
        if (!ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                publishedManifest.ApplicationPayload.Span,
                out var publishedActorAuthority))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                $"Actor '{request.ActorId}' published authority identity is unreadable.",
                retryAdvice: ZLinkRetryAdvice.DoNotRetry);
        var actorState = GetOrCreateActorState(request.ActorId);
        Task<(ZLinkSessionRouteCommitRequest Request, RoutingId SessionOwnerNode)?>?
            sessionRouteCommitTask = null;

        try
        {
            var target = ResolveActorHandoffTarget(spotId)
                         ?? throw new ZLinkFrameworkException(
                             ZLinkFrameworkErrorKind.NotFound,
                             $"Actor '{request.ActorId}' handoff target '{spotId}' is not active during completion.");
            var actorRef = actorState.NativeActorRef
                           ?? throw new ZLinkFrameworkException(
                               ZLinkFrameworkErrorKind.NotFound,
                               $"Actor '{request.ActorId}' does not have a native Actor ref during route commit.");
            var relocationId = Guid.ParseExact(request.HandoffId, "N");
            var actorLocations = RequireActorRelocationLocationLifecycle(
                    LocationLifecycle,
                    request.ActorId)
                .ActorOwnership;
            var durablePhase = await actorLocations
                .ReadTransferredActorAuthorityPhaseAsync(
                    request.ActorId,
                    actorRef.ToNative(publishedActorAuthority.MeshName),
                    cancellationToken)
                .ConfigureAwait(false);
            var authorityWasNormalized = false;
            ZLinkAuthoritySnapshot? normalizedSnapshot = null;
            ZLinkActorRelocationAuthorityPayload? durablePayload = null;
            if (durablePhase is { } durable)
            {
                durablePayload = durable.Phase;
                if (durable.Phase.RelocationId != relocationId
                    || durable.Phase.Phase
                    is < ZLinkActorRelocationAuthorityPhase.Completed)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        $"Actor '{request.ActorId}' source cleanup has not reached durable Completed.");
            }
            else
            {
                var normalizedRead = await authorityStore.ReadAuthorityAsync(
                        ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                            request.ActorId),
                        cancellationToken)
                    .ConfigureAwait(false);
                authorityWasNormalized =
                    normalizedRead is ZLinkAuthorityReadResult.Found normalized
                    && IsCompletedCanonicalActorRelocation(
                        normalized.Snapshot,
                        publishedReference);
                if (authorityWasNormalized
                    && normalizedRead is ZLinkAuthorityReadResult.Found found)
                    normalizedSnapshot = found.Snapshot;
                if (!authorityWasNormalized)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        $"Actor '{request.ActorId}' source cleanup has not reached durable Completed.");
            }
            if (normalizedSnapshot is { } adoptedSnapshot)
                actorLocations.UpdateTrackedSnapshot(
                    request.ActorId,
                    adoptedSnapshot);
            var authorityWasSteady = authorityWasNormalized
                                     || durablePayload?.Phase
                                     == ZLinkActorRelocationAuthorityPhase.Steady;
            var recoveryBoundRoute =
                durablePayload?.BoundSessionRoute.IsBound == true
                    ? durablePayload.BoundSessionRoute
                    : ZLinkRemoteActorJoinPackets.DecodeBoundSessionRoute(
                        request);
            if (recoveryBoundRoute.IsBound
                && !actorState.TryGetStagedRelocationSessionRoute(
                    request.HandoffId,
                    out _))
            {
                var committedAuthority = publishedActorAuthority;
                if (durablePayload is not null
                    && !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                        durablePayload.ApplicationPayload.Span,
                        out committedAuthority))
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DataLost,
                        $"Actor '{request.ActorId}' committed authority identity is unreadable.",
                        retryAdvice: ZLinkRetryAdvice.DoNotRetry);
                actorState.StageRelocationSessionRoute(
                    request.HandoffId,
                    recoveryBoundRoute);
                actorState.MarkRelocationSessionAuthorityCommitted(
                    request.HandoffId,
                    actorRef,
                    publishedFound.Snapshot.AuthorityOwnerGeneration,
                    committedAuthority.MeshName,
                    committedAuthority.NodeGeneration,
                    committedAuthority.OwnerLeaseGeneration);
            }
            // The target temporary queue remains closed to application
            // dispatch, but its bound-session route update must not wait for
            // the joined callback. Otherwise a callback gate can keep the
            // session route sealed and prevent frames from reaching that
            // temporary queue (common spec 20 section 5).
            if (actorState.TryGetCommittedRelocationSessionRoute(
                    request.HandoffId,
                    out _))
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"session_route_update_started actor={request.ActorId}");
                sessionRouteCommitTask = CommitAndUnsealSessionRouteAsync(
                        actorState,
                        request.HandoffId,
                        cancellationToken)
                    .AsTask();
            }
            try
            {
                await CompleteTransferredActorTargetLifecycleAsync(
                        target,
                        actorState,
                        request.HandoffId,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception lifecycleFailure)
            {
                // Authority has already moved. A target lifecycle failure is
                // therefore terminal at the target and must not make the
                // source reopen or roll back its old membership.
                var terminalFailure = lifecycleFailure;
                var routeFailure = await ObserveSessionRouteCommitTaskAsync(
                        sessionRouteCommitTask,
                        "actor-session-route-update-after-lifecycle-failure")
                    .ConfigureAwait(false);
                if (routeFailure is not null)
                    terminalFailure = new AggregateException(
                        lifecycleFailure,
                        routeFailure);
                await DeliverFailedTransferredActorJoinAsync(
                        actorState,
                        request,
                        terminalFailure,
                        cancellationToken)
                    .ConfigureAwait(false);
                if (ownsRecordedCompletion)
                {
                    _actorHandoffAdmissions.RecordCompletion(request, spotId);
                    _actorHandoffAdmissions.Complete(request.HandoffId);
                }
                LogActorHandoff(
                    $"handoff_completion_failed_after_commit actor={request.ActorId} "
                    + $"kind={MapPostCommitActorJoinFailure(terminalFailure)}");
                return;
            }
            var canonicalMaintenance = actorState.Handoff
                .IsCanonicalMaintenanceHandoff(request.HandoffId);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"handoff_recovery_replay_path actor={request.ActorId} "
                + $"handoff={request.HandoffId} canonical={canonicalMaintenance} "
                + $"frames={request.Frames.Count}");
            if (!canonicalMaintenance)
                actorState.Handoff.PrepareImportedReplay(request.Frames);
            if (request.OperationIdHigh != 0 || request.OperationIdLow != 0)
            {
                var actor = actorState.Actor
                            ?? throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.NotFound,
                                $"Actor '{request.ActorId}' has no transferred instance for Join completion.");
                var currentRef = actorState.NativeActorRef
                                 ?? throw new ZLinkFrameworkException(
                                   ZLinkFrameworkErrorKind.NotFound,
                                   $"Actor '{request.ActorId}' has no current reference for Join completion.");
                var reply = request.Reply is { Length: > 0 } payload
                            && request.ReplyContentType is { } contentType
                    ? ZLinkMessage.FromEncoded(contentType, payload, Registration.Codecs)
                    : null;
                await actorState.ExecuteRelocationCompletionAsync(
                        currentRef.Generation,
                        token => actor.OnJoinCompletedAsync(
                            new ZLinkActorJoinCompletion.Accepted(
                                new ZLinkActorJoinOperationId(
                                    request.OperationIdHigh,
                                    request.OperationIdLow),
                                currentRef.ToNative(publishedActorAuthority.MeshName),
                                reply),
                            token),
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            if (canonicalMaintenance)
            {
                await actorState.Handoff.WaitForTargetCompletionAsync(
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            else
            {
                await ReplayFinalTransferredActorHandoffAsync(
                        target,
                        actorState,
                        request.HandoffId,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            var sessionRouteCommit = sessionRouteCommitTask is null
                ? null
                : await sessionRouteCommitTask.ConfigureAwait(false);
            LogActorHandoff(
                $"session_route_commit_{(sessionRouteCommit is null ? "not_required" : "acknowledged")} "
                + $"actor={request.ActorId}");
            if (!authorityWasSteady)
                await actorLocations.AdvanceTransferredActorAuthorityPhaseAsync(
                        request.ActorId,
                        actorRef.ToNative(publishedActorAuthority.MeshName),
                        relocationId,
                        ZLinkActorRelocationAuthorityPhase.Completed,
                        ZLinkActorRelocationAuthorityPhase.Steady,
                        cancellationToken)
                    .ConfigureAwait(false);
            if (!authorityWasNormalized)
                await actorLocations.NormalizeTransferredActorAuthorityAsync(
                        request.ActorId,
                        actorRef.ToNative(publishedActorAuthority.MeshName),
                        relocationId,
                        cancellationToken)
                    .ConfigureAwait(false);
            actorState.Handoff.Complete(request.HandoffId);
            {
                var released = await new ZLinkRelocationPublicationCoordinator(
                            authorityStore,
                            relocationStore)
                        .ReleasePublishedAsync(
                            ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                                request.ActorId),
                            publishedReference,
                            cancellationToken)
                        .ConfigureAwait(false);
                if (released is { } releasedSnapshot)
                    actorLocations.UpdateTrackedSnapshot(
                        request.ActorId,
                        releasedSnapshot);
            }
            if (ownsRecordedCompletion)
            {
                _actorHandoffAdmissions.RecordCompletion(request, spotId);
                _actorHandoffAdmissions.Complete(request.HandoffId);
            }
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"handoff_completion actor={request.ActorId} id={request.HandoffId} frames={request.Frames.Count}");
        }
        catch (Exception handoffFailure)
        {
            var routeFailure = await ObserveSessionRouteCommitTaskAsync(
                    sessionRouteCommitTask,
                    "actor-session-route-update-after-handoff-failure")
                .ConfigureAwait(false);
            if (ownsRecordedCompletion)
                _actorHandoffAdmissions.CancelCompletion(request, spotId);
            if (routeFailure is not null
                && !ReferenceEquals(routeFailure, handoffFailure))
                handoffFailure = new AggregateException(
                    handoffFailure,
                    routeFailure);
            ExceptionDispatchInfo.Capture(handoffFailure).Throw();
            throw;
        }
    }

    internal static bool IsCompletedCanonicalActorRelocation(
        ZLinkAuthoritySnapshot snapshot,
        string expectedReference) =>
        ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
            snapshot.Payload.Span,
            out var publication)
        && publication.Phase
           == (byte)ZLinkStandaloneActorCanonicalPhase.Completed
        && publication.SourceCleanupState == 1
        && string.Equals(
            publication.RelocationReference,
            expectedReference,
            StringComparison.Ordinal)
        && ZLinkActorAuthorityPayloadCodec.TryDecode(
            publication.SteadyAuthorityPayload.Span,
            out _);

    //  Relocation의 bound session route는 target 쪽에서만 풀린다. 완료 명령과
    //  reconcile 두 경로가 같은 꼬리를 부르지 않으면, 한쪽이 stage를 먼저
    //  치웠을 때 seal이 영구히 남아 이후 frame이 전부 거절된다.
    internal async ValueTask FinishRelocationTargetAsync(
        ZLinkActorRuntimeState actorState,
        string meshName,
        string handoffId,
        CancellationToken cancellationToken)
    {
        //  Staged relocation은 target에서 claim을 건너뛴다(`ZLinkActorClaimMode
        //  .StagedRelocation`). 이동이 끝난 뒤에도 아무도 adopt하지 않으면 target의
        //  location owner가 그 actor를 모르는 채로 남아, 이후 spot join이
        //  `not tracked by this location owner`로 실패한다.
        if (LocationLifecycle is { } locations
            && actorState.ActorType is { } actorType
            && actorState.NativeActorRef is { } nativeActor)
            await locations.ActorOwnership.AdoptCommittedActorAuthorityAsync(
                    actorState.ActorId,
                    actorType,
                    nativeActor.ToNative(meshName),
                    _ => DeactivateActorOnOwnershipLossAsync(actorState.ActorId),
                    cancellationToken)
                .ConfigureAwait(false);
        await FinishRelocationSessionRouteAsync(
                actorState,
                handoffId,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask FinishRelocationSessionRouteAsync(
        ZLinkActorRuntimeState actorState,
        string handoffId,
        CancellationToken cancellationToken)
    {
        //  이미 다른 경로가 끝냈으면 commit이 null이라 그대로 no-op이다.
        var commit = await CommitAndUnsealSessionRouteAsync(
                actorState,
                handoffId,
                cancellationToken)
            .ConfigureAwait(false);
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"relocation_session_route commit={(commit is null ? "none" : "present")} "
            + $"actor={actorState.ActorId} handoff={handoffId}");
    }

    private async ValueTask<(
        ZLinkSessionRouteCommitRequest Request,
        RoutingId SessionOwnerNode)?>
        CommitAndUnsealSessionRouteAsync(
            ZLinkActorRuntimeState actorState,
            string handoffId,
            CancellationToken cancellationToken)
    {
        var commit = await CommitCompletedSessionRouteAsync(
                actorState,
                handoffId,
                cancellationToken)
            .ConfigureAwait(false);
        if (commit is not { } completedRoute)
            return null;

        await UnsealCompletedSessionRouteAsync(
                completedRoute.Request,
                completedRoute.SessionOwnerNode,
                cancellationToken)
            .ConfigureAwait(false);
        // Keep the pending target route until the session owner confirms the
        // seal is removed. This preserves the exact request for a retry when
        // commit succeeded but unseal did not.
        actorState.CompleteRelocationSessionRoute(handoffId);
        return completedRoute;
    }

    private static async ValueTask<Exception?> ObserveSessionRouteCommitTaskAsync(
        Task<(ZLinkSessionRouteCommitRequest Request, RoutingId SessionOwnerNode)?>? task,
        string operation)
    {
        if (task is null)
            return null;
        try
        {
            await task.ConfigureAwait(false);
            return null;
        }
        catch (Exception exception)
        {
            ZLinkFrameworkDebugLog.TaskFailure(operation, exception);
            return exception;
        }
    }

    private async ValueTask<(
        ZLinkSessionRouteCommitRequest Request,
        RoutingId SessionOwnerNode)?>
        CommitCompletedSessionRouteAsync(
        ZLinkActorRuntimeState actorState,
        string handoffId,
        CancellationToken cancellationToken)
    {
        if (!actorState.TryGetCommittedRelocationSessionRoute(
                handoffId,
                out var pending))
            return null;

        var route = pending.Route;
        var targetActor = pending.TargetActor
                          ?? throw new InvalidOperationException(
                              "Session route commit requires a target Actor ref.");
        var request = new ZLinkSessionRouteCommitRequest(
            actorState.ActorId,
            route.BindingToken!,
            route.BindingGeneration,
            route.ObjectGeneration,
            route.AuthorityOwnerGeneration,
            pending.TargetAuthorityOwnerGeneration,
            route.MeshName!,
            pending.TargetMeshName!,
            route.TargetNodeGeneration,
            pending.TargetNodeGeneration,
            route.OwnerLeaseGeneration,
            pending.TargetOwnerLeaseGeneration,
            route.SessionOwnerNodeGeneration,
            route.AcceptedHighWater,
            handoffId,
            targetActor.NodeRid.ToHex());

        var meshName = route.MeshName
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorState.ActorId}' session route has no Mesh.");
        var sessionOwnerNode = route.NodeRid!.Value;
        var localNode = GetMeshNodeRuntime(meshName).Node.RoutingId;
        ZLinkSessionRouteCommitReply reply;
        if (sessionOwnerNode == localNode)
        {
            var result = CommitSessionActorRoute(
                new ZLinkSessionRouteCommit(
                    request.ActorId,
                    request.BindingToken,
                    request.BindingGeneration,
                    request.ObjectGeneration,
                    request.PreviousAuthorityOwnerGeneration,
                    request.TargetAuthorityOwnerGeneration,
                    request.PreviousMeshName,
                    request.TargetMeshName,
                    request.PreviousTargetNodeGeneration,
                    request.TargetNodeGeneration,
                    request.PreviousOwnerLeaseGeneration,
                    request.TargetOwnerLeaseGeneration,
                    request.SessionOwnerNodeGeneration,
                    request.AcceptedHighWater,
                    request.HandoffId,
                    targetActor.ToNative(request.TargetMeshName)));
            reply = new ZLinkSessionRouteCommitReply(
                result.Acknowledged,
                result.AcceptedHighWater);
        }
        else
        {
            reply = await RequestSessionRouteCommitAsync(
                    meshName,
                    sessionOwnerNode,
                    request,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        if (!reply.Acknowledged
            || reply.AcceptedHighWater < route.AcceptedHighWater)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actorState.ActorId}' session route commit was fenced by its binding identity.");

        return (request, sessionOwnerNode);
    }

    private async ValueTask UnsealCompletedSessionRouteAsync(
        ZLinkSessionRouteCommitRequest request,
        RoutingId sessionOwnerNode,
        CancellationToken cancellationToken)
    {
        var meshName = request.PreviousMeshName;
        var localNode = GetMeshNodeRuntime(meshName).Node.RoutingId;
        bool acknowledged;
        if (sessionOwnerNode == localNode)
        {
            acknowledged = UnsealCommittedSessionActorRoute(
                new ZLinkSessionRouteCommit(
                    request.ActorId,
                    request.BindingToken,
                    request.BindingGeneration,
                    request.ObjectGeneration,
                    request.PreviousAuthorityOwnerGeneration,
                    request.TargetAuthorityOwnerGeneration,
                    request.PreviousMeshName,
                    request.TargetMeshName,
                    request.PreviousTargetNodeGeneration,
                    request.TargetNodeGeneration,
                    request.PreviousOwnerLeaseGeneration,
                    request.TargetOwnerLeaseGeneration,
                    request.SessionOwnerNodeGeneration,
                    request.AcceptedHighWater,
                    request.HandoffId,
                    new ActorRef(
                        request.ActorId,
                        request.ObjectGeneration,
                        request.TargetMeshName,
                        RoutingId.FromHex(request.TargetNodeRid))));
        }
        else
        {
            var reply = await RequestSessionRouteUnsealAsync(
                    meshName,
                    sessionOwnerNode,
                    new ZLinkSessionRouteUnsealRequest(
                        request.ActorId,
                        request.BindingToken,
                        request.BindingGeneration,
                        request.ObjectGeneration,
                        request.PreviousAuthorityOwnerGeneration,
                        request.TargetAuthorityOwnerGeneration,
                        request.PreviousMeshName,
                        request.TargetMeshName,
                        request.PreviousTargetNodeGeneration,
                        request.TargetNodeGeneration,
                        request.PreviousOwnerLeaseGeneration,
                        request.TargetOwnerLeaseGeneration,
                        request.SessionOwnerNodeGeneration,
                        request.AcceptedHighWater,
                        request.HandoffId,
                        request.TargetNodeRid),
                    cancellationToken)
                .ConfigureAwait(false);
            acknowledged = reply.Acknowledged;
        }
        if (!acknowledged)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{request.ActorId}' session ingress could not unseal before steady normalization.");
    }

    private void SchedulePublishedActorRelocationRecovery(
        ZLinkRelocationEnvelope envelope)
    {
        //  Spec 15 requires commit-time recovery to deliver Accepted from the
        //  target even when the source dies. Both the duplicate-watch skip and
        //  a detached task that never runs look identical from outside.
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"recovery_scheduled aggregate={envelope.AggregateId} "
            + $"already_watched={_publishedActorRecoveryWatches.ContainsKey(envelope.AggregateId)}");
        if (!_publishedActorRecoveryWatches.TryAdd(
                envelope.AggregateId, 0))
            return;
        if (TryRunDetached(
                "actor-published-relocation-recovery",
                async token =>
                {
                    try
                    {
                        await ZLinkReconciliationRunner.RunAsync(
                                async retryToken =>
                                {
                                    var locationStore =
                                        Registration.Locations.ResolveStore()
                                        ?? throw new ZLinkConfigurationException(
                                            "Actor relocation recovery requires a Location Store.");
                                    var relocationStore =
                                        Registration.Locations
                                            .ResolveRelocationStore()
                                        ?? throw new ZLinkConfigurationException(
                                            "Actor relocation recovery requires a Relocation Store.");
                                    var candidate =
                                        await new ZLinkRelocationStartupRecovery(
                                                locationStore,
                                                relocationStore)
                                            .TryReadExactPublishedAsync(
                                                envelope,
                                                retryToken)
                                            .ConfigureAwait(false);
                                    if (candidate is null)
                                        return;
                                    await _standaloneActorRelocationRuntime
                                        .RecoverPublishedAsync(
                                            candidate,
                                            retryToken)
                                        .ConfigureAwait(false);
                                    await RecoverCanonicalRemoteJoinCompletionAsync(
                                            candidate,
                                            retryToken)
                                        .ConfigureAwait(false);
                                },
                                exception => ZLinkFrameworkDebugLog.SpotDiscovery(
                                        "published Actor relocation recovery "
                                        + $"id={envelope.AggregateId:N}: "
                                        + exception.Message),
                                token,
                                static exception =>
                                    exception is ZLinkRelocationDataLostException
                                    or ZLinkFrameworkException
                                    {
                                        Kind: ZLinkFrameworkErrorKind.DataLost
                                    },
                                Registration.Locations.Options.PollingInterval)
                            .ConfigureAwait(false);
                    }
                    finally
                    {
                        _publishedActorRecoveryWatches.TryRemove(
                            envelope.AggregateId,
                            out _);
                    }
                }))
            return;
        _publishedActorRecoveryWatches.TryRemove(envelope.AggregateId, out _);
        throw new InvalidOperationException(
            "Published Actor relocation recovery could not be scheduled.");
    }

    private async ValueTask RecoverCanonicalRemoteJoinCompletionAsync(
        ZLinkRelocationRecoveryCandidate candidate,
        CancellationToken cancellationToken)
    {
        var participant = candidate.Envelope.Participants.Single();
        var canonical = ZLinkCanonicalParticipantRecoveryCodec.Decode(
            participant.RecoveryPayload.Span);
        var sourceFence = ZLinkActorRelocationSourceFenceCodec.Decode(
            canonical.MembershipMutation.Span);
        //  Printed before the branch, with the values it tests: an absent
        //  trace inside the branch would not say whether recovery metadata was
        //  missing or the path was never reached.
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"recovery_metadata operation_empty={canonical.OperationRecovery.IsEmpty} "
            + $"legacy_empty={sourceFence.LegacyRemoteJoinRecovery.IsEmpty}");
        if (canonical.OperationRecovery.IsEmpty
            && sourceFence.LegacyRemoteJoinRecovery.IsEmpty)
            return;
        ZLinkActorRelocationRecoveryRecord recovery;
        try
        {
            recovery = ZLinkActorRemoteJoinRecoveryCodec.Decode(
                canonical.OperationRecovery.Span,
                sourceFence.LegacyRemoteJoinRecovery.Span);
        }
        catch (Exception error) when (error is InvalidDataException)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DataLost,
                "Canonical Actor Join recovery metadata is malformed.",
                retryAdvice: ZLinkRetryAdvice.DoNotRetry,
                error);
        }
        var wire = recovery.Request;
        //  Only one call separates this from the completion delivery, so a
        //  trace on each side of it isolates the stall.
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"recovery_validating actor={wire.ActorId} handoff={wire.HandoffId}");
        await ValidateCanonicalRemoteJoinRecoveryAsync(
                candidate,
                participant,
                canonical,
                sourceFence,
                recovery,
                cancellationToken)
            .ConfigureAwait(false);
        //  End of the recovery path: this is what delivers the Accepted
        //  completion spec 15 requires after a post-commit source failure.
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"recovery_completing candidate={candidate}");
        await CompleteRoutedActorHandoffAsync(
                recovery.TargetSpotId,
                new ZLinkRemoteActorHandoffCompletionRequest(
                    wire.ActorId,
                    wire.HandoffId,
                    wire.SourceSpotId,
                    wire.SourceNodeRid,
                    recovery.TargetSpotId,
                    [],
                    recovery.OperationIdHigh,
                    recovery.OperationIdLow,
                    recovery.ReplyContentType,
                    recovery.Reply,
                    wire.BoundSessionNodeRid,
                    wire.BoundSessionRid,
                    wire.BoundSessionBindingToken,
                    wire.BoundSessionBindingGeneration,
                    wire.BoundSessionObjectGeneration,
                    wire.BoundSessionAuthorityOwnerGeneration,
                    wire.BoundSessionMeshName,
                    wire.BoundSessionTargetNodeGeneration,
                    wire.BoundSessionOwnerLeaseGeneration,
                    wire.BoundSessionOwnerNodeGeneration,
                    wire.BoundSessionAcceptedHighWater),
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask ValidateCanonicalRemoteJoinRecoveryAsync(
        ZLinkRelocationRecoveryCandidate candidate,
        ZLinkRelocationParticipantEnvelope participant,
        ZLinkCanonicalParticipantRecovery canonical,
        ZLinkActorRelocationSourceFence sourceFence,
        ZLinkActorRelocationRecoveryRecord recovery,
        CancellationToken cancellationToken)
    {
        var wire = recovery.Request;
        var actorKey =
            ZLinkActorAuthorityPayloadCodec.AuthorityKey(wire.ActorId);
        //  This validation throws into a detached recovery task, where the
        //  exception reaches no caller. Name it before letting it propagate.
        try
        {
            ValidateCanonicalRemoteJoinRecoveryIdentity(
                candidate,
                participant,
                canonical,
                sourceFence,
                recovery);
        }
        catch (Exception identityFailure)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"recovery_identity_failed {identityFailure}");
            throw;
        }
        var matchingAuthorities = candidate.Authorities
            .Where(entry => entry.Key == actorKey)
            .ToArray();
        //  Printed before the guard with the value it tests.
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"recovery_authorities count={matchingAuthorities.Length}");
        if (matchingAuthorities.Length != 1)
            throw RemoteJoinRecoveryMismatch(
                wire.ActorId,
                "published Actor authority cardinality");
        var authority = matchingAuthorities[0];
        var targetRid = RoutingId.From(recovery.TargetNodeRid);
        var aggregateBytes =
            candidate.Envelope.AggregateId.ToByteArray(bigEndian: true);
        if (authority.Snapshot.ObjectGeneration
               != participant.ObjectGeneration
            || authority.Snapshot.AuthorityOwnerGeneration
               != recovery.TargetAuthorityOwnerGeneration
            || authority.Snapshot.Allocation.ObjectKind
               != ZLinkPlacementObjectKind.Actor
            || !string.Equals(
                authority.Snapshot.Allocation.StableType,
                wire.ActorType,
                StringComparison.Ordinal)
            || authority.Snapshot.Allocation.Descriptor.Rid != targetRid
            || authority.Snapshot.Allocation.DescriptorLifecycleGeneration
               != recovery.TargetNodeGeneration
            || !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                authority.Snapshot.Payload.Span,
                out var publication)
            || publication.RelocationReference
               != candidate.Reference.Reference
            || publication.RelocationChecksumCrc32c
               != candidate.Reference.ChecksumCrc32c
            || publication.RelocationHigh
               != System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(
                   aggregateBytes.AsSpan(0, 8))
            || publication.RelocationLow
               != System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(
                   aggregateBytes.AsSpan(8, 8))
            || !string.Equals(
                publication.State.TargetNodeRid,
                targetRid.ToHex(),
                StringComparison.Ordinal)
            || publication.State.TargetNodeGeneration
               != recovery.TargetNodeGeneration
            || authority.Snapshot.OwnerId != publication.TargetOwnerId
            || authority.Snapshot.OwnerLeaseGeneration <= 0
            || checked((ulong)authority.Snapshot.OwnerLeaseGeneration)
               != publication.TargetOwnerLeaseGeneration
            || !ZLinkActorAuthorityPayloadCodec.TryDecode(
                publication.SteadyAuthorityPayload.Span,
                out var steady)
            || steady.ActorId != wire.ActorId
            || steady.StableType != wire.ActorType
            || steady.CurrentSpotId != recovery.TargetSpotId
            || steady.CurrentSpotGeneration
               != recovery.TargetSpotGeneration
            || steady.NodeRid != targetRid
            || steady.NodeGeneration != recovery.TargetNodeGeneration
            || steady.OwnerId != publication.TargetOwnerId
            || steady.OwnerLeaseGeneration
               != publication.TargetOwnerLeaseGeneration)
            throw RemoteJoinRecoveryMismatch(
                wire.ActorId,
                "published Actor authority or target owner fence");

        if (ResolveActorHandoffTarget(recovery.TargetSpotId)
                is not { } target
            || target.NodeRid != targetRid)
            throw RemoteJoinRecoveryMismatch(
                wire.ActorId,
                "target Spot runtime fence");
        var targetNodeGeneration = GetSpotNodeRuntime(target.NodeRid)
            .Node
            .MeshStatus()
            .LifecycleGeneration;
        var targetSpotGeneration = target.UserSpot?.ObjectGeneration
                                   ?? target.EntrySpot?.ObjectGeneration
                                   ?? 0;
        if (targetNodeGeneration != recovery.TargetNodeGeneration
            || targetSpotGeneration != recovery.TargetSpotGeneration)
            throw RemoteJoinRecoveryMismatch(
                wire.ActorId,
                "target Spot generation");
        if (target.EntrySpot is not null)
        {
            if (wire.TargetSpotAuthorityOwnerGeneration
                != targetNodeGeneration)
                throw RemoteJoinRecoveryMismatch(
                    wire.ActorId,
                    "target Entry Spot owner fence");
            return;
        }

        var locationStore = Registration.Locations.ResolveStore()
                            ?? throw new ZLinkConfigurationException(
                                "Actor relocation recovery requires a Location Store.");
        var targetSpot = await locationStore.ReadAuthorityAsync(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(
                    recovery.TargetSpotId),
                cancellationToken)
            .ConfigureAwait(false);
        if (targetSpot is not ZLinkAuthorityReadResult.Found targetFound
            || targetFound.Snapshot.ObjectGeneration
               != recovery.TargetSpotGeneration
            || targetFound.Snapshot.AuthorityOwnerGeneration
               != wire.TargetSpotAuthorityOwnerGeneration
            || targetFound.Snapshot.Allocation.ObjectKind
               != ZLinkPlacementObjectKind.UserSpot
            || targetFound.Snapshot.Allocation.Descriptor.Rid != targetRid
            || targetFound.Snapshot.Allocation.DescriptorLifecycleGeneration
               != recovery.TargetNodeGeneration)
            throw RemoteJoinRecoveryMismatch(
                wire.ActorId,
                "target User Spot authority fence");
    }

    internal static void ValidateCanonicalRemoteJoinRecoveryIdentity(
        ZLinkRelocationRecoveryCandidate candidate,
        ZLinkRelocationParticipantEnvelope participant,
        ZLinkCanonicalParticipantRecovery canonical,
        ZLinkActorRelocationSourceFence sourceFence,
        ZLinkActorRelocationRecoveryRecord recovery)
    {
        var wire = recovery.Request;
        var actorKey =
            ZLinkActorAuthorityPayloadCodec.AuthorityKey(wire.ActorId);
        var handoffMatches = Guid.TryParseExact(
                                 wire.HandoffId,
                                 "N",
                                 out var handoffId)
                             && handoffId
                             == candidate.Envelope.AggregateId;
        RoutingId sourceNodeRid;
        try
        {
            sourceNodeRid = RoutingId.From(wire.SourceNodeRid);
        }
        catch (Exception error) when (error is ArgumentException)
        {
            throw RemoteJoinRecoveryMismatch(
                wire.ActorId,
                "source node identity",
                error);
        }
        //  One compound guard covers a dozen axes; print them so a mismatch
        //  names the axis instead of the whole condition.
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"recovery_identity handoff_matches={handoffMatches} "
            + $"key={participant.AuthorityKey == actorKey} "
            + $"kind={participant.ObjectKind} "
            + $"obj_gen={participant.ObjectGeneration}/{wire.ActorGeneration} "
            + $"auth_gen={participant.AuthorityOwnerGeneration}/{wire.ActorAuthorityOwnerGeneration} "
            + $"canon_key={canonical.AuthorityKey == participant.AuthorityKey} "
            + $"canon_obj_gen={canonical.ObjectGeneration}/{participant.ObjectGeneration} "
            + $"canon_auth_gen={canonical.AuthorityOwnerGeneration}/{participant.AuthorityOwnerGeneration} "
            + $"stable_type={canonical.StableType}/{wire.ActorType} "
            + $"reloc_ref={wire.RelocationReference} crc={wire.RelocationChecksumCrc32c} "
            + $"agg_id={wire.RelocationAggregateId == candidate.Envelope.AggregateId} "
            + $"agg_gen={wire.RelocationAggregateGeneration}/{candidate.Envelope.AggregateGeneration} "
            + $"digest_len={wire.RelocationInventoryDigest.Length} "
            + $"digest_zero={!wire.RelocationInventoryDigest.Any(static v => v != 0)} "
            + $"src_rid={sourceNodeRid == sourceFence.NodeRid} "
            + $"env_ref_id={candidate.Envelope.AggregateId == candidate.Reference.AggregateId} "
            + $"env_ref_gen={candidate.Envelope.AggregateGeneration == candidate.Reference.AggregateGeneration} "
            + $"env_ref_digest={candidate.Envelope.InventoryDigest.Span.SequenceEqual(candidate.Reference.InventoryDigest.Span)} "
            + $"env_digest={Convert.ToHexString(candidate.Envelope.InventoryDigest.Span)[..16]} "
            + $"ref_digest={Convert.ToHexString(candidate.Reference.InventoryDigest.Span)[..16]} "
            + $"env_len={candidate.Envelope.InventoryDigest.Length} ref_len={candidate.Reference.InventoryDigest.Length}");
        if (participant.AuthorityKey != actorKey
            || participant.ObjectKind != ZLinkPlacementObjectKind.Actor
            || participant.ObjectGeneration != wire.ActorGeneration
            || participant.AuthorityOwnerGeneration
               != wire.ActorAuthorityOwnerGeneration
            || canonical.AuthorityKey != participant.AuthorityKey
            || canonical.ObjectKind != participant.ObjectKind
            || canonical.ObjectGeneration != participant.ObjectGeneration
            || canonical.AuthorityOwnerGeneration
               != participant.AuthorityOwnerGeneration
            || !string.Equals(
                canonical.StableType,
                wire.ActorType,
                StringComparison.Ordinal)
            || !string.Equals(
                wire.RelocationReference,
                "pending",
                StringComparison.Ordinal)
            || wire.RelocationChecksumCrc32c != 0
            || wire.RelocationAggregateId
               != candidate.Envelope.AggregateId
            || wire.RelocationAggregateGeneration
               != candidate.Envelope.AggregateGeneration
            || wire.RelocationInventoryDigest.Length != 32
            || wire.RelocationInventoryDigest.Any(static value => value != 0)
            || !handoffMatches
            || sourceNodeRid != sourceFence.NodeRid
            || candidate.Envelope.AggregateId
               != candidate.Reference.AggregateId
            || candidate.Envelope.AggregateGeneration
               != candidate.Reference.AggregateGeneration
            || !candidate.Envelope.InventoryDigest.Span.SequenceEqual(
                candidate.Reference.InventoryDigest.Span))
            throw RemoteJoinRecoveryMismatch(
                wire.ActorId,
                "participant or aggregate identity");
    }

    private static ZLinkFrameworkException RemoteJoinRecoveryMismatch(
        string actorId,
        string field,
        Exception? error = null) =>
        new(
            ZLinkFrameworkErrorKind.DataLost,
            $"Actor '{actorId}' canonical Join recovery mismatches its {field}.",
            retryAdvice: ZLinkRetryAdvice.DoNotRetry,
            innerException: error);

    // Kept as a small reconciliation primitive for protocol tests and
    // same-process callers. Runtime startup and Actor activation no longer
    // schedule this operation from durable completion state.
    internal static async ValueTask RunDeferredJoinCompletionRecoveryAsync(
        Func<CancellationToken, ValueTask> recover,
        Action<Exception> report,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(recover);
        ArgumentNullException.ThrowIfNull(report);
        await ZLinkReconciliationRunner.RunAsync(
                recover,
                report,
                cancellationToken,
                static exception =>
                    exception is OperationCanceledException
                    || exception is ZLinkRelocationDataLostException
                    || exception is ZLinkFrameworkException
                    {
                        RetryAdvice: ZLinkRetryAdvice.DoNotRetry
                    })
            .ConfigureAwait(false);
    }

    internal async ValueTask RecoverPublishedRelocationsAsync(
        CancellationToken cancellationToken)
    {
        if (Registration.Locations.ResolveStore()
                is not { } locationStore
            || Registration.Locations.ResolveRelocationStore()
                is not { } relocationStore)
            return;
        await new ZLinkRelocationStartupRecovery(
                locationStore,
                relocationStore)
            .RecoverAsync(
                async (candidate, token) =>
                {
                    if (candidate.Authorities.Any(
                            static entry =>
                                entry.Snapshot.Allocation.ObjectKind
                                is ZLinkPlacementObjectKind.UserSpot
                                or ZLinkPlacementObjectKind.InstanceSpot))
                    {
                        if (Services.GetService(
                                typeof(ZLinkSpotRetireTargetRuntime))
                            is ZLinkSpotRetireTargetRuntime spotRecovery)
                            await spotRecovery.RecoverPublishedAsync(
                                    candidate,
                                    token)
                                .ConfigureAwait(false);
                        return;
                    }
                    var actorAuthority = candidate.Authorities.SingleOrDefault(
                        static entry => entry.Snapshot.Allocation.ObjectKind
                                        == ZLinkPlacementObjectKind.Actor);
                    if (actorAuthority is null)
                        return; // User Spot aggregate recovery is owned by its target bridge.
                    if (candidate.Authorities.Count != 1)
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DataLost,
                            "Standalone Actor relocation root contains another authority.",
                            retryAdvice: ZLinkRetryAdvice.DoNotRetry);
                    if (ZLinkStandaloneActorRelocationRuntime.OwnsRecovery(
                            candidate))
                    {
                        await _standaloneActorRelocationRuntime
                            .RecoverPublishedAsync(candidate, token)
                            .ConfigureAwait(false);
                        await RecoverCanonicalRemoteJoinCompletionAsync(
                                candidate,
                                token)
                            .ConfigureAwait(false);
                        return;
                    }
                    if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                            actorAuthority.Snapshot.Payload.Span,
                            out var publication)
                        || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                            publication.ApplicationPayload.Span,
                            out var actorPayload))
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DataLost,
                            $"Actor authority '{actorAuthority.Key.Value}' has an invalid relocation payload.",
                            retryAdvice: ZLinkRetryAdvice.DoNotRetry);
                    var hasPhase =
                        ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                            publication.ApplicationPayload.Span,
                            out _);
                    var localNode = _state?.SpotNodes.Values.SingleOrDefault(
                        node => node.Node.RoutingId == actorPayload.NodeRid);
                    if (localNode is null
                        || actorPayload.NodeGeneration
                        != localNode.Node.MeshStatus().LifecycleGeneration)
                        return;

                    var templateParticipant =
                        candidate.Envelope.Participants.Single();
                    ZLinkActorRelocationRecoveryRecord recovery;
                    try
                    {
                        recovery = System.Text.Json.JsonSerializer.Deserialize<
                                       ZLinkActorRelocationRecoveryRecord>(
                                       templateParticipant.RecoveryPayload.Span)
                                   ?? throw new System.Text.Json.JsonException();
                    }
                    catch (Exception error) when (error
                                                  is System.Text.Json.JsonException
                                                  or NotSupportedException)
                    {
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DataLost,
                            "Actor relocation recovery metadata is malformed.",
                            retryAdvice: ZLinkRetryAdvice.DoNotRetry,
                            error);
                    }
                    var wire = recovery.Request with
                    {
                        RelocationReference =
                            candidate.Reference.Reference,
                        RelocationChecksumCrc32c =
                            candidate.Reference.ChecksumCrc32c,
                        RelocationAggregateId =
                            candidate.Reference.AggregateId,
                        RelocationAggregateGeneration =
                            candidate.Reference.AggregateGeneration,
                        RelocationInventoryDigest =
                            candidate.Reference.InventoryDigest.ToArray(),
                        HandoffFrames = []
                    };
                    if (!recovery.TargetNodeRid.AsSpan().SequenceEqual(
                            actorPayload.NodeRid.ToBytes())
                        || recovery.TargetNodeGeneration
                           != actorPayload.NodeGeneration
                        || recovery.TargetSpotGeneration
                           != actorPayload.CurrentSpotGeneration
                        || recovery.TargetAuthorityOwnerGeneration
                           != actorAuthority.Snapshot.AuthorityOwnerGeneration)
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DataLost,
                            $"Actor '{wire.ActorId}' durable target fence does not match its published authority.",
                            retryAdvice: ZLinkRetryAdvice.DoNotRetry);
                    ZLinkActorRelocationRegistry.TryResolve(
                        Registration,
                        wire.ActorType,
                        actorPayload.NodeRid,
                        out var recoveredRelocation);
                    if (recoveredRelocation is null
                        || !_relocationPermits.TryAcquire(
                            ZLinkRelocationPermitRequest.Inbound(
                                wire.ReservedPayloadBytes,
                                restore: recoveredRelocation.PolicyKind == 2),
                            out var recoveredReservation))
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Unavailable,
                            $"Actor '{wire.ActorId}' recovery target reservation is busy.");
                    try
                    {
                        _actorHandoffAdmissions.RegisterRecoveredReservation(
                            wire,
                            recovery.TargetSpotId,
                            DateTimeOffset.UtcNow + Registration.DefaultRequestTimeout,
                            recoveredReservation);
                    }
                    catch
                    {
                        recoveredReservation.Dispose();
                        throw;
                    }
                    await JoinRoutedActorAsync(
                            recovery.TargetSpotId,
                            wire,
                            token)
                        .ConfigureAwait(false);
                    var actorRef = GetOrCreateActorState(wire.ActorId)
                                       .NativeActorRef
                                   ?? throw new ZLinkFrameworkException(
                                       ZLinkFrameworkErrorKind.NotFound,
                                       $"Actor '{wire.ActorId}' was not restored during relocation recovery.");
                    var ownership = RequireActorRelocationLocationLifecycle(
                            LocationLifecycle,
                            wire.ActorId)
                        .ActorOwnership;
                    var durable = await ownership
                        .ReadTransferredActorAuthorityPhaseAsync(
                            wire.ActorId,
                            actorRef.ToNative(actorPayload.MeshName),
                            token)
                        .ConfigureAwait(false);
                    if (hasPhase
                        && (durable is null
                            || durable.Value.Phase.RelocationId
                            != candidate.Reference.AggregateId))
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DataLost,
                            $"Actor '{wire.ActorId}' lost its committed relocation phase.",
                            retryAdvice: ZLinkRetryAdvice.DoNotRetry);
                    if (durable?.Phase.Phase
                        == ZLinkActorRelocationAuthorityPhase.Activated)
                        await ownership.AdvanceTransferredActorAuthorityPhaseAsync(
                                wire.ActorId,
                                actorRef.ToNative(actorPayload.MeshName),
                                candidate.Reference.AggregateId,
                                ZLinkActorRelocationAuthorityPhase.Activated,
                                ZLinkActorRelocationAuthorityPhase.Cleaning,
                                token)
                            .ConfigureAwait(false);
                    if (hasPhase)
                    {
                        durable = await ownership
                            .ReadTransferredActorAuthorityPhaseAsync(
                            wire.ActorId,
                            actorRef.ToNative(actorPayload.MeshName),
                                token)
                            .ConfigureAwait(false);
                        if (durable?.Phase.Phase
                            == ZLinkActorRelocationAuthorityPhase.Cleaning)
                            await ownership.AdvanceTransferredActorAuthorityPhaseAsync(
                                    wire.ActorId,
                                    actorRef.ToNative(actorPayload.MeshName),
                                    candidate.Reference.AggregateId,
                                    ZLinkActorRelocationAuthorityPhase.Cleaning,
                                    ZLinkActorRelocationAuthorityPhase.Completed,
                                    token)
                                .ConfigureAwait(false);
                    }
                    await CompleteRoutedActorHandoffAsync(
                            recovery.TargetSpotId,
                            new ZLinkRemoteActorHandoffCompletionRequest(
                                wire.ActorId,
                                wire.HandoffId,
                                wire.SourceSpotId,
                                wire.SourceNodeRid,
                                recovery.TargetSpotId,
                                [],
                                recovery.OperationIdHigh,
                                recovery.OperationIdLow,
                                recovery.ReplyContentType,
                                recovery.Reply,
                                wire.BoundSessionNodeRid,
                                wire.BoundSessionRid,
                                wire.BoundSessionBindingToken,
                                wire.BoundSessionBindingGeneration,
                                wire.BoundSessionObjectGeneration,
                                wire.BoundSessionAuthorityOwnerGeneration,
                                wire.BoundSessionMeshName,
                                wire.BoundSessionTargetNodeGeneration,
                                wire.BoundSessionOwnerLeaseGeneration,
                                wire.BoundSessionOwnerNodeGeneration,
                                wire.BoundSessionAcceptedHighWater),
                            token)
                        .ConfigureAwait(false);
                },
                async (entry, token) =>
                {
                    if (entry.Snapshot.Allocation.ObjectKind
                        != ZLinkPlacementObjectKind.Actor)
                        return;
                    if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                            entry.Snapshot.Payload.Span,
                            out var preparing)
                        || preparing.Phase != 1
                        || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                            entry.Snapshot.Payload.Span,
                            out var actorPayload))
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DataLost,
                            $"Actor authority '{entry.Key.Value}' has an invalid Preparing marker.",
                            retryAdvice: ZLinkRetryAdvice.DoNotRetry);
                    var takeover =
                        new ZLinkStandaloneActorRelocationTakeoverCoordinator(
                            this,
                            _actorSessionManager,
                            Registration);
                    if (await takeover.HasLiveRemoteRecoveryOwnerAsync(
                            preparing,
                            actorPayload.MeshName,
                            token)
                        .ConfigureAwait(false))
                        return;
                    await new ZLinkStandaloneActorRelocationPrecommitCoordinator(
                            locationStore)
                        .AbortPreparingAsync(
                            entry.Key,
                            DecodeCanonicalRelocationId(preparing),
                            token)
                        .ConfigureAwait(false);
                },
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static Guid DecodeCanonicalRelocationId(
        ZLinkCanonicalRelocationAuthorityProjection projection)
    {
        Span<byte> bytes = stackalloc byte[16];
        System.Buffers.Binary.BinaryPrimitives.WriteUInt64BigEndian(
            bytes[..8], projection.RelocationHigh);
        System.Buffers.Binary.BinaryPrimitives.WriteUInt64BigEndian(
            bytes[8..], projection.RelocationLow);
        return new Guid(bytes, bigEndian: true);
    }

    private async ValueTask RollbackPreparedTransferredActorAsync(
        ZLinkActorRuntimeState actorState,
        CancellationToken cancellationToken,
        bool startTeardownReconciliation = true)
    {
        Exception? failure = null;
        if (actorState.Actor is { } actor)
        {
            try
            {
                if (actorState.LiveActivation is { } activation)
                    await activation.NotifyActorLeftAfterManagedJoinSpotAsync(actor, cancellationToken)
                        .ConfigureAwait(false);
                else
                    await NotifyEntrySpotActorLeftAsync(
                            actor,
                            actorState.NativeActorRef?.NodeRid,
                            cancellationToken)
                        .ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                failure = exception;
            }
        }

        try
        {
            await _actorSessionManager.RollbackTransferredActorAsync(
                    actorState.ActorId,
                    cancellationToken,
                    startTeardownReconciliation)
                .ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            failure = failure is null ? exception : new AggregateException(failure, exception);
        }

        if (failure is not null)
            throw new InvalidOperationException(
                $"Actor '{actorState.ActorId}' prepared handoff rollback did not finish.",
                failure);
    }

    private async ValueTask<ZLinkRelocationCapacityAbortResult>
        AbortActorHandoffCapacityReservationAsync(
        ZLinkRelocationCapacityFence fence,
        CancellationToken cancellationToken)
    {
        var store = Registration.Locations.ResolveStore();
        if (store is null)
            return ZLinkRelocationCapacityAbortResult.Stale;
        return await store.AbortRelocationCapacityAsync(
                fence,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static ZLinkRemoteActorJoinReply CreateRejectedHandoffReply(string actorId)
        => ZLinkRemoteActorJoinPackets.CreateJoinReply(
            false,
            new ZLinkBackendActorRef(RoutingId.From("rejected"), actorId, 0));

    private async ValueTask<ZLinkCommittedActorAuthority>
        PublishTransferredActorAuthorityAsync(
        ZLinkActorRuntimeState actorState,
        ActorHandoffTarget target,
        string handoffId,
        ulong sourceObjectGeneration,
        ZLinkRelocationManifestReference relocationReference,
        ZLinkRelocationEnvelope relocationRoot,
        ZLinkRelocationCapacityFence? capacityFence,
        ulong targetAuthorityOwnerGeneration,
        CancellationToken cancellationToken)
    {
        var locations = RequireActorRelocationLocationLifecycle(
            LocationLifecycle,
            actorState.ActorId);

        var actorRef = actorState.NativeActorRef
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorState.ActorId}' does not have a native Actor ref during location commit.");
        var targetSpotId = target.UserSpot?.SpotId
                           ?? target.EntrySpot?.SpotId
                           ?? throw new InvalidOperationException(
                               $"Actor '{actorState.ActorId}' handoff target has no Spot identity.");
        if (!locations.SpotLocations.TryGetTrackedGeneration(
                targetSpotId,
                out var targetSpotGeneration)
            || targetSpotGeneration == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor '{actorState.ActorId}' handoff target Spot generation is unavailable.");

        var actorType = actorState.ActorType
                        ?? throw new InvalidOperationException(
                            $"Actor '{actorState.ActorId}' has no registered type during handoff.");
        var meshName = ResolveActorDrainMeshName(Registration, actorType)
                       ?? throw new InvalidOperationException(
                           $"Actor '{actorState.ActorId}' has no mesh during handoff.");
        var committedAuthority =
            await locations.ActorOwnership.CommitTransferredActorAuthorityAsync(
                    actorState.ActorId,
                    actorRef.ToNative(meshName),
                    meshName,
                    targetSpotId,
                    targetSpotGeneration,
                    target.UserSpot is null
                        ? ZLinkSpotKind.Entry
                        : ZLinkSpotKind.User,
                    Guid.ParseExact(handoffId, "N"),
                    actorState.TryGetStagedRelocationSessionRoute(
                        handoffId,
                        out var stagedSessionRoute)
                        ? stagedSessionRoute
                        : default,
                    relocationReference,
                    relocationRoot,
                    capacityFence,
                    targetAuthorityOwnerGeneration,
                    _ => DeactivateActorOnOwnershipLossAsync(actorState.ActorId),
                    cancellationToken)
                .ConfigureAwait(false);
        actorState.Handoff.MarkAuthorityCommitted(
            handoffId,
            sourceObjectGeneration,
            actorRef.Generation);
        GetMeshNodeRuntime(meshName).Node.SetLocalActorAuthority(
            actorRef,
            committedAuthority.AuthorityOwnerGeneration);
        LogActorHandoff(
            $"location_committed actor={actorState.ActorId} spot={target.TargetRid}");
        return committedAuthority;
    }

    internal static ZLinkLocationLifecycle RequireActorRelocationLocationLifecycle(
        ZLinkLocationLifecycle? lifecycle,
        string actorId)
        => lifecycle
           ?? throw new ZLinkFrameworkException(
               ZLinkFrameworkErrorKind.NotFound,
               $"Actor '{actorId}' relocation cannot publish authority because the Location runtime is unavailable.");

    internal async ValueTask<ZLinkRemoteActorAdmissionReply> AdmitRoutedActorJoinAsync(
        string spotId,
        ZLinkRemoteActorAdmissionRequest request,
        CancellationToken cancellationToken = default)
    {
        //  Entry point for the target side of a relocation. Without this a
        //  stall between arrival and the admission decision is invisible.
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"admit_entry actor={request.ActorId} spot={spotId} draining={_drainAdmission.IsDraining}");
        if (!_drainAdmission.TryEnterActorAdmission(out var admissionLease))
            return ZLinkRemoteActorJoinPackets.CreateAdmissionReply(
                false,
                ZLinkMessage.Empty,
                Registration.Codecs,
                request.DeadlineUnixTimeMilliseconds);
        using (admissionLease)
        {
            var target = ResolveActorHandoffTarget(spotId)
                         ?? throw new InvalidOperationException(
                             $"Actor handoff target '{spotId}' is not active.");

            return await _actorHandoffAdmissions.AdmitReservedAsync(
                    request,
                    spotId,
                    async ct =>
                    {
                        var targetSpotGeneration = target.UserSpot?.ObjectGeneration
                                                   ?? target.EntrySpot?.ObjectGeneration
                                                   ?? 0;
                        var targetNodeGeneration = GetSpotNodeRuntime(target.NodeRid)
                            .Node
                            .MeshStatus()
                            .LifecycleGeneration;
                        var authorityStore = Registration.Locations.ResolveStore()
                                             ?? throw new ZLinkConfigurationException(
                                                 "Cross-node Actor relocation requires an Authority Store.");
                    var authorityKey =
                        ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                            request.ActorId);
                    var authorityRead = await authorityStore.ReadAuthorityAsync(
                            authorityKey,
                            ct)
                        .ConfigureAwait(false);
                    if (authorityRead
                            is not ZLinkAuthorityReadResult.Found authority
                        || authority.Snapshot.ObjectGeneration
                           != request.ActorGeneration
                        || authority.Snapshot.AuthorityOwnerGeneration
                           != request.ActorAuthorityOwnerGeneration
                        || authority.Snapshot.Allocation.ObjectKind
                           != ZLinkPlacementObjectKind.Actor
                        || !StringComparer.Ordinal.Equals(
                            authority.Snapshot.Allocation.StableType,
                            request.ActorType))
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.InvalidOperation,
                            $"Actor '{request.ActorId}' authority changed before target reservation.");
                    var targetDescriptor = (await authorityStore
                            .ListAllMeshNodesAsync(
                                authority.Snapshot.Allocation.Descriptor
                                    .MeshName,
                                ct)
                            .ConfigureAwait(false))
                        .SingleOrDefault(candidate =>
                            candidate.Rid == target.NodeRid
                            && candidate.LifecycleGeneration
                            == targetNodeGeneration)
                        ?? throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.NotFound,
                            $"Actor '{request.ActorId}' target descriptor is unavailable.");
                    var targetFenceValid = target.EntrySpot is not null
                        ? request.TargetSpotAuthorityOwnerGeneration
                          == targetNodeGeneration
                        : await HasExactUserSpotAuthorityAsync(
                                target.TargetRid,
                                request.TargetSpotGeneration,
                                request.TargetSpotAuthorityOwnerGeneration,
                                authorityStore,
                                ct)
                            .ConfigureAwait(false);
                    if (request.ActorGeneration == 0
                        || request.ActorAuthorityOwnerGeneration == 0
                        || request.PredictedPayloadBytes <= 0
                        || request.TargetSpotGeneration != targetSpotGeneration
                        || !targetFenceValid
                        || targetNodeGeneration == 0)
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.InvalidOperation,
                            $"Actor '{request.ActorId}' relocation admission target changed.");
                    ZLinkActorRelocationRegistry.TryResolve(
                        Registration,
                        request.ActorType,
                        target.NodeRid,
                        out var relocation);
                    if (relocation is null)
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Rejected,
                            $"Actor type '{request.ActorType}' relocation policy is not registered on the target node.");
                    var capacityRequest =
                        new ZLinkRelocationCapacityReservationRequest(
                            Guid.ParseExact(request.HandoffId, "N"),
                            authorityKey,
                            authority.Snapshot.StoreVersion,
                            ZLinkPlacementObjectKind.Actor,
                            request.ActorType,
                            authority.Snapshot.Allocation.Descriptor,
                            authority.Snapshot.Allocation
                                .DescriptorLifecycleGeneration,
                            new ZLinkLocationOwnerToken(
                                authority.Snapshot.OwnerId,
                                authority.Snapshot.OwnerLeaseGeneration),
                            new ZLinkMeshNodeDescriptorKey(
                                targetDescriptor.MeshName,
                                targetDescriptor.Rid),
                            targetDescriptor.LifecycleGeneration,
                            new ZLinkLocationOwnerToken(
                                targetDescriptor.OwnerId,
                                targetDescriptor.LeaseGeneration),
                            new ZLinkCapacityVector(1, 0, null));
                    var capacityReservation =
                        await authorityStore.ReserveRelocationCapacityAsync(
                                capacityRequest,
                                ct)
                            .ConfigureAwait(false);
                    var (capacityFence,
                        expectedTargetAuthorityOwnerGeneration) =
                        capacityReservation switch
                        {
                            ZLinkRelocationCapacityReserveResult.Reserved
                                value => (
                                value.Fence,
                                value.TargetAuthorityOwnerGeneration),
                            ZLinkRelocationCapacityReserveResult.AlreadyReserved
                                value => (
                                value.Fence,
                                value.TargetAuthorityOwnerGeneration),
                            ZLinkRelocationCapacityReserveResult
                                .PlacementCapacityExhausted =>
                                throw new ZLinkFrameworkException(
                                    ZLinkFrameworkErrorKind.CapacityExceeded,
                                    $"Actor '{request.ActorId}' target has no relocation capacity.",
                                    ZLinkRetryAdvice.RetryAfterBackoff),
                            ZLinkRelocationCapacityReserveResult
                                .TargetUnavailable =>
                                throw new ZLinkFrameworkException(
                                    ZLinkFrameworkErrorKind.Unavailable,
                                    $"Actor '{request.ActorId}' target became unavailable.",
                                    ZLinkRetryAdvice.RetryAfterBackoff),
                            _ => throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.InvalidOperation,
                                $"Actor '{request.ActorId}' authority changed during target reservation.",
                                ZLinkRetryAdvice.RetryAfterBackoff)
                        };
                    if (expectedTargetAuthorityOwnerGeneration
                            <= request.ActorAuthorityOwnerGeneration
                        || expectedTargetAuthorityOwnerGeneration
                           > long.MaxValue)
                    {
                        await _actorHandoffAdmissions
                            .OwnAndAbortCapacityAsync(
                                request,
                                spotId,
                                capacityFence,
                                default,
                                ct)
                            .ConfigureAwait(false);
                        throw new ZLinkRelocationDataLostException(
                            $"Actor '{request.ActorId}' target reservation returned an invalid authority generation.");
                    }
                    if (!_relocationPermits.TryAcquire(
                            ZLinkRelocationPermitRequest.Inbound(
                                request.PredictedPayloadBytes,
                                restore: relocation.PolicyKind == 2),
                            out var reservationLease))
                    {
                        await _actorHandoffAdmissions
                            .OwnAndAbortCapacityAsync(
                                request,
                                spotId,
                                capacityFence,
                                default,
                                ct)
                            .ConfigureAwait(false);
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Unavailable,
                            $"Actor '{request.ActorId}' target relocation admission is busy.");
                    }
                    var reservation = new ZLinkActorRelocationReservation(
                        Guid.NewGuid().ToString("N"),
                        request.PredictedPayloadBytes,
                        target.NodeRid,
                        targetNodeGeneration,
                        targetSpotGeneration,
                        expectedTargetAuthorityOwnerGeneration,
                        request.TargetSpotAuthorityOwnerGeneration);
                    var cleanupOwned = false;
                    try
                    {
                        var payload =
                            ZLinkRemoteActorJoinPackets
                                .DecodeAdmissionRequestPayload(
                                    request,
                                    Registration.Codecs);
                        ZLinkSpotActorJoinResult result;
                        if (target.UserSpot is { } userSpot)
                            result = await userSpot.AdmitRemoteActorJoinAsync(
                                    request.ActorId,
                                    payload,
                                    ct)
                                .ConfigureAwait(false);
                        else if (target.EntrySpot is not null)
                            result = ZLinkSpotActorJoinResult.Accept();
                        else
                            result = ZLinkSpotActorJoinResult.Reject();
                        ZLinkFrameworkDebugLog.SpotDiscovery(
                            $"admission_decision actor={request.ActorId} spot={spotId} "
                            + $"accepted={result.Accepted} "
                            + $"user_spot={target.UserSpot is not null} "
                            + $"entry_spot={target.EntrySpot is not null}");
                        if (!result.Accepted)
                        {
                            cleanupOwned = true;
                            await _actorHandoffAdmissions
                                .OwnAndAbortCapacityAsync(
                                    request,
                                    spotId,
                                    capacityFence,
                                    reservationLease,
                                    ct)
                                .ConfigureAwait(false);
                            return new ZLinkActorHandoffAdmissionDecision(
                                ZLinkRemoteActorJoinPackets.CreateAdmissionReply(
                                    false,
                                    result.Reply,
                                    Registration.Codecs,
                                    request.DeadlineUnixTimeMilliseconds),
                                default);
                        }
                        ZLinkFrameworkDebugLog.SpotDiscovery(
                            $"admit_reply_built actor={request.ActorId} accepted=true");
                        return new ZLinkActorHandoffAdmissionDecision(
                            ZLinkRemoteActorJoinPackets.CreateAdmissionReply(
                                true,
                                result.Reply,
                                Registration.Codecs,
                                request.DeadlineUnixTimeMilliseconds,
                                reservation),
                            reservationLease,
                            capacityFence);
                    }
                    catch
                    {
                        if (!cleanupOwned)
                            await _actorHandoffAdmissions
                                .OwnAndAbortCapacityAsync(
                                    request,
                                    spotId,
                                    capacityFence,
                                    reservationLease,
                                    ct)
                                .ConfigureAwait(false);
                        throw;
                    }
                },
                cancellationToken)
            .ConfigureAwait(false);
        }
    }

    internal ValueTask AbortRoutedActorJoinAdmissionAsync(
        string spotId,
        ZLinkRemoteActorAdmissionAbortRequest request,
        CancellationToken cancellationToken) =>
        _actorHandoffAdmissions.AbortReservationAsync(
            request,
            spotId,
            cancellationToken);

    private ActorHandoffTarget? ResolveActorHandoffTarget(string spotId)
    {
        var state = _state
                    ?? throw new InvalidOperationException(
                        "ZLink framework runtime is not available for actor handoff.");
        if (_spots.GetActivationBySpotId(state, spotId) is { } userSpot)
            return new ActorHandoffTarget(
                spotId,
                userSpot.NodeRid,
                userSpot,
                null);
        var entryNode = state.SpotNodes.Values.FirstOrDefault(
            node => node.EntrySpotActivation is { } entrySpot
                    && string.Equals(entrySpot.SpotId, spotId, StringComparison.Ordinal));
        if (entryNode?.EntrySpotActivation is { } entrySpot)
            return new ActorHandoffTarget(
                spotId,
                entryNode.Node.RoutingId,
                null,
                entrySpot);
        return null;
    }

    private async ValueTask ValidateActorRelocationTargetAsync(
        ZLinkRemoteActorJoinRequest request,
        ActorHandoffTarget target,
        IZLinkLocationRepository authorityStore,
        CancellationToken cancellationToken)
    {
        var nodeGeneration = GetSpotNodeRuntime(target.NodeRid)
            .Node
            .MeshStatus()
            .LifecycleGeneration;
        var spotGeneration = target.UserSpot?.ObjectGeneration
                             ?? target.EntrySpot?.ObjectGeneration
                             ?? 0;
        var targetFenceValid = target.EntrySpot is not null
            ? request.TargetSpotAuthorityOwnerGeneration == nodeGeneration
            : await HasExactUserSpotAuthorityAsync(
                    target.TargetRid,
                    request.TargetSpotGeneration,
                    request.TargetSpotAuthorityOwnerGeneration,
                    authorityStore,
                    cancellationToken)
                .ConfigureAwait(false);
        if (string.IsNullOrEmpty(request.ReservationToken)
            || request.ReservedPayloadBytes <= 0
            || request.TargetNodeRid is null
            || !request.TargetNodeRid.AsSpan().SequenceEqual(
                target.NodeRid.ToBytes())
            || request.TargetNodeGeneration != nodeGeneration
            || request.TargetSpotGeneration != spotGeneration
            || !targetFenceValid
            || request.ActorAuthorityOwnerGeneration
               is 0 or > long.MaxValue
            || request.TargetAuthorityOwnerGeneration
               is 0 or > long.MaxValue
            || request.TargetAuthorityOwnerGeneration
               <= request.ActorAuthorityOwnerGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{request.ActorId}' relocation target fence changed.");
    }

    private static async ValueTask<bool> HasExactUserSpotAuthorityAsync(
        string spotId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        IZLinkLocationRepository authorityStore,
        CancellationToken cancellationToken)
    {
        var read = await authorityStore.ReadAuthorityAsync(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId),
                cancellationToken)
            .ConfigureAwait(false);
        return read is ZLinkAuthorityReadResult.Found found
               && found.Snapshot.ObjectGeneration == objectGeneration
               && found.Snapshot.AuthorityOwnerGeneration
                  == authorityOwnerGeneration;
    }

    private async ValueTask PrepareTransferredActorTargetAsync(
        ActorHandoffTarget target,
        IZLinkActor actor,
        ZLinkActorRuntimeState actorState,
        CancellationToken cancellationToken)
    {
        if (target.UserSpot is { } userSpot)
        {
            await userSpot.PrepareTransferredActorJoinAndReplayAsync(
                    actor,
                    actorState,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }
    }

    private async ValueTask CompleteTransferredActorTargetLifecycleAsync(
        ActorHandoffTarget target,
        ZLinkActorRuntimeState actorState,
        string handoffId,
        CancellationToken cancellationToken)
    {
        if (target.UserSpot is { } userSpot)
        {
            await userSpot.CompleteTransferredActorJoinLifecycleAsync(
                    actorState,
                    handoffId,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        if (target.EntrySpot is not null)
        {
            var state = _state
                        ?? throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.InvalidOperation,
                            "Framework runtime state is not available during Actor Join completion.");
            var actor = actorState.Actor
                        ?? throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.NotFound,
                            $"Actor '{actorState.ActorId}' has no transferred instance at commit.");
            if (!actorState.Handoff.TryBeginJoinedNotification(handoffId)) return;
            try
            {
                // Entry Spot callbacks are invoked from the routed handoff
                // completion path, not from the actor dispatch mailbox. Mark
                // the callback as the actor's lifecycle owner so a callback
                // that requests DestroyActorAsync defers terminal cleanup
                // until the callback returns instead of waiting on itself.
                using var dispatch = actorState.EnterDeferredJoinExecution();
                await _spots.EntrySpotActors.NotifyJoinedForRelocationAsync(
                        state,
                        actor,
                        target.NodeRid,
                        cancellationToken)
                    .ConfigureAwait(false);
                actorState.Handoff.CompleteJoinedNotification(handoffId);
            }
            catch
            {
                actorState.Handoff.RetryJoinedNotification(handoffId);
                throw;
            }
            return;
        }

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NotFound,
            $"Actor '{actorState.ActorId}' has no active handoff target.");
    }

    private async ValueTask DeliverFailedTransferredActorJoinAsync(
        ZLinkActorRuntimeState actorState,
        ZLinkRemoteActorHandoffCompletionRequest request,
        Exception failure,
        CancellationToken cancellationToken)
    {
        if (!actorState.Handoff.FailJoinedNotification(request.HandoffId)) return;
        if (request.OperationIdHigh == 0 && request.OperationIdLow == 0) return;

        var actor = actorState.Actor
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{request.ActorId}' has no transferred instance for failure completion.");
        var actorRef = actorState.NativeActorRef
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{request.ActorId}' has no current reference for failure completion.");
        var kind = MapPostCommitActorJoinFailure(failure);
        try
        {
            await actorState.ExecuteRelocationCompletionAsync(
                    actorRef.Generation,
                    token => actor.OnJoinCompletedAsync(
                        new ZLinkActorJoinCompletion.Failed(
                            new ZLinkActorJoinOperationId(
                                request.OperationIdHigh,
                                request.OperationIdLow),
                            kind),
                        token),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception deliveryFailure)
        {
            ZLinkFrameworkDebugLog.TaskFailure(
                "actor-target-failed-completion",
                deliveryFailure);
        }
    }

    private ZLinkFrameworkErrorKind MapPostCommitActorJoinFailure(
        Exception failure) =>
        failure switch
        {
            ZLinkFrameworkException framework => framework.Kind,
            OperationCanceledException when ShutdownToken.IsCancellationRequested
                => ZLinkFrameworkErrorKind.ShuttingDown,
            TimeoutException => ZLinkFrameworkErrorKind.DeadlineExceeded,
            _ => ZLinkFrameworkErrorKind.InternalFailure
        };

    private async ValueTask ReplayTransferredActorHandoffAsync(
        ActorHandoffTarget target,
        ZLinkActorRuntimeState actorState,
        IReadOnlyList<ZLinkActorHandoffFrame> sourceFrames,
        CancellationToken cancellationToken)
    {
        if (target.UserSpot is { } userSpot)
        {
            await userSpot.ReplayTransferredActorHandoffAsync(
                    actorState,
                    sourceFrames,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        var frames = actorState.Handoff.PrepareImportedReplay(sourceFrames);
        await ReplayEntrySpotActorFramesAsync(actorState, frames, cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask ReplayFinalTransferredActorHandoffAsync(
        ActorHandoffTarget target,
        ZLinkActorRuntimeState actorState,
        string handoffId,
        CancellationToken cancellationToken)
    {
        if (target.UserSpot is { } userSpot)
        {
            await userSpot.ReplayFinalTransferredActorHandoffAsync(
                    actorState,
                    handoffId,
                    cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        while (true)
        {
            var frames = actorState.Handoff.SnapshotFinalReplay();
            if (frames.Count == 0
                && actorState.Handoff.TryCompleteTransferredActorReplay(handoffId))
                return;
            if (frames.Count == 0)
                continue;
            await ReplayEntrySpotActorFramesAsync(actorState, frames, cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private async ValueTask ReplayEntrySpotActorFramesAsync(
        ZLinkActorRuntimeState actorState,
        IReadOnlyList<ZLinkActorHandoffFrame> frames,
        CancellationToken cancellationToken)
    {
        if (frames.Count == 0) return;
        var actorRef = actorState.NativeActorRef
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorState.ActorId}' does not have a native Actor ref during handoff replay.");
        var pipeline = new ZLinkActorInboundPipeline(
            this,
            new ZLinkEntrySpotActorInboundEndpoint(this));
        await pipeline.DispatchReplayAsync(
                ZLinkActorHandoffFrames.Restore(actorRef, frames),
                actorState.Handoff.AcknowledgeReplayedFrame,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private readonly record struct ActorHandoffTarget(
        string TargetRid,
        RoutingId NodeRid,
        ZLinkSpotActivation? UserSpot,
        ZLinkEntrySpotActivation? EntrySpot);

    internal async ValueTask<ZLinkRemoteSessionBindResponse> BindRemoteBoundSessionRouteAsync(
        ZLinkRemoteSessionBindRequest request,
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var targetNodeRid = RoutingId.From(request.TargetNodeRid);
        var sessionNodeRid = RoutingId.From(request.SessionNodeRid);
        var sessionRid = RoutingId.From(request.SessionRid);
        if (sourceNodeRid != sessionNodeRid)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Remote actor session binding source did not match the declared session node.");

        var actorRef = new ZLinkBackendActorRef(
            targetNodeRid,
            request.ActorId,
            request.ObjectGeneration);
        var nodeRuntime = GetSpotNodeRuntime(targetNodeRid);
        var node = nodeRuntime.Node;
        var currentNodeGeneration = node.MeshStatus().LifecycleGeneration;
        var localMeshName = ResolveSpotNodeMeshName(nodeRuntime);
        if (!string.Equals(localMeshName, request.MeshName, StringComparison.Ordinal)
            || currentNodeGeneration == 0
            || !HasExactAdmittedNodeLifecycle(
                node,
                sessionNodeRid,
                request.SessionOwnerNodeGeneration)
            || node.RoutingId != targetNodeRid
            || node is not IZLinkBackendLocalActorAuthorityReader authorityReader
            || !authorityReader.TryGetLocalActorAuthority(
                actorRef,
                out var authorityOwnerGeneration,
                out var ownerLeaseGeneration)
            || authorityOwnerGeneration == 0
            || ownerLeaseGeneration == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{request.ActorId}' remote session binding target lifecycle is stale.");

        //  Recorded at bind time; the relocation seal reads it back later, so
        //  printing both ends shows whether they name the same node.
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"bind_session actor={request.ActorId} session_node={sessionNodeRid}");
        var replacement = _actorBoundSessionCoordinator.BeginActorSessionReplacement(
            request.ActorId,
            sessionNodeRid,
            sessionRid,
            request.BindingToken,
            request.BindingGeneration,
            request.ObjectGeneration,
            authorityOwnerGeneration,
            localMeshName,
            currentNodeGeneration,
            ownerLeaseGeneration,
            request.SessionOwnerNodeGeneration,
            request.AcceptedHighWater,
            ZLinkSessionBindingReplacement.CreateFence(
                request.PreviousBinding));
        if (!replacement.OwnsExecution)
        {
            var joinedFailure = await replacement.Completion
                .WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            if (joinedFailure is not null)
                System.Runtime.ExceptionServices.ExceptionDispatchInfo
                    .Capture(joinedFailure)
                    .Throw();
        }
        else
        {
            try
            {
                if (!replacement.PreviousBindingTombstoned
                    && request.PreviousBinding is { } previousBinding
                    && RoutingId.From(previousBinding.TargetNodeRid)
                    != targetNodeRid)
                {
                    await ZLinkSessionBindingReplacement.CompletePreviousAsync(
                            request.ActorId,
                            targetNodeRid,
                            request.PreviousBinding,
                            SendPreviousTombstoneAsync,
                            // Once an exact replacement attempt owns execution,
                            // remote invalidation and local publication form one
                            // forward-completing operation. Caller cancellation
                            // cannot split those durable transitions.
                            CancellationToken.None)
                        .ConfigureAwait(false);
                    _actorBoundSessionCoordinator
                        .MarkPreviousActorSessionBindingTombstoned(
                            request.ActorId,
                            replacement);
                }
                EnsureSessionReplacementAuthorityCurrent(
                    request,
                    targetNodeRid,
                    currentNodeGeneration,
                    authorityOwnerGeneration,
                    ownerLeaseGeneration);
                _actorBoundSessionCoordinator.PublishActorSessionReplacement(
                    request.ActorId,
                    replacement);
                await TombstoneReplacedSessionOwnerAsync(
                        request.ActorId,
                        targetNodeRid,
                        replacement.Previous,
                        // Publication is the irreversible cutover. Complete
                        // the remaining exact cleanup even if the initiating
                        // request is cancelled after that point.
                        CancellationToken.None)
                    .ConfigureAwait(false);
                _actorBoundSessionCoordinator.CompleteActorSessionReplacement(
                    request.ActorId,
                    replacement);
            }
            catch (Exception failure)
            {
                _actorBoundSessionCoordinator.AbortActorSessionReplacement(
                    request.ActorId,
                    replacement,
                    failure);
                throw;
            }
        }
        return new ZLinkRemoteSessionBindResponse(
            true,
            request.ObjectGeneration,
            localMeshName,
            targetNodeRid.ToBytes().ToArray(),
            currentNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);

        async ValueTask<ZLinkRemoteSessionUnbindResponse>
            SendPreviousTombstoneAsync(
                ZLinkRemoteSessionUnbindRequest unbind,
                CancellationToken token)
        {
            return await Services.GetRequiredService<IZLinkRouteClient>()
                .RequestToNode(
                    unbind.MeshName,
                    RoutingId.From(unbind.TargetNodeRid),
                    unbind)
                .Timeout(Registration.DefaultRequestTimeout)
                .Async<ZLinkRemoteSessionUnbindResponse>(token)
                .ConfigureAwait(false);
        }
    }

    private void EnsureSessionReplacementAuthorityCurrent(
        ZLinkRemoteSessionBindRequest request,
        RoutingId targetNodeRid,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration)
    {
        var nodeRuntime = GetSpotNodeRuntime(targetNodeRid);
        var node = nodeRuntime.Node;
        var actorRef = new ZLinkBackendActorRef(
            targetNodeRid,
            request.ActorId,
            request.ObjectGeneration);
        if (node.MeshStatus().LifecycleGeneration != targetNodeGeneration
            || !HasExactAdmittedNodeLifecycle(
                node,
                RoutingId.From(request.SessionNodeRid),
                request.SessionOwnerNodeGeneration)
            || !string.Equals(
                ResolveSpotNodeMeshName(nodeRuntime),
                request.MeshName,
                StringComparison.Ordinal)
            || !TryGetCreatedActorState(request.ActorId, out var state)
            || state.NativeActorRef is not { } currentActor
            || currentActor != actorRef
            || node is not IZLinkBackendLocalActorAuthorityReader authorityReader
            || !authorityReader.TryGetLocalActorAuthority(
                actorRef,
                out var currentAuthorityOwnerGeneration,
                out var currentOwnerLeaseGeneration)
            || currentAuthorityOwnerGeneration != authorityOwnerGeneration
            || currentOwnerLeaseGeneration != ownerLeaseGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{request.ActorId}' changed authority before session binding publication.",
                ZLinkRetryAdvice.RetryAfterBackoff);
    }

    private static bool HasExactAdmittedNodeLifecycle(
        IZLinkBackendSpotNode node,
        RoutingId nodeRid,
        ulong nodeGeneration) =>
        MatchesAdmittedNodeLifecycle(
            node.MeshStatus(),
            node.MeshPeers(),
            nodeRid,
            nodeGeneration);

    internal static bool MatchesAdmittedNodeLifecycle(
        MeshNodeStatus local,
        IReadOnlyList<MeshNodePeer> peers,
        RoutingId nodeRid,
        ulong nodeGeneration)
    {
        if (nodeGeneration == 0) return false;
        if (local.RoutingId == nodeRid)
            return local.LifecycleGeneration == nodeGeneration;
        return peers.Any(peer =>
            peer.State == MeshPeerState.Admitted
            && peer.RoutingId == nodeRid
            && peer.LifecycleGeneration == nodeGeneration);
    }

    private async ValueTask TombstoneReplacedSessionOwnerAsync(
        string actorId,
        RoutingId actorNodeRid,
        ZLinkActorBoundSession? replaced,
        CancellationToken cancellationToken)
    {
        if (replaced is not { SessionNodeRid: { } sessionNodeRid } previous
            || sessionNodeRid.IsEmpty)
            return;

        // A disconnected Session owner has already removed its local binding
        // during the stream terminal path. There is no remote node to ACK the
        // owner tombstone after that lifecycle is no longer admitted, so the
        // replacement must not wait for an unreachable cleanup request. An
        // admitted exact lifecycle still requires the normal tombstone ACK.
        var meshNode = GetMeshNodeRuntime(previous.MeshName).Node;
        if (!HasExactAdmittedNodeLifecycle(
                meshNode,
                sessionNodeRid,
                previous.SessionOwnerNodeGeneration))
            return;

        var request = new ZLinkRemoteSessionOwnerTombstoneRequest(
            actorId,
            actorNodeRid.ToBytes().ToArray(),
            previous.ObjectGeneration,
            previous.MeshName,
            previous.TargetNodeGeneration,
            previous.AuthorityOwnerGeneration,
            previous.OwnerLeaseGeneration,
            previous.SessionRid.ToBytes().ToArray(),
            previous.BindingToken,
            previous.BindingGeneration,
            previous.SessionOwnerNodeGeneration);
        ZLinkRemoteSessionOwnerTombstoneResponse response;
        var localNodeRid = GetMeshNodeRuntime(previous.MeshName).Node.RoutingId;
        try
        {
            if (sessionNodeRid == localNodeRid)
            {
                response = await TombstoneRemoteSessionOwnerBindingAsync(
                        request,
                        actorNodeRid,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            else
            {
                response = await Services.GetRequiredService<IZLinkRouteClient>()
                    .RequestToNode(previous.MeshName, sessionNodeRid, request)
                    .Timeout(Registration.DefaultRequestTimeout)
                    .Async<ZLinkRemoteSessionOwnerTombstoneResponse>(cancellationToken)
                    .ConfigureAwait(false);
            }
        }
        catch (ZLinkFrameworkException exception)
            when (exception.Kind == ZLinkFrameworkErrorKind.NotFound)
        {
            // A RouteMesh can lose the old Session owner between the
            // replacement read and this cleanup request. The disconnected
            // Session has already discarded its local binding, so an absent
            // owner is an idempotent terminal cleanup result.
            return;
        }
        if (!response.Acknowledged)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actorId}' previous session owner did not acknowledge its binding tombstone.",
                ZLinkRetryAdvice.RetryAfterBackoff);
    }

    internal async ValueTask<ZLinkRemoteSessionOwnerTombstoneResponse>
        TombstoneRemoteSessionOwnerBindingAsync(
            ZLinkRemoteSessionOwnerTombstoneRequest request,
            RoutingId sourceNodeRid,
            CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var actorNodeRid = RoutingId.From(request.ActorNodeRid);
        var sessionNode = GetMeshNodeRuntime(request.MeshName).Node;
        var rows = Services.GetService(typeof(ZLinkStoreLocationResolvers))
                   as ZLinkStoreLocationResolvers;
        if (rows is null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Session owner tombstone requires current Location Store authority.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        var key = new ZLinkActorLocationKey(request.ActorId);
        rows.InvalidateActorRoute(key);
        var currentLocation = await rows.ResolveActorRowAsync(
                key,
                cancellationToken)
            .ConfigureAwait(false);
        if (actorNodeRid != sourceNodeRid
            || currentLocation is null
            || currentLocation.ActorRef.NodeRid != actorNodeRid
            || currentLocation.ActorRef.ObjectGeneration
            != request.ObjectGeneration
            || currentLocation.OwnerNodeGeneration
            != request.ActorNodeGeneration
            || currentLocation.AuthorityOwnerGeneration
            != request.AuthorityOwnerGeneration
            || currentLocation.LeaseGeneration < 0
            || (ulong)currentLocation.LeaseGeneration
            != request.OwnerLeaseGeneration
            || !HasExactAdmittedNodeLifecycle(
                sessionNode,
                actorNodeRid,
                request.ActorNodeGeneration)
            || sessionNode.MeshStatus().LifecycleGeneration
            != request.SessionOwnerNodeGeneration
            || request.ObjectGeneration == 0
            || string.IsNullOrWhiteSpace(request.MeshName)
            || request.ActorNodeGeneration == 0
            || request.AuthorityOwnerGeneration == 0
            || request.OwnerLeaseGeneration == 0
            || request.BindingGeneration == 0
            || request.SessionOwnerNodeGeneration == 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Session binding tombstone source or identity is invalid.");
        var actorRoute = ZLinkSessionBindingRoute.Create(
            new ActorRef(
                request.ActorId,
                request.ObjectGeneration,
                request.MeshName,
                actorNodeRid),
            request.MeshName,
            request.ActorNodeGeneration,
            request.AuthorityOwnerGeneration,
            request.OwnerLeaseGeneration);
        TombstoneSessionActorBinding(
            request.ActorId,
            RoutingId.From(request.SessionRid),
            request.BindingToken,
            request.BindingGeneration,
            request.SessionOwnerNodeGeneration,
            actorRoute);
        return new ZLinkRemoteSessionOwnerTombstoneResponse(true);
    }

    internal async ValueTask<ZLinkRemoteSessionUnbindResponse>
        TombstoneRemoteActorBindingAsync(
            ZLinkRemoteSessionUnbindRequest request,
            RoutingId sourceNodeRid,
            CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var targetNodeRid = RoutingId.From(request.TargetNodeRid);
        var nodeRuntime = GetSpotNodeRuntime(targetNodeRid);
        var node = nodeRuntime.Node;
        var rows = Services.GetService(typeof(ZLinkStoreLocationResolvers))
                   as ZLinkStoreLocationResolvers;
        if (rows is null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Actor session replacement requires current Location Store authority.",
                ZLinkRetryAdvice.RetryAfterBackoff);

        var key = new ZLinkActorLocationKey(request.ActorId);
        rows.InvalidateActorRoute(key);
        var currentLocation = await rows.ResolveActorRowAsync(
                key,
                cancellationToken)
            .ConfigureAwait(false);
        var actorRef = new ZLinkBackendActorRef(
            targetNodeRid,
            request.ActorId,
            request.ObjectGeneration);
        var sessionNodeRid = RoutingId.From(request.SessionNodeRid);
        var sessionRid = RoutingId.From(request.SessionRid);
        var expected = new ZLinkActorBoundSession(
            sessionNodeRid,
            sessionRid,
            request.BindingToken,
            request.BindingGeneration,
            request.ObjectGeneration,
            request.AuthorityOwnerGeneration,
            request.MeshName,
            request.TargetNodeGeneration,
            request.OwnerLeaseGeneration,
            request.SessionOwnerNodeGeneration,
            request.AcceptedHighWater);
        if (sourceNodeRid.IsEmpty
            || sessionNodeRid.IsEmpty
            || sessionRid.IsEmpty
            || currentLocation is null
            || currentLocation.ActorRef.NodeRid != sourceNodeRid
            || !string.Equals(
                currentLocation.MeshName,
                request.MeshName,
                StringComparison.Ordinal)
            || !HasExactAdmittedNodeLifecycle(
                node,
                sourceNodeRid,
                currentLocation.OwnerNodeGeneration)
            || node.MeshStatus().LifecycleGeneration
            != request.TargetNodeGeneration
            || !string.Equals(
                ResolveSpotNodeMeshName(nodeRuntime),
                request.MeshName,
                StringComparison.Ordinal)
            || !TryGetCreatedActorState(request.ActorId, out var actorState)
            || actorState.NativeActorRef is not { } currentActor
            || currentActor != actorRef
            || node is not IZLinkBackendLocalActorAuthorityReader authorityReader
            || !authorityReader.TryGetLocalActorAuthority(
                actorRef,
                out var currentAuthorityOwnerGeneration,
                out var currentOwnerLeaseGeneration)
            || currentAuthorityOwnerGeneration
            != request.AuthorityOwnerGeneration
            || currentOwnerLeaseGeneration != request.OwnerLeaseGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{request.ActorId}' previous binding tombstone is stale.",
                ZLinkRetryAdvice.RetryAfterBackoff);

        _actorBoundSessionCoordinator.TombstoneActorSession(
            request.ActorId,
            expected);
        return new ZLinkRemoteSessionUnbindResponse(true);
    }

    private static string ResolveSpotNodeMeshName(ZLinkSpotNodeRuntime node) =>
        node.Registration.SpotMeshChannelName ?? node.Registration.SpotNodeName;

    internal ValueTask<ZLinkSpotActivation?> JoinActorToSpotAsync(
        ZLinkSpotActivation activation,
        IZLinkActor actor,
        CancellationToken cancellationToken = default)
    {
        return _actorSessionManager.JoinActorToSpotAsync(activation, actor, cancellationToken);
    }

    internal ValueTask<ZLinkSpotActivation?> CommitActorToSpotAsync(
        ZLinkSpotActivation activation,
        IZLinkActor actor,
        Func<CancellationToken, ValueTask> commitAuthority,
        Action publishTargetMembership,
        CancellationToken cancellationToken = default)
    {
        return _actorSessionManager.CommitActorToSpotAsync(
            activation,
            actor,
            commitAuthority,
            publishTargetMembership,
            cancellationToken);
    }

    internal ValueTask RestoreActorSpotAfterFailedCommitAsync(
        ZLinkSpotActivation failedTarget,
        ZLinkSpotActivation? previousActivation,
        IZLinkActor actor,
        CancellationToken cancellationToken = default)
    {
        return _actorSessionManager.RestoreActorSpotAfterFailedCommitAsync(
            failedTarget,
            previousActivation,
            actor,
            cancellationToken);
    }

    internal ValueTask AttachActorAsync(
        IZLinkActor actor,
        IZLinkStream stream,
        CancellationToken cancellationToken = default)
    {
        return _actorSessionManager.AttachActorAsync(actor, stream, cancellationToken);
    }

    internal ValueTask DisconnectActorAsync(
        IZLinkActor actor,
        IZLinkStream stream,
        CancellationToken cancellationToken = default)
    {
        return _actorSessionManager.DisconnectActorAsync(actor, stream, cancellationToken);
    }

    internal ValueTask SubmitActorAsync(
        IZLinkActor actor,
        ZlinkStreamHeader header,
        Message payload,
        CancellationToken cancellationToken = default) =>
        SubmitActorAsync(
            actor,
            header,
            payload,
            relocationReplay: false,
            cancellationToken);

    internal ValueTask SubmitActorAsync(
        IZLinkActor actor,
        ZlinkStreamHeader header,
        Message payload,
        bool relocationReplay,
        CancellationToken cancellationToken = default)
    {
        return _actorSessionManager.SubmitActorAsync(
            actor,
            header,
            payload,
            relocationReplay,
            cancellationToken);
    }

    internal ValueTask<CreateActorResult> CreateLocalActorAsync(
        string actorId,
        string actorType,
        CancellationToken cancellationToken = default)
    {
        return CreateLocalActorAsync(
                actorId,
                actorType,
                ZLinkMessage.Empty,
                cancellationToken);
    }

    internal ValueTask<CreateActorResult> CreateLocalActorAsync(
        string actorId,
        string actorType,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken = default)
    {
        return CreateLocalActorAsync(
            actorId,
            actorType,
            createRequest,
            ZLinkActorClaimMode.NewOwner,
            cancellationToken);
    }

    private async ValueTask<CreateActorResult> CreateLocalActorAsync(
        string actorId,
        string actorType,
        ZLinkMessage createRequest,
        ZLinkActorClaimMode claimMode,
        CancellationToken cancellationToken)
    {
        var result = await _actorSessionManager.CreateAndBindActorAsync(
                actorId,
                actorType,
                createRequest,
                claimMode,
                cancellationToken)
            .ConfigureAwait(false);
        if (result.Created)
        {
            var state = GetOrCreateActorState(result.Actor.Context.ActorId);
            var nativeRef = state.NativeActorRef
                            ?? throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.NotFound,
                                $"Actor '{result.Actor.Context.ActorId}' does not have a native Actor ref after creation.");
            var response = await NotifyEntrySpotActorCreatedAsync(
                    result.Actor,
                    result.CreateRequest,
                    nativeRef.NodeRid,
                    cancellationToken)
                .ConfigureAwait(false);
            if (!response.Accepted)
            {
                await DestroyActorAsync(
                        nativeRef.NodeRid,
                        result.Actor,
                        CancellationToken.None)
                    .ConfigureAwait(false);
            }

            return result with { Response = response };
        }

        return result;
    }

    internal ValueTask<CreateActorResult> CreateActorAsync(
        string actorId,
        string actorType,
        CancellationToken cancellationToken = default)
    {
        return CreateActorAsync(actorId, actorType, ZLinkMessage.Empty, cancellationToken);
    }

    internal ValueTask<CreateActorResult> CreateActorAsync(
        string actorId,
        string actorType,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken = default)
    {
        _drainAdmission.RequireActorAdmission();
        return _actorSessionManager.CreateActorAsync(actorId, actorType, createRequest, cancellationToken);
    }

    internal async ValueTask<CreateActorResult> PrepareReservedActorAsync(
        string actorId,
        string actorType,
        ZLinkMessage createRequest,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        CancellationToken cancellationToken)
    {
        var result = await _actorSessionManager.PrepareReservedActorAsync(
                actorId,
                actorType,
                createRequest,
                objectGeneration,
                authorityOwnerGeneration,
                cancellationToken)
            .ConfigureAwait(false);
        if (!result.Created)
            return result;

        var state = GetOrCreateActorState(actorId);
        var nativeRef = state.NativeActorRef
                        ?? throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.NotFound,
                            $"Actor '{actorId}' does not have a native Actor ref after reserved creation.");
        var response = await NotifyEntrySpotActorCreatedAsync(
                result.Actor,
                createRequest,
                nativeRef.NodeRid,
                cancellationToken)
            .ConfigureAwait(false);
        return result with { Response = response };
    }

    internal void PublishReservedActor(string actorId) =>
        _actorSessionManager.PublishReservedActor(actorId);

    internal async ValueTask PublishCreatedReservedActorAsync(
        string actorId,
        string actorType,
        ActorRef committedActor,
        CancellationToken cancellationToken,
        ZLinkAuthoritySnapshot? committedAuthority = null)
    {
        var state = GetOrCreateActorState(actorId);
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_state_commit_check actor={actorId} "
            + $"committed_generation={committedActor.ObjectGeneration} "
            + $"committed_node={committedActor.NodeRid} "
            + $"actor_present={state.Actor is not null} "
            + $"native_generation={state.NativeActorRef?.Generation.ToString() ?? "<none>"} "
            + $"native_node={state.NativeActorRef?.NodeRid.ToString() ?? "<none>"} "
            + $"retired_generation={state.RetiredLocalActorRef?.Generation.ToString() ?? "<none>"} "
            + $"blocked={state.IsDispatchBlocked}");
        if (state.Actor is null
            || state.NativeActorRef is not { } nativeActor
            || nativeActor.Generation != committedActor.ObjectGeneration
            || nativeActor.NodeRid != committedActor.NodeRid)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor '{actorId}' reserved creation is not materialized on its committed owner.");

        if (LocationLifecycle is { } locations)
        {
            if (committedAuthority is { } snapshot)
                locations.ActorOwnership.AdoptCommittedActorAuthority(
                    actorId,
                    actorType,
                    committedActor,
                    snapshot,
                    _ => DeactivateActorOnOwnershipLossAsync(actorId));
            else
                await locations.ActorOwnership.AdoptCommittedActorAuthorityAsync(
                        actorId,
                        actorType,
                        committedActor,
                        _ => DeactivateActorOnOwnershipLossAsync(actorId),
                        cancellationToken)
                    .ConfigureAwait(false);
        }

        _actorSessionManager.PublishReservedActor(actorId);
    }

    internal ValueTask DiscardReservedActorAsync(
        string actorId,
        CancellationToken cancellationToken = default) =>
        _actorSessionManager.RollbackTransferredActorAsync(
            actorId,
            cancellationToken,
            startTeardownReconciliation: true);

    internal ValueTask<IZLinkActor?> FindActorAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        return _actorSessionManager.FindActorAsync(actorId, cancellationToken);
    }

    internal bool TryGetCreatedActor(
        string actorId,
        string actorType,
        out IZLinkActor actor)
    {
        return _actorSessionManager.TryGetCreatedActor(actorId, actorType, out actor);
    }

    internal bool TryGetCreatedActorState(
        string actorId,
        out ZLinkActorRuntimeState state)
    {
        return _actorSessionManager.TryGetCreatedActorState(actorId, out state);
    }

    internal bool TryGetCreatedActorState(
        string actorId,
        string actorType,
        out ZLinkActorRuntimeState state)
    {
        return _actorSessionManager.TryGetCreatedActorState(actorId, actorType, out state);
    }

    internal ValueTask<ZLinkActorReply> SubmitActorForReplyAsync(
        string actorId,
        ZlinkStreamHeader header,
        Message payload,
        CancellationToken cancellationToken = default) =>
        SubmitActorForReplyAsync(
            actorId,
            header,
            payload,
            relocationReplay: false,
            cancellationToken);

    internal ValueTask<ZLinkActorReply> SubmitActorForReplyAsync(
        string actorId,
        ZlinkStreamHeader header,
        Message payload,
        bool relocationReplay,
        CancellationToken cancellationToken = default)
    {
        return _actorSessionManager.SubmitActorForReplyAsync(
            actorId,
            header,
            payload,
            relocationReplay,
            cancellationToken);
    }

    internal ValueTask SubmitActorByIdAsync(
        string actorId,
        ZlinkStreamHeader header,
        Message payload,
        CancellationToken cancellationToken = default)
    {
        return _actorSessionManager.SubmitActorByIdAsync(actorId, header, payload, cancellationToken);
    }

    internal ValueTask NotifyActorDisconnectedByIdAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        return _actorSessionManager.NotifyDisconnectedByIdAsync(actorId, cancellationToken);
    }

    internal async ValueTask NotifyActorDisconnectedAsync(
        ZLinkSessionBindingEntry binding,
        CancellationToken cancellationToken = default)
    {
        var actor = binding.Route.Ref;
        var state = GetOrCreateActorState(actor.ActorId);
        if (state.Actor is not null
            && state.NativeActorRef is { } localActor
            && localActor.NodeRid == actor.NodeRid
            && localActor.Generation == actor.ObjectGeneration)
        {
            if (!state.TryGetBoundSession(out var current)
                || !string.Equals(
                    current.BindingToken,
                    binding.BindingToken,
                    StringComparison.Ordinal))
                return;
            try
            {
                await NotifyActorDisconnectedByIdAsync(actor.ActorId, cancellationToken)
                    .ConfigureAwait(false);
            }
            finally
            {
                RemoveActorSessionBinding(actor.ActorId, binding.BindingToken);
            }
            return;
        }

        var nodeRuntime = GetMeshNodeRuntime(binding.MeshName);

        await _actorBoundSessionCoordinator.NotifyRemoteDisconnectedAsync(
                binding,
                nodeRuntime.Node,
                nodeRuntime.LocalRequestSource,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal ZLinkActorRuntimeState GetOrCreateActorState(string actorId)
    {
        return _actorSessionManager.GetOrCreateState(actorId);
    }

    internal bool TryGetActorState(
        string actorId,
        out ZLinkActorRuntimeState state)
    {
        return _actorSessionManager.TryGetState(actorId, out state!);
    }

    internal ValueTask DeactivateActorOnOwnershipLossAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        return _actorSessionManager.DeactivateActorOnOwnershipLossAsync(actorId, cancellationToken);
    }

    internal ZLinkLocationLifecycle? LocationLifecycle => _locationLifecycle;

    internal ZLinkSessionBindingEntry[] BindSessionActor(
        string actorId,
        ZLinkSessionContext context,
        string bindingToken,
        ZLinkSessionActor actorRef,
        ulong bindingGeneration,
        ZLinkSessionBindingRoute route,
        ulong sessionOwnerNodeGeneration)
    {
        return _actorBoundSessionCoordinator.BindSessionActor(
            actorId,
            context,
            bindingToken,
            actorRef,
            bindingGeneration,
            route,
            sessionOwnerNodeGeneration);
    }

    internal ulong NextSessionBindingGeneration()
        => _actorBoundSessionCoordinator.NextBindingGeneration();

    internal bool TryAcceptSessionActorFrame(
        string actorId,
        string bindingToken,
        out ulong acceptedHighWater)
        => _actorBoundSessionCoordinator.TryAcceptSessionFrame(
            actorId,
            bindingToken,
            out acceptedHighWater);

    internal ValueTask<bool> WaitForSessionActorRouteAvailableAsync(
        string actorId,
        string bindingToken,
        CancellationToken cancellationToken)
        => _actorBoundSessionCoordinator.WaitForSessionRouteAvailableAsync(
            actorId,
            bindingToken,
            cancellationToken);

    internal ZLinkSessionRouteCommitResult CommitSessionActorRoute(
        ZLinkSessionRouteCommit request)
        => _actorBoundSessionCoordinator.CommitSessionRoute(request);

    internal ValueTask<ZLinkSessionRouteSealResult> SealSessionActorRouteAsync(
        ZLinkSessionRouteSeal request,
        CancellationToken cancellationToken)
        => _actorBoundSessionCoordinator.SealSessionRouteAsync(
            request,
            cancellationToken);

    internal void CompleteAcceptedSessionActorFrame(
        string actorId,
        string bindingToken)
        => _actorBoundSessionCoordinator.CompleteAcceptedSessionFrame(
            actorId,
            bindingToken);

    internal string TrackRemoteSessionActorRequest(
        string actorId,
        ulong requestId,
        string bindingToken)
        => _actorBoundSessionCoordinator.TrackRemoteSessionRequest(
            actorId,
            requestId,
            bindingToken);

    internal void CompleteRemoteSessionActorRequest(
        string actorId,
        ulong objectGeneration,
        string bindingToken,
        ulong requestId)
        => _actorBoundSessionCoordinator.CompleteRemoteSessionRequest(
            actorId,
            objectGeneration,
            bindingToken,
            requestId);

    internal bool AbortSessionActorRouteSeal(
        ZLinkSessionRouteSeal request)
        => _actorBoundSessionCoordinator.AbortSessionRouteSeal(request);

    internal bool UnsealCommittedSessionActorRoute(
        ZLinkSessionRouteCommit request)
        => _actorBoundSessionCoordinator.UnsealCommittedSessionRoute(request);

    internal void UnbindSessionActor(
        string actorId,
        ZLinkSessionContext context,
        string bindingToken)
    {
        _actorBoundSessionCoordinator.UnbindSessionActor(actorId, context, bindingToken);
    }

    /// <summary>Actor-node entry for a relayed session frame whose bound actor
    /// migrated here: dispatches it through the standard actor inbound
    /// pipeline. The frame carries the session identity, so the dispatch
    /// binds the remote session route and replies travel back over the
    /// bound-session push relay.</summary>
    internal async ValueTask DispatchRemoteActorFrameAsync(
        string actorId,
        ulong actorGeneration,
        RoutingId targetNodeRid,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        RoutingId authenticatedRelayNodeRid,
        RoutingId relayNodeRid,
        ulong relayNodeGeneration,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        RoutingId sourceSessionRid,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource,
        byte[] applicationMetadata,
        MeshOperationId operationId,
        byte messageFollowHopCount,
        ulong replyRequestId,
        uint replyFlags,
        string? replyCapability,
        ulong deadlineUnixMs,
        byte[] header,
        byte[] body,
        CancellationToken cancellationToken)
    {
        // Core represents an unbounded operation as ulong.MaxValue. The
        // Framework relay contract represents the same state as zero.
        deadlineUnixMs =
            ZLinkMeshRecordAdapters.NormalizeDeadline(deadlineUnixMs);
        // The relay target is this node. Preserve the generation carried by
        // the incoming stale route instead of reading NativeActorRef: during
        // a chained transfer that state already points at the next owner, and
        // replacing the incoming identity would bypass this node's Message
        // Use the Message Follow route before attempting blocked local dispatch.
        var targetNode = GetSpotNodeRuntime(targetNodeRid).Node;
        //  These throws run inside a one-way route send handler, so the
        //  exception never reaches the original requester: the frame just
        //  disappears and the caller waits out its deadline.
        Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"remote_frame_dispatch actor={actorId} "
            + $"node_gen={targetNode.MeshStatus().LifecycleGeneration}/{targetNodeGeneration} "
            + $"authority_gen={authorityOwnerGeneration} lease_gen={ownerLeaseGeneration}");
        if (targetNode.MeshStatus().LifecycleGeneration != targetNodeGeneration
            || authorityOwnerGeneration == 0
            || ownerLeaseGeneration == 0)
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actorId}' session relay target lifecycle is stale.");
        }
        if (authenticatedRelayNodeRid.IsEmpty
            || relayNodeRid.IsEmpty
            || authenticatedRelayNodeRid != relayNodeRid
            || relayNodeGeneration == 0
            || !targetNode.MeshPeers().Any(peer =>
                peer.State == MeshPeerState.Admitted
                && peer.RoutingId == authenticatedRelayNodeRid
                && peer.LifecycleGeneration == relayNodeGeneration))
        {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actorId}' relay peer identity is stale.");
        }
        var state = GetOrCreateActorState(actorId);
        var actorRef = new ZLinkBackendActorRef(
            targetNodeRid,
            actorId,
            actorGeneration);
        var hasBoundSessionFence =
            ZLinkActorBoundSessionHandoffMetadata.TryDecode(
                applicationMetadata,
                out var boundSessionFence);
        var routeContext = new ZLinkBackendActorRouteContext(
            operationId,
            messageFollowHopCount,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            replyRequestId,
            replyFlags,
            replyCapability,
            deadlineUnixMs,
            IsBoundSessionRoute: hasBoundSessionFence);
        ValidateRemoteActorFrameSource(
            actorId,
            routeContext,
            sourceNodeRid,
            sourceNodeGeneration,
            requestSource);
        if (routeContext.IsDirectRoute)
        {
            if (messageFollowHopCount is 0 or > 8
                || targetNode is not IZLinkBackendLocalActorAuthorityReader authorityReader
                || !authorityReader.TryGetLocalActorAuthority(
                    actorRef,
                    out var currentAuthorityOwnerGeneration,
                    out var currentOwnerLeaseGeneration)
                || currentAuthorityOwnerGeneration != authorityOwnerGeneration
                || currentOwnerLeaseGeneration != ownerLeaseGeneration)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Actor '{actorId}' direct relay authority identity is stale.");
        }
        else if (!hasBoundSessionFence
                 || boundSessionFence.ActorId != actorId
                 || boundSessionFence.ActorGeneration != actorGeneration
                 || boundSessionFence.SessionRid != sourceSessionRid
                 || messageFollowHopCount != 0
                 || ((replyRequestId != 0 || replyFlags != 0)
                     && !ZLinkActorBoundSessionRelay.IsNoBindRequest(
                         replyRequestId,
                         replyFlags))
                 || !state.TryGetBoundSessionForInbound(out var session)
                 || !string.Equals(
                     session.BindingToken,
                     boundSessionFence.BindingToken,
                     StringComparison.Ordinal)
                 || session.BindingGeneration
                    != boundSessionFence.BindingGeneration
                 || !ZLinkActorBoundSessionRelay.MatchesRelaySource(
                     session,
                     sourceNodeRid,
                     sourceSessionRid)
                 || session.ObjectGeneration != actorGeneration
                 || !string.Equals(
                     session.MeshName,
                     ResolveSpotNodeMeshName(GetSpotNodeRuntime(targetNodeRid)),
                     StringComparison.Ordinal)
                 || session.TargetNodeGeneration != targetNodeGeneration
                 || session.AuthorityOwnerGeneration != authorityOwnerGeneration
                 || session.OwnerLeaseGeneration != ownerLeaseGeneration)
        {
            //  Nine fields decide this and the frame is gone either way, so
            //  name what the relay claimed against what the binding holds.
            //  A replaced incarnation never comes back, so this is NotFound
            //  (DoNotRetry) rather than Unavailable (RetryAfterBackoff).
            var staleIdentity = new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor '{actorId}' session relay authority identity is stale.");
            //  This runs inside a one-way route send handler, so throwing here
            //  only reaches the dispatch error sink and the requester waits out
            //  its deadline. Spec 20 requires the relay to end with a typed
            //  stale error, so answer the request before giving up on it.
            await ZLinkActorBoundSessionRelay.ReplyStaleActorAsync(
                    this,
                    actorRef,
                    sourceNodeRid,
                    sourceSessionRid,
                    replyRequestId,
                    replyFlags,
                    replyCapability,
                    ZLinkStreamProtocolDefaults.DecodeHeader(header),
                    staleIdentity,
                    cancellationToken)
                .ConfigureAwait(false);
            // The relay has either sent the typed stale response or dropped a
            // one-way frame. Do not surface the expected stale identity as a
            // second dispatch failure; the old owner must not be retried.
            return;
        }
        var parts = new[]
        {
            new ZLinkBackendActorPart(
                actorRef, sourceNodeRid, sourceSessionRid, replyRequestId, replyFlags,
                Message.From(header), More: true, RouteContext: routeContext,
                SourceNodeGeneration: sourceNodeGeneration,
                RequestSource: requestSource,
                ApplicationMetadata: applicationMetadata),
            new ZLinkBackendActorPart(
                actorRef, sourceNodeRid, sourceSessionRid, replyRequestId, replyFlags,
                Message.From(body), More: false, RouteContext: routeContext,
                SourceNodeGeneration: sourceNodeGeneration,
                RequestSource: requestSource,
                ApplicationMetadata: applicationMetadata)
        };
        if (!routeContext.IsDirectRoute)
        {
            // The Session owner assigned this sequence before the frame
            // entered the relay. Apply it before handoff capture so a frame
            // held for replay also advances the target route watermark.
            state.RecordRelocatedSessionAccepted(
                sourceSessionRid,
                boundSessionFence.SessionSequence);
            Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                $"session_route_watermark actor={actorId} "
                + $"session={sourceSessionRid} "
                + $"accepted={boundSessionFence.SessionSequence}");
        }
        var batch = ZLinkActorHandoffIngress.CaptureMovingFrames(this, parts);
        if (batch.Count == 0)
        {
            //  A frame for an Actor mid-handoff is captured for replay instead
            //  of dispatched. Returning quietly makes that indistinguishable
            //  from delivery, and the frame is lost outright if the handoff
            //  never completes.
            return;
        }
        // Per-actor FIFO across concurrently handled relay records: sibling
        // forwarded frames must not overtake each other (spec 23 §10.2).
        Task chained;
        lock (_remoteFrameChainGate)
        {
            var prior = _remoteFrameChains.TryGetValue(actorId, out var chain)
                ? chain
                : Task.CompletedTask;
            chained = DispatchRemoteFrameAfterAsync(
                prior,
                batch,
                deadlineUnixMs,
                cancellationToken);
            _remoteFrameChains[actorId] = chained;
        }

        try
        {
            await chained.ConfigureAwait(false);
        }
        finally
        {
            lock (_remoteFrameChainGate)
            {
                if (_remoteFrameChains.TryGetValue(actorId, out var current)
                    && ReferenceEquals(current, chained))
                    _remoteFrameChains.Remove(actorId);
            }
        }
    }

    private async Task DispatchRemoteFrameAfterAsync(
        Task prior,
        ZLinkSpotActorFrameBatch batch,
        ulong deadlineUnixMs,
        CancellationToken cancellationToken)
    {
        try
        {
            await prior.ConfigureAwait(false);
        }
        catch
        {
            // The prior frame reported its own failure; the chain continues.
        }

        if (deadlineUnixMs == 0)
        {
            await new ZLinkActorInboundPipeline(
                    this,
                    new ZLinkEntrySpotActorInboundEndpoint(this))
                .DispatchAsync(batch, cancellationToken)
                .ConfigureAwait(false);
            return;
        }

        var remaining = checked((long)deadlineUnixMs)
                        - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (remaining <= 0)
        {
            batch.Dispose();
            return;
        }
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken);
        deadline.CancelAfter(TimeSpan.FromMilliseconds(remaining));
        await new ZLinkActorInboundPipeline(
                this,
                new ZLinkEntrySpotActorInboundEndpoint(this))
            .DispatchAsync(batch, deadline.Token)
            .ConfigureAwait(false);
    }

    private readonly object _remoteFrameChainGate = new();
    private readonly Dictionary<string, Task> _remoteFrameChains = new(StringComparer.Ordinal);

    /// <summary>Session-node relay for a frame whose bound actor lives on
    /// another node: wraps the stream frame in the internal node-addressed
    /// actor-frame packet.</summary>
    private bool RelayRemoteActorFrame(
        string? meshName,
        ZLinkBackendActorRef actor,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ZLinkBackendActorRouteContext routeContext,
        ulong sourceNodeGeneration,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource,
        ReadOnlyMemory<byte> applicationMetadata,
        byte[] header,
        byte[] body)
    {
        var nodeRuntime = meshName is null
            ? GetActorClientSpotNodeRuntime()
            : GetMeshNodeRuntime(meshName);
        meshName = nodeRuntime.Registration.SpotMeshChannelName
                   ?? nodeRuntime.Registration.SpotNodeName;
        // A locally relayed frame carries no source node rid (the session is
        // on this node); the receiver needs the concrete session node for the
        // reply route, so substitute the local node rid.
        var sessionNodeRid = sourceNodeRid.IsEmpty ? nodeRuntime.Node.RoutingId : sourceNodeRid;
        // A caller-routed frame forwarded to a moved actor carries no session
        // identity; only the reply-route node rid is mandatory. The target's
        // dispatch binds nothing for an identity-less frame.
        if (sessionNodeRid.IsEmpty) return false;
        ValidateRemoteActorFrameSource(
            actor.ActorId,
            routeContext,
            sessionNodeRid,
            sourceNodeGeneration,
            requestSource);
        var relayStatus = nodeRuntime.Node.MeshStatus();
        if (relayStatus.RoutingId.IsEmpty || relayStatus.LifecycleGeneration == 0)
            return false;
        var relayMessage = new ZLinkRemoteActorFrameRelay(
            actor.ActorId,
            actor.Generation,
            actor.NodeRid.ToHex(),
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            relayStatus.RoutingId.ToHex(),
            relayStatus.LifecycleGeneration,
            sessionNodeRid.ToHex(),
            sourceNodeGeneration,
            sourceSessionRid.ToHex(),
            requestSource?.OwnerId,
            requestSource?.LeaseGeneration ?? 0,
            requestSource is { } source ? source.NodeRid.ToHex() : null,
            requestSource?.NodeGeneration ?? 0,
            routeContext.OperationId.High,
            routeContext.OperationId.Low,
            routeContext.MessageFollowHopCount,
            routeContext.ReplyRequestId,
            routeContext.ReplyFlags,
            routeContext.ReplyCapability,
            routeContext.DeadlineUnixMs,
            applicationMetadata.ToArray(),
            header,
            body);
        var envelope = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Command,
            meshName,
            ZLinkRemoteActorFrameProtocol.PacketName);
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            envelope,
            relayMessage,
            typeof(ZLinkRemoteActorFrameRelay),
            Registration.Codecs);
        var submit = nodeRuntime.Node.SendToNode(actor.NodeRid, parts, SendFlags.DontWait);
        ZLinkMessageParts.DisposeAll(parts);
        return submit == SubmitResult.Ok;
    }

    internal static void ValidateRemoteActorFrameSource(
        string actorId,
        ZLinkBackendActorRouteContext routeContext,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource)
    {
        if (!routeContext.IsDirectRoute)
        {
            if (sourceNodeRid.IsEmpty
                || sourceNodeGeneration == 0
                || requestSource is not { } boundSource
                || boundSource.NodeRid != sourceNodeRid
                || boundSource.NodeGeneration != sourceNodeGeneration
                || string.IsNullOrWhiteSpace(boundSource.OwnerId)
                || boundSource.LeaseGeneration == 0)
            {
                //  This runs inside a one-way route send handler, so the throw
                //  reaches no caller and the frame simply disappears.
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Actor '{actorId}' bound-session relay source identity is stale.");
            }
            return;
        }

        if (sourceNodeRid.IsEmpty
            || sourceNodeGeneration == 0
            || requestSource is not { } source
            || string.IsNullOrWhiteSpace(source.OwnerId)
            || source.LeaseGeneration == 0
            || source.NodeRid != sourceNodeRid
            || source.NodeGeneration != sourceNodeGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actorId}' direct relay request-source identity is stale.");
    }

    /// <summary>Session-node entry for a relayed remote push: delivers the
    /// encoded frame to the still-bound local session, retrying backpressured
    /// writes within the request timeout (a stale binding drops the push per
    /// spec 31 §6).</summary>
    internal async ValueTask DeliverRemoteSessionPushAsync(
        ZLinkRemoteSessionPushRelay identity,
        byte[] frame,
        RoutingId sourceNodeRid,
        CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow + Registration.DefaultRequestTimeout;
        ZLinkActorBoundSessionCoordinator.RemotePushDelivery? previousDelivery = null;
        while (true)
        {
            var delivery = _actorBoundSessionCoordinator.DeliverLocalSessionFrame(
                identity,
                frame,
                sourceNodeRid);
            if (delivery != ZLinkActorBoundSessionCoordinator.RemotePushDelivery.NoBinding
                || previousDelivery != delivery)
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"session_push_delivery_result actor={identity.ActorId} "
                    + $"result={delivery} source_node={sourceNodeRid}");
            previousDelivery = delivery;
            // Backpressure and the transient release→bind gap of a rebind
            // both retry; a definite different-session binding drops the
            // push (spec 31 §6).
            var retryable = delivery
                is ZLinkActorBoundSessionCoordinator.RemotePushDelivery.Backpressured
                or ZLinkActorBoundSessionCoordinator.RemotePushDelivery.NoBinding;
            if (!retryable || DateTime.UtcNow >= deadline) return;
            await Task.Delay(TimeSpan.FromMilliseconds(10), cancellationToken)
                .ConfigureAwait(false);
        }
    }

    /// <summary>Actor-node relay for a push whose bound session lives on
    /// another node: wraps the frame in the internal node-addressed route
    /// packet. One-way push semantics — the submit runs on a detached runtime
    /// task so the actor's turn never blocks on route admission; failures are
    /// reported through the runtime task error sink.</summary>
    private bool RelayRemoteSessionPush(
        string actorId,
        ZLinkActorBoundSession session,
        byte[] frame)
    {
        var nodeRuntime = GetMeshNodeRuntime(session.MeshName);
        var actorRef = GetOrCreateActorState(actorId).NativeActorRef
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorId}' session route has no local Actor ref.");
        if (actorRef.Generation != session.ObjectGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actorId}' session route ObjectGeneration is stale.");
        var sessionNodeRid = session.SessionNodeRid
                             ?? throw new ZLinkFrameworkException(
                                 ZLinkFrameworkErrorKind.InvalidOperation,
                                 $"Actor '{actorId}' session route has no owner node.");
        var relayMessage = new ZLinkRemoteSessionPushRelay(
            actorId,
            session.ObjectGeneration,
            session.MeshName,
            actorRef.NodeRid.ToHex(),
            session.TargetNodeGeneration,
            session.AuthorityOwnerGeneration,
            session.OwnerLeaseGeneration,
            session.BindingToken,
            session.BindingGeneration,
            session.SessionOwnerNodeGeneration,
            session.SessionRid.ToHex(),
            frame);
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Command,
            session.MeshName,
            ZLinkRemoteSessionPushProtocol.PacketName);
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            header,
            relayMessage,
            typeof(ZLinkRemoteSessionPushRelay),
            Registration.Codecs);
        var submit = nodeRuntime.Node.SendToNode(sessionNodeRid, parts, SendFlags.DontWait);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"session_push_relay_submit actor={actorId} source_node={actorRef.NodeRid} "
            + $"target_node={sessionNodeRid} submit={submit} bytes={frame.Length}");
        ZLinkMessageParts.DisposeAll(parts);
        return submit == SubmitResult.Ok;
    }

    internal bool TryGetSessionActorContext(
        string actorId,
        string bindingToken,
        out ZLinkSessionContext context)
    {
        return _actorBoundSessionCoordinator.TryGetSessionActorContext(actorId, bindingToken, out context);
    }

    internal bool TryGetSessionActorContext(
        string actorId,
        out ZLinkSessionContext context)
    {
        return _actorBoundSessionCoordinator.TryGetSessionActorContext(actorId, out context);
    }

    internal bool TryGetSessionActorBinding(
        string actorId,
        string bindingToken,
        out ZLinkSessionBindingEntry entry) =>
        _actorBoundSessionCoordinator.TryGetSessionBinding(
            actorId,
            bindingToken,
            out entry);

    internal bool TryGetSessionActorRoute(
        string actorId,
        string bindingToken,
        ZLinkSessionActor actorRef,
        out ZLinkSessionBindingRoute route) =>
        _actorBoundSessionCoordinator.TryGetSessionRoute(
            actorId,
            bindingToken,
            actorRef,
            out route);

    internal bool TryGetSessionActorBinding(
        string actorId,
        out ZLinkSessionBindingEntry entry) =>
        _actorBoundSessionCoordinator.TryGetSessionBindingByActorId(
            actorId,
            out entry);

    internal IReadOnlyCollection<IZLinkSessionActor> SnapshotSessionActors(
        ZLinkSessionContext context) =>
        _actorBoundSessionCoordinator.SnapshotSessionActors(context);

    internal ZLinkSessionActor? FindSessionActor(
        ZLinkSessionContext context,
        string actorId) =>
        _actorBoundSessionCoordinator.FindSessionActor(context, actorId);

    internal ZLinkActorBoundSession? BindActorSession(
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
        return _actorBoundSessionCoordinator.BindActorSession(
            actorId,
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
    }

    internal void TombstoneSessionActorBinding(
        string actorId,
        RoutingId sessionRid,
        string bindingToken,
        ulong bindingGeneration,
        ulong sessionOwnerNodeGeneration,
        ZLinkSessionBindingRoute actorRoute)
    {
        _actorBoundSessionCoordinator.TombstoneSessionActorBinding(
            actorId,
            sessionRid,
            bindingToken,
            bindingGeneration,
            sessionOwnerNodeGeneration,
            actorRoute);
    }

    internal void UnbindActorSession(
        string actorId,
        string bindingToken)
    {
        _actorBoundSessionCoordinator.UnbindActorSession(actorId, bindingToken);
    }

    internal void RetireMigratedActorSession(
        string actorId,
        string bindingToken)
    {
        _actorBoundSessionCoordinator.RetireMigratedActorSession(
            actorId,
            bindingToken);
    }

    internal ValueTask FinalizeMigratedActorSourceAsync(
        ZLinkActorRuntimeState state,
        ZLinkBackendActorRef sourceActor)
    {
        return _actorSessionManager.FinalizeMigratedSourceAsync(
            state,
            sourceActor);
    }

    internal void RemoveActorSessionBinding(
        string actorId,
        string bindingToken)
    {
        _actorBoundSessionCoordinator.RemoveActorSessionBinding(actorId, bindingToken);
    }

    internal void CleanupActorSessionsForSession(RoutingId sessionRid)
    {
        _actorBoundSessionCoordinator.CleanupActorSessionsForSession(sessionRid);
    }

    internal bool TryGetActorBoundSession(
        string actorId,
        out ZLinkActorBoundSession session)
    {
        return _actorBoundSessionCoordinator.TryGetActorBoundSession(actorId, out session);
    }

    internal bool TryGetActorBoundSessionForOutbound(
        string actorId,
        out ZLinkActorBoundSession session)
    {
        return GetOrCreateActorState(actorId)
            .TryGetBoundSessionForOutbound(out session);
    }

    private async ValueTask ResetActorRuntimeGenerationAsync(
        CancellationToken cancellationToken = default,
        Action<Exception>? detachedCleanupFailure = null)
    {
        var failures = new List<Exception>();
        await CaptureAsync(
                () => _actorSessionManager.ResetGenerationAsync(
                    cancellationToken,
                    detachedCleanupFailure))
            .ConfigureAwait(false);
        using var cleanupDeadline = new CancellationTokenSource(
            TimeSpan.FromSeconds(5));
        await CaptureAsync(
                () => _actorHandoffAdmissions.ResetGenerationAsync(
                    cleanupDeadline.Token))
            .ConfigureAwait(false);
        await CaptureAsync(
                _actorSessionManager.ResetBoundSessionGenerationAsync)
            .ConfigureAwait(false);
        Capture(_actorBoundSessionCoordinator.ResetGeneration);
        ThrowCleanupFailures(failures);
        return;

        async ValueTask CaptureAsync(Func<ValueTask> cleanup)
        {
            try
            {
                await cleanup().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        }

        void Capture(Action cleanup)
        {
            try
            {
                cleanup();
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        }
    }

    internal bool SendActorBoundSession(
        string actorId,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        return _actorBoundSessionCoordinator.Send(actorId, parts, flags);
    }

    internal bool SendActorBoundSessionIfCurrent(
        string actorId,
        string expectedBindingToken,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        return _actorBoundSessionCoordinator.SendIfBoundTo(
            actorId,
            expectedBindingToken,
            parts,
            flags);
    }

    internal ZLinkAsyncSubmitter CreateActorBoundSessionSubmitter(
        string meshName)
    {
        return _actorBoundSessionCoordinator.CreateSubmitter(meshName);
    }

    internal async ValueTask ReplyActorNoBindAsync(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ulong requestId,
        uint flags,
        string? replyCapability,
        IReadOnlyList<Message> parts)
    {
        var nodeRuntime = actor.NodeRid.IsEmpty
            ? GetActorClientSpotNodeRuntime()
            : GetSpotNodeRuntime(actor.NodeRid);
        var replyDeadlineUnixMs = 0UL;
        var preservedReplyNodeRid = default(RoutingId);
        var capabilityResolved = !string.IsNullOrWhiteSpace(replyCapability)
                                 && _actorMessageFollower.TryResolveReplyRoute(
                                     replyCapability,
                                     out preservedReplyNodeRid,
                                     out replyDeadlineUnixMs);
        var replyNodeRid = capabilityResolved
            ? preservedReplyNodeRid
            : sourceNodeRid;
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_reply_route actor={actor.ActorId} request_id={requestId} "
            + $"actor_node={nodeRuntime.Node.RoutingId} reply_node={replyNodeRid} "
            + $"source_node={sourceNodeRid} capability_resolved={capabilityResolved}");

        if (!replyNodeRid.IsEmpty
            && !replyNodeRid.Equals(nodeRuntime.Node.RoutingId))
        {
            if (string.IsNullOrWhiteSpace(replyCapability))
            {
                ZLinkMessageParts.DisposeAll(parts);
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ProtocolError,
                    "Remote Actor reply did not preserve its reply capability.");
            }
            var frameLength = parts.Sum(static part => checked((int)part.Size));
            var frame = new byte[frameLength];
            var offset = 0;
            foreach (var part in parts)
            {
                part.AsReadOnlySpan().CopyTo(frame.AsSpan(offset));
                offset += checked((int)part.Size);
            }

            var meshName = nodeRuntime.Registration.SpotMeshChannelName
                           ?? nodeRuntime.Registration.SpotNodeName;
            var relay = new ZLinkRemoteActorReplyRelay(
                actor.ActorId,
                requestId,
                flags,
                replyCapability,
                nodeRuntime.Node.RoutingId.ToHex(),
                frame);
            var envelope = ZLinkClientCallCodec.CreateEnvelope(
                ZLinkMessageKind.Command,
                meshName,
                ZLinkRemoteActorReplyProtocol.PacketName);
            var relayParts = ZLinkEnvelopeCodec.EncodeParts(
                envelope,
                relay,
                typeof(ZLinkRemoteActorReplyRelay),
                Registration.Codecs);
            try
            {
                var timeout = RemainingReplyTime(
                    replyDeadlineUnixMs,
                    Registration.DefaultRequestTimeout);
                if (timeout <= TimeSpan.Zero)
                    return;
                var attempt = 0;
                await ZLinkRetryingSubmitter.Async(
                        () =>
                        {
                            var result = nodeRuntime.Node.SendToNode(
                                replyNodeRid,
                                relayParts,
                                SendFlags.DontWait);
                            if (attempt++ == 0 || result is not SubmitResult.Backpressured)
                                ZLinkFrameworkDebugLog.SpotDiscovery(
                                    $"actor_reply_relay_submit actor={actor.ActorId} request_id={requestId} "
                                    + $"target_node={replyNodeRid} attempt={attempt} result={result}");
                            return result;
                        },
                        timeout,
                        $"Actor reply relay to node '{replyNodeRid}' was not admitted.",
                        ShutdownToken)
                    .ConfigureAwait(false);
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"actor_reply_relay_submitted actor={actor.ActorId} request_id={requestId} "
                    + $"target_node={replyNodeRid} attempts={attempt}");
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(relayParts);
                ZLinkMessageParts.DisposeAll(parts);
            }
            return;
        }

        if (!string.IsNullOrWhiteSpace(replyCapability)
            && await _actorMessageFollower.TryCompleteLocalDirectReplyAsync(
                actor.ActorId,
                requestId,
                flags,
                replyCapability,
                parts,
                ShutdownToken).ConfigureAwait(false))
            return;

        if (_actorBoundSessionCoordinator.ReplyNoBind(
                actor, sourceNodeRid, sourceSessionRid, requestId, flags, parts))
            return;

        ZLinkMessageParts.DisposeAll(parts);
    }

    private static TimeSpan RemainingReplyTime(
        ulong deadlineUnixMs,
        TimeSpan fallback)
    {
        if (deadlineUnixMs is 0 or > long.MaxValue)
            return fallback;
        var remaining = checked(
            (long)deadlineUnixMs
            - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
        return remaining <= 0
            ? TimeSpan.Zero
            : TimeSpan.FromMilliseconds(remaining);
    }

    internal async ValueTask DeliverRemoteActorReplyAsync(
        string actorId,
        ulong requestId,
        uint flags,
        string replyCapability,
        RoutingId sourceNodeRid,
        RoutingId responderNodeRid,
        byte[] frame,
        CancellationToken cancellationToken)
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"remote_actor_reply_received actor={actorId} request_id={requestId} "
            + $"source_node={sourceNodeRid} responder_node={responderNodeRid} "
            + $"flags={flags} capability={(!string.IsNullOrWhiteSpace(replyCapability))}");
        if (await _actorMessageFollower.TryCompleteDirectReplyAsync(
                actorId,
                requestId,
                flags,
                replyCapability,
                sourceNodeRid,
                responderNodeRid,
                frame,
                cancellationToken).ConfigureAwait(false))
            return;
        if (!_actorBoundSessionCoordinator.TryClaimRemoteSessionReply(
                actorId,
                requestId,
                flags,
                replyCapability,
                sourceNodeRid,
                responderNodeRid,
                out var claim))
            return;

        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"remote_actor_reply_claimed actor={actorId} request_id={requestId}");

        var deadline = DateTime.UtcNow + Registration.DefaultRequestTimeout;
        using (claim)
        {
            while (true)
            {
                var delivery = claim.Deliver(frame);
                var retryable = delivery
                    is ZLinkActorBoundSessionCoordinator.RemotePushDelivery.Backpressured
                    or ZLinkActorBoundSessionCoordinator.RemotePushDelivery.NoBinding;
                if (!retryable || DateTime.UtcNow >= deadline)
                    return;
                await Task.Delay(TimeSpan.FromMilliseconds(10), cancellationToken)
                    .ConfigureAwait(false);
            }
        }
    }

    internal bool ForwardActorBoundSessionPart(
        string meshName,
        ZLinkBackendActorRef actorRef,
        ulong targetNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Message message,
        bool hasMore,
        SendFlags flags,
        ZLinkBackendActorRouteContext routeContext = default,
        ulong sourceNodeGeneration = 0,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource = null,
        ReadOnlyMemory<byte> applicationMetadata = default)
    {
        return _actorBoundSessionCoordinator.ForwardPart(
            actorRef,
            sourceNodeRid,
            sourceSessionRid,
            message,
            hasMore,
            flags,
            meshName,
            GetMeshNodeRuntime(meshName).Node,
            targetNodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration,
            routeContext,
            sourceNodeGeneration,
            requestSource,
            applicationMetadata);
    }

    internal ValueTask CloseActorBoundSessionAsync(
        string actorId,
        CancellationToken cancellationToken)
    {
        return _actorBoundSessionCoordinator.CloseAsync(actorId, cancellationToken);
    }
}

internal readonly record struct ZLinkRelocationTargetSelection(
    ZLinkFrameworkRelocationMode Mode,
    long ApplicationVersion)
{
    internal bool Matches(ZLinkMeshNodeDescriptor descriptor) =>
        Matches(descriptor.ApplicationVersion);

    internal bool Matches(long applicationVersion) =>
        applicationVersion == ApplicationVersion;
}
