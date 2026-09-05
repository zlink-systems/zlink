using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Backend.DotNet.Mappings;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkActorManagerService(ZLinkFrameworkRuntime runtime) : IZLinkActorManager
{
    private long _nextPlacementSelection;
    private readonly IZLinkMeshNodeLocationResolver? _locationResolver =
        runtime.Services.GetService<IZLinkMeshNodeLocationResolver>()
        ?? runtime.Services.GetService<ZLinkStoreLocationResolvers>();

    public IZLinkActorCreateCall Create(string actorId, string actorType) =>
        new ZLinkActorCreateCall(
            runtime.Registration.DefaultRequestTimeout,
            (meshName, request, timeout, cancellationToken) =>
                SubmitAsync(actorId, actorType, true, meshName, request, timeout, cancellationToken));

    public IZLinkActorGetOrCreateCall GetOrCreate(string actorId, string actorType) =>
        new ZLinkActorGetOrCreateCall(
            runtime.Registration.DefaultRequestTimeout,
            (meshName, request, timeout, cancellationToken) =>
                SubmitAsync(actorId, actorType, false, meshName, request, timeout, cancellationToken));

    public async ValueTask<ActorRef?> FindAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        using var operation = runtime.EnterOperationalRead();
        cancellationToken.ThrowIfCancellationRequested();
        var store = runtime.Registration.Locations.ResolveStore();
        if (store is null)
            return runtime.TryGetCreatedActorState(actorId, out var local)
                   && local.NativeActorRef is { } actorRef
                ? actorRef.ToNative(
                    local.Activation?.MeshName
                    ?? local.Context?.MeshName
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{actorId}' does not have an owner Mesh."))
                : null;
        var read = await store.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId),
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found
            || found.Snapshot.Allocation.State != ZLinkPlacementAllocationState.Active
            || !ZLinkActorAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var authority)
            || authority.State != ZLinkActorAuthorityState.Ready
            || !await HasLiveOwnerAsync(
                    store,
                    found.Snapshot,
                    cancellationToken)
                .ConfigureAwait(false))
            return null;
        return new ActorRef(
            actorId,
            found.Snapshot.ObjectGeneration,
            authority.MeshName,
            authority.NodeRid);
    }

    public async ValueTask<SpotRef?> FindSpotAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        using var operation = runtime.EnterOperation();
        cancellationToken.ThrowIfCancellationRequested();
        var store = runtime.Registration.Locations.ResolveStore();
        if (store is null)
        {
            if (!runtime.TryGetCreatedActorState(actorId, out var state)
                || state.SpotId is not { } spotId)
                return null;
            return await runtime.FindAsync(spotId, cancellationToken).ConfigureAwait(false);
        }
        var read = await store.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId),
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found
            || found.Snapshot.Allocation.State != ZLinkPlacementAllocationState.Active
            || !ZLinkActorAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var authority)
            || authority.State != ZLinkActorAuthorityState.Ready
            || !await HasLiveOwnerAsync(
                    store,
                    found.Snapshot,
                    cancellationToken)
                .ConfigureAwait(false))
            return null;
        return new SpotRef(
            authority.CurrentSpotId,
            authority.CurrentSpotGeneration,
            authority.MeshName,
            authority.NodeRid);
    }

    public async ValueTask<bool> DestroyAsync(
        ActorRef actor,
        CancellationToken cancellationToken = default)
    {
        using var operation = runtime.EnterOperation();
        cancellationToken.ThrowIfCancellationRequested();
        var store = runtime.Registration.Locations.ResolveStore();
        if (store is null)
        {
            if (!runtime.TryGetCreatedActorState(actor.ActorId, out var state)
                || state.NativeActorRef is not { } current
                || state.Actor is not { } instance)
                return false;
            if (current.Generation != actor.ObjectGeneration)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"Actor '{actor.ActorId}' generation is stale.");
            await runtime.DestroyActorAsync(
                    state.LiveActivation?.NodeRid ?? current.NodeRid,
                    instance,
                    cancellationToken)
                .ConfigureAwait(false);
            return true;
        }

        var authorityKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId);
        var read = await store.ReadAuthorityAsync(
                authorityKey,
                cancellationToken)
            .ConfigureAwait(false);
        if (read is ZLinkAuthorityReadResult.Missing)
            return false;
        var snapshot = ((ZLinkAuthorityReadResult.Found)read).Snapshot;
        if (snapshot.ObjectGeneration != actor.ObjectGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actor.ActorId}' generation is stale.");
        if (snapshot.Allocation.State != ZLinkPlacementAllocationState.Active
            || snapshot.Allocation.ObjectKind != ZLinkPlacementObjectKind.Actor
            || !ZLinkActorAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var authority)
            || authority.State != ZLinkActorAuthorityState.Ready)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actor.ActorId}' is moving.",
                ZLinkRetryAdvice.RetryAfterBackoff);

        // MeshName and NodeRid in ActorRef are route snapshots. Exact destroy
        // fences the logical incarnation by ActorId + ObjectGeneration and
        // always targets the current authority owner.
        var currentRef = new ActorRef(
            actor.ActorId,
            snapshot.ObjectGeneration,
            authority.MeshName,
            authority.NodeRid);
        var source = runtime.ResolveActorCreationSource(authority.MeshName);
        if (authority.NodeRid == source.Node.RoutingId)
        {
            var local = await source.DestroyActorLocalAsync(
                    new ActorDestroyOperation(
                        1,
                        currentRef,
                        authority.NodeRid,
                        authority.NodeGeneration,
                        snapshot.AuthorityOwnerGeneration,
                        checked((ulong)snapshot.OwnerLeaseGeneration)),
                    cancellationToken)
                .ConfigureAwait(false);
            return local.Completion?.Destroyed
                   ?? throw new ZLinkFrameworkException(
                       ZLinkFrameworkErrorKind.Unavailable,
                       $"Actor '{actor.ActorId}' local destroy did not produce a terminal result.",
                       ZLinkRetryAdvice.RetryAfterBackoff);
        }

        return await source.Node.DestroyActorRemoteAsync(
                currentRef.ToBackend(),
                authority.NodeGeneration,
                snapshot.AuthorityOwnerGeneration,
                checked((ulong)snapshot.OwnerLeaseGeneration),
                runtime.Registration.DefaultRequestTimeout,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkActorCreateResult> SubmitAsync(
        string actorId,
        string actorType,
        bool createOnly,
        string? meshName,
        ZLinkMessage createRequest,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        try
        {
            return await SubmitCoreAsync(
                    actorId,
                    actorType,
                    createOnly,
                    meshName,
                    createRequest,
                    timeout,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            throw CreateActorCreationUnavailableException(actorId);
        }
        catch (TimeoutException error) when (!cancellationToken.IsCancellationRequested)
        {
            throw CreateActorCreationUnavailableException(actorId, error);
        }
    }

    private async ValueTask<ZLinkActorCreateResult> SubmitCoreAsync(
        string actorId,
        string actorType,
        bool createOnly,
        string? meshName,
        ZLinkMessage createRequest,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadlineAt = DateTimeOffset.UtcNow.Add(timeout);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(timeout);
        using var operation = runtime.EnterOperation();
        var source = runtime.ResolveActorCreationSource(meshName);
        var store = runtime.Registration.Locations.ResolveStore()
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.InvalidOperation,
                        "Actor creation requires a Location Store.");
        var selectedMesh = source.Registration.SpotMeshChannelName
                           ?? source.Registration.SpotNodeName;
        var descriptors = await ListLiveMeshNodesAsync(selectedMesh, deadline.Token)
            .ConfigureAwait(false);
        var placementEligible = descriptors
            .Where(candidate => IsEligibleCandidate(candidate, actorType))
            .OrderBy(static candidate => candidate.Rid, ZLinkRoutingIdOrder.Instance)
            .ToList();
        var eligible = FilterRouteReadyCandidates(source, placementEligible);
        var encoded = createRequest.Encode(runtime.Registration.Codecs);
        var applicationPayload = ZLinkApplicationPayloadEnvelopeCodec.Encode(
            ZLinkApplicationPayloadEnvelopeCodec.CreationPacketName,
            encoded.ContentType,
            encoded.Payload.Bytes.Span);
        var applicationHash = System.Security.Cryptography.SHA256.HashData(applicationPayload);
        var contentReference = ZLinkInlineCreationIntentCodec.Encode(applicationPayload);
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var reservationRefreshAttempt = 0;
        var joinRetryAttempt = 0;
        while (true)
        {
            var target = ZLinkWeightedSelector.Select(
                    eligible,
                    static candidate => candidate.PlacementWeight,
                    ref _nextPlacementSelection);
            if (target is null
                && placementEligible.Count == 0
                && reservationRefreshAttempt == 0)
            {
                var anyCompatible = descriptors.Any(
                    candidate => IsCompatibleCandidate(candidate, actorType));
                throw new ZLinkFrameworkException(
                    anyCompatible
                        ? ZLinkFrameworkErrorKind.CapacityExceeded
                        : ZLinkFrameworkErrorKind.Unavailable,
                    anyCompatible
                        ? $"No Ready Actor target has placement capacity for '{actorType}'."
                        : $"No compatible Actor target is available for '{actorType}'.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            }
            if (target is null)
            {
                var backoffMilliseconds =
                    1 << Math.Min(reservationRefreshAttempt++, 6);
                await Task.Delay(
                        TimeSpan.FromMilliseconds(backoffMilliseconds),
                        deadline.Token)
                    .ConfigureAwait(false);
                descriptors = await ListLiveMeshNodesAsync(
                        selectedMesh,
                        deadline.Token)
                    .ConfigureAwait(false);
                placementEligible = descriptors
                    .Where(candidate => IsEligibleCandidate(candidate, actorType))
                    .OrderBy(
                        static candidate => candidate.Rid,
                        ZLinkRoutingIdOrder.Instance)
                    .ToList();
                eligible = FilterRouteReadyCandidates(
                    source,
                    placementEligible);
                continue;
            }
            var owner = new ZLinkLocationOwnerToken(target.OwnerId, target.LeaseGeneration);
            var creating = ZLinkActorAuthorityPayloadCodec.Encode(
                new ZLinkActorAuthorityPayload(
                    ZLinkActorAuthorityState.Creating,
                    actorType,
                    actorId,
                    target.EntrySpotId!,
                    target.LifecycleGeneration,
                    ZLinkSpotKind.Entry,
                    owner.OwnerId,
                    checked((ulong)owner.LeaseGeneration),
                    selectedMesh,
                    target.Rid,
                    target.LifecycleGeneration));
            var reserve = await store.ReserveAsync(
                    new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.Actor,
                        key,
                        actorType,
                        contentReference,
                        applicationHash,
                        applicationPayload.Length,
                        new ZLinkMeshNodeDescriptorKey(selectedMesh, target.Rid),
                        target.LifecycleGeneration,
                        owner,
                        creating,
                        new ZLinkCapacityVector(1, 0, null)),
                    deadline.Token)
                .ConfigureAwait(false);
            if (reserve is ZLinkObjectReserveResult.PlacementCapacityExhausted)
            {
                eligible.Remove(target);
                continue;
            }
            if (reserve is ZLinkObjectReserveResult.TypeMismatch)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.TypeMismatch,
                    $"Actor '{actorId}' is not registered as '{actorType}'.");
            if (reserve is ZLinkObjectReserveResult.AlreadyExists existing)
            {
                if (createOnly)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.AlreadyExists,
                        $"Actor '{actorId}' already exists.");
                var joined = await JoinExistingAsync(
                        store,
                        key,
                        actorId,
                        actorType,
                        existing.Current,
                        deadline.Token)
                    .ConfigureAwait(false);
                if (joined is not null)
                    return joined;
                await DelayJoinRetryAsync(
                        actorId,
                        joinRetryAttempt++,
                        deadline.Token)
                    .ConfigureAwait(false);
                continue;
            }
            if (!createOnly
                && reserve is ZLinkObjectReserveResult.Conflict(
                    ZLinkAuthorityReadResult.Found found))
            {
                var joined = await JoinExistingAsync(
                        store,
                        key,
                        actorId,
                        actorType,
                        found.Snapshot,
                        deadline.Token)
                    .ConfigureAwait(false);
                if (joined is not null)
                    return joined;
                await DelayJoinRetryAsync(
                        actorId,
                        joinRetryAttempt++,
                        deadline.Token)
                    .ConfigureAwait(false);
                continue;
            }
            if (reserve is ZLinkObjectReserveResult.Conflict(
                ZLinkAuthorityReadResult.Missing))
            {
                var backoffMilliseconds =
                    1 << Math.Min(reservationRefreshAttempt++, 6);
                await Task.Delay(
                        TimeSpan.FromMilliseconds(backoffMilliseconds),
                        deadline.Token)
                    .ConfigureAwait(false);
                descriptors = await ListLiveMeshNodesAsync(
                        selectedMesh,
                        deadline.Token)
                    .ConfigureAwait(false);
                placementEligible = descriptors
                    .Where(candidate => IsEligibleCandidate(candidate, actorType))
                    .OrderBy(
                        static candidate => candidate.Rid,
                        ZLinkRoutingIdOrder.Instance)
                    .ToList();
                eligible = FilterRouteReadyCandidates(
                    source,
                    placementEligible);
                continue;
            }
            if (reserve is not ZLinkObjectReserveResult.Reserved reserved)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Actor '{actorId}' creation reservation changed.",
                    ZLinkRetryAdvice.RetryAfterBackoff);

            var reservation = reserved.Reservation;
            try
            {
                var fence = new ObjectReservationFence(
                    reservation.ReservationVersion,
                    reservation.StoreVersion,
                    reservation.ObjectGeneration,
                    reservation.AuthorityOwnerGeneration,
                    target.Rid,
                    target.LifecycleGeneration,
                    owner.OwnerId,
                    checked((ulong)owner.LeaseGeneration),
                    1);
                var deadlineUnixMs = checked((ulong)deadlineAt.ToUnixTimeMilliseconds());
                if (target.Rid == source.Node.RoutingId)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"actor_create_local actor={actorId} target={target.Rid} "
                        + $"generation={target.LifecycleGeneration}");
                    try
                    {
                        var local = await source.CreateActorLocalAsync(
                                actorId,
                                actorType,
                                fence,
                                deadlineUnixMs,
                                deadline.Token)
                            .ConfigureAwait(false);
                        return DecodeRemoteResult(local.Completion, local.Reply);
                    }
                    catch (OperationCanceledException error)
                        when (!cancellationToken.IsCancellationRequested)
                    {
                        throw CreateActorCreationDeadlineException(actorId, error);
                    }
                }

                var remaining = deadlineAt - DateTimeOffset.UtcNow;
                if (remaining <= TimeSpan.Zero)
                    throw CreateActorCreationUnavailableException(actorId);
                // The mesh durable sender keeps this reservation's encoded
                // OperationId until a terminal or the original deadline.
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"actor_create_remote actor={actorId} source={source.Node.RoutingId} "
                    + $"target={target.Rid} generation={target.LifecycleGeneration}");
                var remote = await source.Node.CreateActorRemoteAsync(
                        target.Rid,
                        actorId,
                        actorType,
                        fence,
                        deadlineUnixMs,
                        remaining,
                        cancellationToken)
                    .ConfigureAwait(false);
                return DecodeRemoteResult(remote.Completion, remote.Reply);
            }
            catch (ZlinkSubmitException)
            {
                await store.AbortAsync(reservation, CancellationToken.None)
                    .ConfigureAwait(false);
                throw;
            }
            catch (OperationCanceledException)
            {
                await store.AbortAsync(reservation, CancellationToken.None)
                    .ConfigureAwait(false);
                throw;
            }
            catch (TimeoutException)
            {
                await store.AbortAsync(reservation, CancellationToken.None)
                    .ConfigureAwait(false);
                throw;
            }
            catch (ZLinkFrameworkException)
            {
                await store.AbortAsync(reservation, CancellationToken.None)
                    .ConfigureAwait(false);
                // A received Failed terminal also ends the operation. Target
                // reselection here would create a different durable identity.
                throw;
            }
        }
    }

    private static ZLinkFrameworkException CreateActorCreationUnavailableException(
        string actorId,
        Exception? innerException = null) =>
        new(ZLinkFrameworkErrorKind.Unavailable,
            $"Actor '{actorId}' creation was not admitted before its deadline.",
            ZLinkRetryAdvice.RetryAfterBackoff,
            innerException);

    internal static ZLinkFrameworkException CreateActorCreationDeadlineException(
        string actorId,
        Exception? innerException = null) =>
        new(
            ZLinkFrameworkErrorKind.DeadlineExceeded,
            $"Actor '{actorId}' creation deadline elapsed.",
            ZLinkRetryAdvice.RetryAfterBackoff,
            innerException);

    //  A compatible/live target: serving, a server, placeable, has an Entry
    //  Spot, and advertises the Actor type. Capacity is a separate axis so an
    //  absent compatible target (Unavailable) is distinguished from a compatible
    //  target that is out of placement capacity (CapacityExceeded), spec
    //  15-spot-actor:367-368.
    internal static bool IsCompatibleCandidate(
        ZLinkMeshNodeDescriptor candidate,
        string actorType) =>
        candidate.State == ZLinkFrameworkRuntimeState.Serving
        && candidate.ObjectRole == ZLinkMeshNodeObjectRole.Server
        && candidate.PlacementWeight > 0
        && candidate.EntrySpotId is not null
        && candidate.ObjectCapabilities.Any(capability =>
            capability.ObjectKind == ZLinkPlacementObjectKind.Actor
            && string.Equals(capability.StableType, actorType, StringComparison.Ordinal));

    private static bool HasActorCapacity(ZLinkMeshNodeDescriptor candidate) =>
        candidate.Capacity.Actors.Limit == 0
        || candidate.Capacity.Actors.Active
            + (long)candidate.Capacity.Actors.Reserved
            < candidate.Capacity.Actors.Limit;

    internal static bool IsEligibleCandidate(
        ZLinkMeshNodeDescriptor candidate,
        string actorType) =>
        IsCompatibleCandidate(candidate, actorType) && HasActorCapacity(candidate);

    private static List<ZLinkMeshNodeDescriptor> FilterRouteReadyCandidates(
        ZLinkSpotNodeRuntime source,
        IReadOnlyList<ZLinkMeshNodeDescriptor> candidates,
        IReadOnlySet<ZLinkMeshNodeTargetAvailability.PeerEpoch>?
            unavailablePeerEpochs = null) =>
        ZLinkMeshNodeTargetAvailability.FilterAdmitted(
                source.Node.RoutingId,
                candidates,
                source.Node.MeshPeers(),
                unavailablePeerEpochs)
            .ToList();

    private ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
        string meshName,
        CancellationToken cancellationToken)
    {
        if (_locationResolver is null)
            throw new ZLinkConfigurationException(
                "Actor creation requires the live MeshNode resolver.");
        return _locationResolver.ListLiveMeshNodesAsync(
            meshName,
            cancellationToken);
    }

    private async ValueTask<ZLinkActorCreateResult?> JoinExistingAsync(
        IZLinkLocationRepository store,
        ZLinkAuthorityKey key,
        string actorId,
        string actorType,
        ZLinkAuthoritySnapshot current,
        CancellationToken cancellationToken)
    {
        while (true)
        {
            if (!await HasLiveOwnerAsync(
                    store,
                    current,
                    cancellationToken)
                .ConfigureAwait(false))
                return null;
            if (current.Allocation.ObjectKind != ZLinkPlacementObjectKind.Actor
                || !string.Equals(current.Allocation.StableType, actorType, StringComparison.Ordinal)
                || !ZLinkActorAuthorityPayloadCodec.TryDecode(current.Payload.Span, out var authority))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.TypeMismatch,
                    $"Actor '{actorId}' does not use type '{actorType}'.");
            if (current.Allocation.State == ZLinkPlacementAllocationState.Active
                && authority.State == ZLinkActorAuthorityState.Ready)
                return new ZLinkActorCreateResult.Existing(
                    new ActorRef(
                        actorId,
                        current.ObjectGeneration,
                        authority.MeshName,
                        authority.NodeRid));
            if (authority.State != ZLinkActorAuthorityState.Creating)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"Actor '{actorId}' creation state is invalid.",
                    ZLinkRetryAdvice.RetryAfterBackoff);

            await Task.Delay(TimeSpan.FromMilliseconds(10), cancellationToken)
                .ConfigureAwait(false);
            var read = await store.ReadAuthorityAsync(key, cancellationToken)
                .ConfigureAwait(false);
            if (read is ZLinkAuthorityReadResult.Found next)
            {
                current = next.Snapshot;
                continue;
            }
            return null;
        }
    }

    private static async ValueTask<bool> HasLiveOwnerAsync(
        IZLinkLocationRepository store,
        ZLinkAuthoritySnapshot snapshot,
        CancellationToken cancellationToken)
    {
        var lease = await store.ReadOwnerLeaseAsync(
                snapshot.OwnerId,
                cancellationToken)
            .ConfigureAwait(false);
        return lease is ZLinkOwnerLeaseReadResult.Found found
               && found.Token == new ZLinkLocationOwnerToken(
                   snapshot.OwnerId,
                   snapshot.OwnerLeaseGeneration)
               && found.LeaseExpiresAt > found.StoreNow;
    }

    private ZLinkActorCreateResult DecodeRemoteResult(
        ActorCreateCompletion completion,
        IReadOnlyList<Message> replyParts)
    {
        ZLinkMessage? reply = null;
        try
        {
            if (replyParts.Count > 1)
            {
                var header = ZLinkEnvelopeCodec.DecodeHeader(
                    replyParts,
                    runtime.Flow.CaptureEnabled);
                reply = ZLinkMessage.FromEnvelopePayload(
                    header.ContentType,
                    replyParts[1],
                    runtime.Registration.Codecs);
            }
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
        return completion.Result switch
        {
            ActorCreateResult.Existing =>
                new ZLinkActorCreateResult.Existing(completion.Actor),
            ActorCreateResult.Created =>
                new ZLinkActorCreateResult.Created(completion.Actor, reply),
            _ => new ZLinkActorCreateResult.Rejected(reply)
        };
    }


    //  A join that observes an existing authority it can neither join (dead
    //  owner) nor reclaim (recovery-required fence) previously spun through
    //  reserve->join->null with no log and no backoff until the caller's
    //  deadline. Trace and back off so the cycle is visible and cheap.
    private static async ValueTask DelayJoinRetryAsync(
        string actorId,
        int attempt,
        CancellationToken cancellationToken)
    {
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"actor_join_retry actor={actorId} attempt={attempt}");
        var backoffMilliseconds = 1 << Math.Min(attempt, 8);
        await Task.Delay(
                TimeSpan.FromMilliseconds(backoffMilliseconds),
                cancellationToken)
            .ConfigureAwait(false);
    }
}

internal abstract class ZLinkActorCreateCallBase
{
    private readonly Func<string?, ZLinkMessage, TimeSpan, CancellationToken,
        ValueTask<ZLinkActorCreateResult>> _submit;
    private string? _meshName;
    private ZLinkMessage _request = ZLinkMessage.Empty;
    private TimeSpan? _timeout;
    private bool _requestSet;
    private int _submitted;

    protected ZLinkActorCreateCallBase(
        Func<string?, ZLinkMessage, TimeSpan, CancellationToken,
            ValueTask<ZLinkActorCreateResult>> submit)
    {
        _submit = submit;
    }

    protected void SetMesh(string meshName)
    {
        if (_meshName is not null) Duplicate("InMesh");
        if (string.IsNullOrWhiteSpace(meshName))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "MeshName is required.");
        _meshName = meshName;
    }

    protected void SetRequest(ZLinkMessage request)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (_requestSet) Duplicate("Request");
        _request = request;
        _requestSet = true;
    }

    protected void SetTimeout(TimeSpan timeout)
    {
        if (_timeout is not null) Duplicate("Timeout");
        if (timeout <= TimeSpan.Zero)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Timeout must be positive.");
        _timeout = timeout;
    }

    protected ValueTask<ZLinkActorCreateResult> SubmitAsync(
        TimeSpan defaultTimeout,
        CancellationToken cancellationToken)
    {
        if (Interlocked.Exchange(ref _submitted, 1) != 0)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "The Actor create call was already submitted.");
        return _submit(_meshName, _request, _timeout ?? defaultTimeout, cancellationToken);
    }

    private static void Duplicate(string option) =>
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InvalidOperation,
            $"{option} was already configured.");
}

internal sealed class ZLinkActorCreateCall(
    TimeSpan defaultTimeout,
    Func<string?, ZLinkMessage, TimeSpan, CancellationToken,
        ValueTask<ZLinkActorCreateResult>> submit)
    : ZLinkActorCreateCallBase(submit), IZLinkActorCreateCall
{
    private readonly ZLinkSerialTurn? _turn = ZLinkSerialTurn.Current;

    public IZLinkActorCreateCall InMesh(string meshName) { SetMesh(meshName); return this; }
    public IZLinkActorCreateCall Request(ZLinkMessage request) { SetRequest(request); return this; }
    public IZLinkActorCreateCall Request<TRequest>(TRequest request)
    {
        SetRequest(ZLinkMessage.From(request));
        return this;
    }
    public IZLinkActorCreateCall Timeout(TimeSpan timeout) { SetTimeout(timeout); return this; }
    public ValueTask<ZLinkActorCreateResult> Async(CancellationToken cancellationToken = default) =>
        SubmitAsync(defaultTimeout, cancellationToken);

    public ValueTask<ZLinkActorCreateResult> Yield(
        CancellationToken cancellationToken = default) =>
        ZLinkApplicationExecutionContext
            .RequireYieldTurn(_turn, "Actor creation")
            .YieldFrameworkCallAsync(
                token => SubmitAsync(defaultTimeout, token),
                cancellationToken);
}

internal sealed class ZLinkActorGetOrCreateCall(
    TimeSpan defaultTimeout,
    Func<string?, ZLinkMessage, TimeSpan, CancellationToken,
        ValueTask<ZLinkActorCreateResult>> submit)
    : ZLinkActorCreateCallBase(submit), IZLinkActorGetOrCreateCall
{
    private readonly ZLinkSerialTurn? _turn = ZLinkSerialTurn.Current;

    public IZLinkActorGetOrCreateCall InMesh(string meshName) { SetMesh(meshName); return this; }
    public IZLinkActorGetOrCreateCall Request(ZLinkMessage request) { SetRequest(request); return this; }
    public IZLinkActorGetOrCreateCall Request<TRequest>(TRequest request)
    {
        SetRequest(ZLinkMessage.From(request));
        return this;
    }
    public IZLinkActorGetOrCreateCall Timeout(TimeSpan timeout) { SetTimeout(timeout); return this; }
    public ValueTask<ZLinkActorCreateResult> Async(CancellationToken cancellationToken = default) =>
        SubmitAsync(defaultTimeout, cancellationToken);

    public ValueTask<ZLinkActorCreateResult> Yield(
        CancellationToken cancellationToken = default) =>
        ZLinkApplicationExecutionContext
            .RequireYieldTurn(_turn, "Actor get-or-create")
            .YieldFrameworkCallAsync(
                token => SubmitAsync(defaultTimeout, token),
                cancellationToken);
}
