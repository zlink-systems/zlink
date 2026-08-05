namespace Zlink.Framework.Runtime.Host;

internal sealed class ZLinkFrameworkActorFacade(
    ZLinkFrameworkRuntime runtime,
    ZLinkFrameworkRegistration registration,
    IServiceProvider services,
    ZLinkSpotRuntimeManager spots,
    ZLinkActorSessionManager actorSessionManager,
    Func<ZLinkFrameworkComponentState> getState,
    Func<IZLinkBackendSpotNode?> getActorSpotNode)
{
    private long _nextEntrySpotSelection;

    private readonly ZLinkActorEntrySpotJoinCoordinator _entrySpotJoin = new(
        registration,
        spots,
        actorSessionManager,
        getState,
        getActorSpotNode,
        runtime.Flow);

    private readonly ZLinkActorRemoteJoiner _remoteJoiner = new(
        runtime,
        registration,
        services,
        spots,
        actorSessionManager);

    public async ValueTask<ZLinkActorJoinResult> JoinActorAsync(
        string spotId,
        IZLinkActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken = default)
    {
        return await JoinActorAsync(
                spotId,
                actor,
                request,
                operationId: null,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask<ZLinkActorJoinResult> JoinActorAsync(
        string spotId,
        IZLinkActor actor,
        ZLinkMessage request,
        ZLinkActorJoinOperationId? operationId,
        CancellationToken cancellationToken,
        DateTimeOffset? absoluteDeadline = null)
    {
        var effectiveDeadline = absoluteDeadline
                                ?? DateTimeOffset.UtcNow
                                   + registration.DefaultRequestTimeout;
        var state = getState();
        var actorState = actorSessionManager.GetOrCreateState(actor.Context.ActorId);
        var node = getActorSpotNode();
        var localActivation = spots.GetActivationBySpotId(state, spotId);

        if (localActivation is null
            && node is not null
            && actorState.NativeActorRef is { } actorRef)
            return await _remoteJoiner.JoinAsync(
                state,
                spotId,
                actor,
                actorRef,
                node,
                request,
                operationId,
                cancellationToken,
                effectiveDeadline).ConfigureAwait(false);

        ZLinkSpotActorJoinResult joinResult;
        var sourceActivation = actorState.Activation;
        if (localActivation is not null
            && ReferenceEquals(sourceActivation, localActivation))
            return new ZLinkActorJoinResult.Accepted(
                ToActorRef(actorState),
                ZLinkMessage.Empty);

        if (localActivation is not null
            && sourceActivation is not null
            && !ReferenceEquals(sourceActivation, localActivation))
        {
            joinResult = await localActivation.AdmitActorJoinFromCallerTurnAsync(
                    actor,
                    request,
                    cancellationToken)
                .ConfigureAwait(false);
            if (joinResult.Accepted)
                await localActivation.CommitActorJoinFromCallerTurnAsync(
                        actor,
                        cancellationToken,
                        effectiveDeadline)
                    .ConfigureAwait(false);
        }
        else if (localActivation is not null)
            joinResult = await localActivation.JoinActorAsync(
                    actor,
                    request,
                    cancellationToken,
                    effectiveDeadline)
                .ConfigureAwait(false);
        else
            joinResult = await spots.JoinActorAsync(
                state,
                spotId,
                actor,
                request,
                cancellationToken,
                effectiveDeadline).ConfigureAwait(false);
        var reply = joinResult.Reply ?? ZLinkMessage.Empty;
        return joinResult.Accepted
            ? new ZLinkActorJoinResult.Accepted(ToActorRef(actorState), reply)
            : RejectedWithTrace(reply, "facade");
    }

    public async ValueTask<ZLinkActorJoinResult> JoinActorEntrySpotAsync(
        IZLinkActor actor,
        ZLinkMessage request,
        ZLinkActorJoinOperationId? operationId,
        CancellationToken cancellationToken,
        DateTimeOffset? absoluteDeadline = null)
    {
        var actorState = actorSessionManager.GetOrCreateState(actor.Context.ActorId);
        if (actorState.LiveActivation is null)
            return new ZLinkActorJoinResult.Accepted(
                ToActorRef(actorState),
                ZLinkMessage.Empty);

        var actorType = actorState.ActorType ?? actor.GetType().Name;
        var meshName = actorState.Context?.MeshName
                       ?? ZLinkActorDrainCoordinator.ResolveMeshName(registration, actorType)
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actor.Context.ActorId}' does not have an owner Mesh.");
        var store = registration.Locations.ResolveStore()
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.InvalidOperation,
                        "Actor Entry Spot Join requires a Location Store.");
        var descriptors = await store.ListAllMeshNodesAsync(meshName, cancellationToken)
            .ConfigureAwait(false);
        var eligible = descriptors
            .Where(candidate => ZLinkActorManagerService.IsEligibleCandidate(
                candidate,
                actorType))
            .OrderBy(static candidate => candidate.Rid.ToHex(), StringComparer.Ordinal)
            .ToArray();
        var target = ZLinkWeightedSelector.Select(
                         eligible,
                         static candidate => candidate.PlacementWeight,
                         ref _nextEntrySpotSelection)
                     ?? throw new ZLinkFrameworkException(
                         ZLinkFrameworkErrorKind.CapacityExceeded,
                         $"No Ready Entry Spot target is available for '{actorType}'.",
                         ZLinkRetryAdvice.RetryAfterBackoff);

        var sourceNodeRid = actorState.NativeActorRef?.NodeRid
                            ?? throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.NotFound,
                                $"Actor '{actor.Context.ActorId}' does not have a current node identity.");
        if (target.Rid == sourceNodeRid)
            return await _entrySpotJoin.JoinAsync(
                    target.Rid,
                    actor,
                    request,
                    cancellationToken)
                .ConfigureAwait(false);

        var actorRef = actorState.NativeActorRef
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actor.Context.ActorId}' does not have a current native reference.");
        return await _remoteJoiner.JoinEntrySpotAsync(
                target,
                actor,
                actorRef,
                request,
                operationId,
                cancellationToken,
                absoluteDeadline)
            .ConfigureAwait(false);
    }

    public async ValueTask<ZLinkActorJoinResult> JoinActorEntrySpotAsync(
        RoutingId spotNodeRid,
        IZLinkActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken = default)
    {
        return await _entrySpotJoin.JoinAsync(spotNodeRid, actor, request, cancellationToken)
            .ConfigureAwait(false);
    }

    private static ActorRef ToActorRef(ZLinkActorRuntimeState actorState)
    {
        var actorRef = actorState.NativeActorRef
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorState.ActorId}' does not have a native Actor ref.");
        var meshName = actorState.Activation?.MeshName
                       ?? actorState.Context?.MeshName
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorState.ActorId}' does not have an owner Mesh.");
        return actorRef.ToNative(meshName);
    }

    private static ZLinkActorJoinResult.Rejected RejectedWithTrace(
        ZLinkMessage reply,
        string site)
    {
        Zlink.Framework.Runtime.Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_join_rejected site={site}");
        return new ZLinkActorJoinResult.Rejected(reply);
    }
}
