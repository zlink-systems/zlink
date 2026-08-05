using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotRuntimeManager(
    IServiceProvider services,
    ZLinkFrameworkRuntime runtime,
    IZLinkBackendAdapterFactory backendAdapterFactory,
    ZLinkFrameworkRegistration registration,
    ZLinkLocationLifecycle? locationLifecycle)
{
    private readonly ZLinkFrameworkRegistration _frameworkRegistration = registration;
    private readonly IZLinkLocationRepository? _locationStore =
        registration.Locations.ResolveStore();
    private readonly IZLinkMeshNodeLocationResolver? _locationResolver =
        services.GetService<IZLinkMeshNodeLocationResolver>()
        ?? services.GetService<ZLinkStoreLocationResolvers>();
    private readonly ZLinkEntrySpotActorRouter _entrySpotActors = new(runtime);
    private long _nextPlacementSelection;

    private readonly ZLinkSpotNodeInitializer _nodeInitializer = new(
        services,
        runtime,
        backendAdapterFactory,
        registration,
        locationLifecycle);

    public ZLinkEntrySpotActorRouter EntrySpotActors => _entrySpotActors;

    public async ValueTask InitializeSpotNodesAsync(ZLinkFrameworkComponentState state)
    {
        await _nodeInitializer.InitializeAsync(state).ConfigureAwait(false);
    }

    public ZLinkSpotPublisherBundle GetPublisherBundle(
        ZLinkFrameworkComponentState state,
        string channelName)
    {
        if (state.SpotNodes.TryGetValue(channelName, out var node))
            return node.GetOrCreatePublisherBundle(channelName);

        throw new ZLinkConfigurationException(
            $"SPOT publisher mesh '{channelName}' is not registered.");
    }

    public async ValueTask<ZLinkSpotCreateResult> CreateAsync(
        ZLinkFrameworkComponentState state,
        Type spotType,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        var node = GetNodeForSpotFactory(state, spotType);
        var relocation = node.Registration.SpotRelocations
            .SingleOrDefault(pair => pair.Value.InstanceType == spotType);
        if (!string.IsNullOrEmpty(relocation.Key)
            && _locationStore is not null)
        {
            return await TryRemoteCreateAsync(
                    node,
                    relocation.Key,
                    Guid.NewGuid().ToString("D"),
                    request,
                    _frameworkRegistration.DefaultRequestTimeout,
                    false,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        return await node.CreateAsync(spotType, request, cancellationToken);
    }

    public async ValueTask<ZLinkSpotCreateResult> GetOrCreateAsync(
        ZLinkFrameworkComponentState state,
        Type spotType,
        string spotId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        var node = GetNodeForSpotFactory(state, spotType);
        var relocation = node.Registration.SpotRelocations
            .SingleOrDefault(pair => pair.Value.InstanceType == spotType);
        if (!string.IsNullOrEmpty(relocation.Key)
            && _locationStore is not null)
        {
            return await TryRemoteCreateAsync(
                    node,
                    relocation.Key,
                    requestedSpotId: spotId,
                    request,
                    _frameworkRegistration.DefaultRequestTimeout,
                    true,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        return await node.GetOrCreateAsync(
            spotType,
            spotId,
            request,
            cancellationToken);
    }

    internal async ValueTask<ZLinkSpotCreateResult> CreateByStableTypeAsync(
        ZLinkFrameworkComponentState state,
        string stableType,
        string requestedSpotId,
        string? meshName,
        ZLinkMessage request,
        TimeSpan timeout,
        bool joinExisting,
        CancellationToken cancellationToken)
    {
        var candidates = state.SpotNodes.Values
            .Where(node => node.Registration.ObjectRoleSelected
                && (meshName is null || string.Equals(
                    node.Registration.SpotNodeName,
                    meshName,
                    StringComparison.Ordinal)))
            .ToArray();
        if (candidates.Length == 0)
            throw new ZLinkFrameworkException(
                meshName is null
                    ? ZLinkFrameworkErrorKind.NotConfigured
                    : ZLinkFrameworkErrorKind.NotFound,
                meshName is null
                    ? "No object client MeshNode is registered."
                    : $"Object MeshNode '{meshName}' is not registered.");
        if (meshName is null && candidates.Length > 1)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "More than one object client MeshNode is registered.");
        return await TryRemoteCreateAsync(
                candidates[0],
                stableType,
                requestedSpotId,
                request,
                timeout,
                joinExisting,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkSpotCreateResult> TryRemoteCreateAsync(
        ZLinkSpotNodeRuntime source,
        string stableType,
        string requestedSpotId,
        ZLinkMessage request,
        TimeSpan timeout,
        bool joinExisting,
        CancellationToken cancellationToken)
    {
        var locationStore = _locationStore
            ?? throw new InvalidOperationException(
                "Remote User Spot creation requires a Location Store.");
        var deadlineAt = DateTimeOffset.UtcNow.Add(timeout);
        using var deadlineToken = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken);
        deadlineToken.CancelAfter(timeout);
        var meshName = source.Registration.SpotNodeName;
        var descriptors = await ListLiveMeshNodesAsync(
                meshName, deadlineToken.Token)
            .ConfigureAwait(false);
        var placementEligible = descriptors
            .Where(candidate => IsEligibleCandidate(candidate, stableType))
            .OrderBy(static candidate => candidate.Rid.ToHex(), StringComparer.Ordinal)
            .ToList();
        var eligible = FilterRouteReadyCandidates(source, placementEligible);
        var encoded = request.Encode(_frameworkRegistration.Codecs);
        var applicationPayload = ZLinkApplicationPayloadEnvelopeCodec.Encode(
            ZLinkApplicationPayloadEnvelopeCodec.CreationPacketName,
            encoded.ContentType,
            encoded.Payload.Bytes.Span);
        var creationIntentReference =
            ZLinkInlineCreationIntentCodec.Encode(applicationPayload);
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(requestedSpotId);
        var applicationPayloadHash =
            System.Security.Cryptography.SHA256.HashData(applicationPayload);
        var reservationRefreshAttempt = 0;
        ZLinkMeshNodeDescriptor target;
        ZLinkLocationOwnerToken owner;
        ZLinkObjectReservation snapshot;
        while (true)
        {
            var selectedTarget = ZLinkWeightedSelector.Select(
                    eligible,
                    static candidate => candidate.PlacementWeight,
                    ref _nextPlacementSelection);
            if (selectedTarget is null
                && placementEligible.Count == 0
                && reservationRefreshAttempt == 0)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.CapacityExceeded,
                    $"No Ready User Spot target is available for '{stableType}'.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            if (selectedTarget is null)
            {
                var backoffMilliseconds =
                    1 << Math.Min(reservationRefreshAttempt++, 6);
                await Task.Delay(
                        TimeSpan.FromMilliseconds(backoffMilliseconds),
                        deadlineToken.Token)
                    .ConfigureAwait(false);
                descriptors = await ListLiveMeshNodesAsync(
                        meshName,
                        deadlineToken.Token)
                    .ConfigureAwait(false);
                placementEligible = descriptors
                    .Where(candidate => IsEligibleCandidate(candidate, stableType))
                    .OrderBy(
                        static candidate => candidate.Rid.ToHex(),
                        StringComparer.Ordinal)
                    .ToList();
                eligible = FilterRouteReadyCandidates(source, placementEligible);
                continue;
            }
            target = selectedTarget;
            owner = new ZLinkLocationOwnerToken(
                target.OwnerId, target.LeaseGeneration);
            var creating = ZLinkUserSpotAuthorityPayloadCodec.Encode(
                new ZLinkUserSpotAuthorityPayload(
                    ZLinkUserSpotAuthorityState.Creating,
                    stableType,
                    requestedSpotId,
                    owner.OwnerId,
                    checked((ulong)owner.LeaseGeneration),
                    meshName,
                    target.Rid,
                    target.LifecycleGeneration));
            var reserved = await locationStore.ReserveAsync(
                    new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.UserSpot,
                        key,
                        stableType,
                        creationIntentReference,
                        applicationPayloadHash,
                        applicationPayload.Length,
                        new ZLinkMeshNodeDescriptorKey(meshName, target.Rid),
                        target.LifecycleGeneration,
                        owner,
                        creating,
                        new ZLinkCapacityVector(
                            0,
                            1,
                            new ZLinkSpotTypeCapacityDelta(
                                ZLinkPlacementObjectKind.UserSpot,
                                stableType,
                                1))),
                    deadlineToken.Token)
                .ConfigureAwait(false);
            if (reserved is ZLinkObjectReserveResult.Reserved reservation)
            {
                snapshot = reservation.Reservation;
                break;
            }
            if (reserved is ZLinkObjectReserveResult.PlacementCapacityExhausted)
            {
                eligible.Remove(target);
                continue;
            }
            if (reserved is ZLinkObjectReserveResult.TypeMismatch)
                throw SpotTypeMismatch(requestedSpotId, stableType);
            if (reserved is ZLinkObjectReserveResult.AlreadyExists existing)
            {
                if (!joinExisting)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.AlreadyExists,
                        $"User Spot '{requestedSpotId}' already exists.");
                var joined = await JoinExistingAsync(
                        requestedSpotId,
                        stableType,
                        existing.Current,
                        deadlineAt,
                        deadlineToken.Token)
                    .ConfigureAwait(false);
                if (joined.HasValue)
                    return joined.Value;
                continue;
            }
            if (joinExisting
                && reserved is ZLinkObjectReserveResult.Conflict(
                    ZLinkAuthorityReadResult.Found found))
            {
                var joined = await JoinExistingAsync(
                        requestedSpotId,
                        stableType,
                        found.Snapshot,
                        deadlineAt,
                        deadlineToken.Token)
                    .ConfigureAwait(false);
                if (joined.HasValue)
                    return joined.Value;
                continue;
            }
            if (reserved is ZLinkObjectReserveResult.Conflict(
                ZLinkAuthorityReadResult.Missing))
            {
                var backoffMilliseconds =
                    1 << Math.Min(reservationRefreshAttempt++, 6);
                await Task.Delay(
                        TimeSpan.FromMilliseconds(backoffMilliseconds),
                        deadlineToken.Token)
                    .ConfigureAwait(false);
                descriptors = await ListLiveMeshNodesAsync(
                        meshName,
                        deadlineToken.Token)
                    .ConfigureAwait(false);
                placementEligible = descriptors
                    .Where(candidate => IsEligibleCandidate(candidate, stableType))
                    .OrderBy(
                        static candidate => candidate.Rid.ToHex(),
                        StringComparer.Ordinal)
                    .ToList();
                eligible = FilterRouteReadyCandidates(source, placementEligible);
                continue;
            }
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"User Spot '{requestedSpotId}' reservation changed.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
        }
        var deadline = checked((ulong)deadlineAt.ToUnixTimeMilliseconds());
        var fence = new ObjectReservationFence(
            snapshot.ReservationVersion,
            snapshot.StoreVersion,
            snapshot.ObjectGeneration,
            snapshot.AuthorityOwnerGeneration,
            target.Rid,
            target.LifecycleGeneration,
            owner.OwnerId,
            checked((ulong)owner.LeaseGeneration),
            1);
        (UserSpotCreateCompletion Completion, IReadOnlyList<Message> Reply) result;
        try
        {
            if (target.Rid == source.Node.RoutingId)
            {
                result = await source.CreateUserSpotLocalAsync(
                        requestedSpotId,
                        stableType,
                        fence,
                        deadline,
                        deadlineToken.Token)
                    .ConfigureAwait(false);
            }
            else
            {
                result = await CreateRemoteAfterAdmissionAsync()
                    .ConfigureAwait(false);
            }

            async ValueTask<(
                UserSpotCreateCompletion Completion,
                IReadOnlyList<Message> Reply)> CreateRemoteAfterAdmissionAsync()
            {
                while (true)
                {
                    var remaining = deadlineAt - DateTimeOffset.UtcNow;
                    if (remaining <= TimeSpan.Zero)
                        throw new TimeoutException(
                            "The User Spot create deadline elapsed.");
                    try
                    {
                        return await source.Node.CreateUserSpotAsync(
                                target.Rid,
                                requestedSpotId,
                                stableType,
                                fence,
                                deadline,
                                remaining,
                                deadlineToken.Token)
                            .ConfigureAwait(false);
                    }
                    catch (ZlinkSubmitException error)
                        when (error.Result is
                                  ZlinkSubmitException.ErrorCode.NotConnected
                              or ZlinkSubmitException.ErrorCode.Backpressured)
                    {
                        // The reservation already fixes the exact target and
                        // generation. Retry only source-local admission; do
                        // not select another owner or create another claim.
                        await Task.Delay(
                                TimeSpan.FromMilliseconds(2),
                                deadlineToken.Token)
                            .ConfigureAwait(false);
                    }
                }
            }
        }
        catch
        {
            await locationStore.AbortAsync(snapshot, CancellationToken.None)
                .ConfigureAwait(false);
            throw;
        }
        ZLinkMessage? reply = null;
        try
        {
            if (result.Reply.Count > 1)
            {
                var header = ZLinkEnvelopeCodec.DecodeHeader(result.Reply);
                reply = ZLinkMessage.FromEnvelopePayload(
                    header.ContentType,
                    result.Reply[1],
                    _frameworkRegistration.Codecs);
            }
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(result.Reply);
        }
        return new ZLinkSpotCreateResult(
            new SpotRef(
                result.Completion.SpotId,
                result.Completion.ObjectGeneration,
                meshName,
                target.Rid),
            result.Completion.Result switch
            {
                UserSpotCreateResult.Existing => ZLinkSpotCreateState.Existing,
                UserSpotCreateResult.Created => ZLinkSpotCreateState.Created,
                _ => ZLinkSpotCreateState.Rejected
            },
            reply);
    }

    internal static bool IsEligibleCandidate(
        ZLinkMeshNodeDescriptor candidate,
        string stableType) =>
        candidate.State == ZLinkFrameworkRuntimeState.Serving
        && candidate.ObjectRole == ZLinkMeshNodeObjectRole.Server
        && candidate.PlacementWeight > 0
        && (candidate.Capacity.Spots.Limit == 0
            || candidate.Capacity.Spots.Active
            + (long)candidate.Capacity.Spots.Reserved
            < candidate.Capacity.Spots.Limit)
        && candidate.ObjectCapabilities.Any(capability =>
            capability.ObjectKind == ZLinkPlacementObjectKind.UserSpot
            && string.Equals(
                capability.StableType,
                stableType,
                StringComparison.Ordinal))
        && candidate.Capacity.SpotTypes.Any(capacity =>
            capacity.ObjectKind == ZLinkPlacementObjectKind.UserSpot
            && string.Equals(
                capacity.StableType,
                stableType,
                StringComparison.Ordinal)
            && (capacity.Limit == 0
                || capacity.Active + (long)capacity.Reserved
                < capacity.Limit));

    private static List<ZLinkMeshNodeDescriptor> FilterRouteReadyCandidates(
        ZLinkSpotNodeRuntime source,
        IReadOnlyList<ZLinkMeshNodeDescriptor> candidates) =>
        ZLinkMeshNodeTargetAvailability.FilterAdmitted(
                source.Node.RoutingId,
                candidates,
                source.Node.MeshPeers())
            .ToList();

    private ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
        string meshName,
        CancellationToken cancellationToken)
    {
        if (_locationResolver is null)
            throw new ZLinkConfigurationException(
                "Remote User Spot creation requires the live MeshNode resolver.");
        return _locationResolver.ListLiveMeshNodesAsync(
            meshName,
            cancellationToken);
    }

    private async ValueTask<ZLinkSpotCreateResult?> JoinExistingAsync(
        string spotId,
        string stableType,
        ZLinkAuthoritySnapshot current,
        DateTimeOffset deadline,
        CancellationToken cancellationToken)
    {
        while (true)
        {
            if (current.Allocation.ObjectKind != ZLinkPlacementObjectKind.UserSpot
                || !string.Equals(
                    current.Allocation.StableType,
                    stableType,
                    StringComparison.Ordinal))
                throw SpotTypeMismatch(spotId, stableType);
            if (ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    current.Payload.Span,
                    out var relocation))
            {
                if (!ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                        relocation.SteadyAuthorityPayload.Span,
                        out var steady)
                    || !string.Equals(
                        steady.SpotId,
                        spotId,
                        StringComparison.Ordinal)
                    || !string.Equals(
                        steady.StableType,
                        stableType,
                        StringComparison.Ordinal))
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        $"User Spot '{spotId}' relocation authority is invalid.",
                        ZLinkRetryAdvice.RetryAfterBackoff);
                if (current.Allocation.State
                        == ZLinkPlacementAllocationState.Active
                    && relocation.Phase >= 4
                    && !string.IsNullOrWhiteSpace(
                        relocation.State.TargetNodeRid))
                    return new ZLinkSpotCreateResult(
                        new SpotRef(
                            spotId,
                            current.ObjectGeneration,
                            steady.MeshName,
                            RoutingId.FromHex(
                                relocation.State.TargetNodeRid)),
                        ZLinkSpotCreateState.Existing,
                        null);
                if (DateTimeOffset.UtcNow >= deadline)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DeadlineExceeded,
                        $"Timed out while joining relocating User Spot '{spotId}'.",
                        ZLinkRetryAdvice.RetryAfterBackoff);
                await Task.Delay(
                        TimeSpan.FromMilliseconds(10),
                        cancellationToken)
                    .ConfigureAwait(false);
                var relocatingRead = await _locationStore!.ReadAuthorityAsync(
                        ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId),
                        cancellationToken)
                    .ConfigureAwait(false);
                if (relocatingRead is not ZLinkAuthorityReadResult.Found
                    relocatingFound)
                    return null;
                current = relocatingFound.Snapshot;
                continue;
            }
            if (!ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                    current.Payload.Span, out var authority))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"User Spot '{spotId}' authority is invalid.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            if (current.Allocation.State == ZLinkPlacementAllocationState.Active
                && authority.State == ZLinkUserSpotAuthorityState.Ready)
                return new ZLinkSpotCreateResult(
                    new SpotRef(
                        spotId,
                        current.ObjectGeneration,
                        authority.MeshName,
                        authority.NodeRid),
                    ZLinkSpotCreateState.Existing,
                    null);
            if (current.Allocation.State != ZLinkPlacementAllocationState.Reserved
                || authority.State != ZLinkUserSpotAuthorityState.Creating)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"User Spot '{spotId}' creation changed.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            if (DateTimeOffset.UtcNow >= deadline)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    $"Timed out while joining User Spot '{spotId}' creation.",
                    ZLinkRetryAdvice.RetryAfterBackoff);

            await Task.Delay(
                    TimeSpan.FromMilliseconds(10),
                    cancellationToken)
                .ConfigureAwait(false);
            var read = await _locationStore!.ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId),
                    cancellationToken)
                .ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                return null;
            current = found.Snapshot;
        }
    }

    private static ZLinkFrameworkException SpotTypeMismatch(
        string spotId,
        string stableType) =>
        new(
            ZLinkFrameworkErrorKind.TypeMismatch,
            $"User Spot '{spotId}' is not registered as '{stableType}'.");

    public async ValueTask<ZLinkSpotInfo?> GetAsync(
        ZLinkFrameworkComponentState state,
        string spotId,
        CancellationToken cancellationToken)
    {
        foreach (var node in state.SpotNodes.Values)
        {
            var info = await node.GetAsync(spotId, cancellationToken);
            if (info is not null) return info;
        }

        return null;
    }

    public async ValueTask<SpotRef?> ResolveAsync(
        string spotId,
        CancellationToken cancellationToken)
    {
        if (_locationStore is null) return null;
        var read = await _locationStore.ReadAuthorityAsync(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId),
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found
            || found.Snapshot.Allocation.State
                != ZLinkPlacementAllocationState.Active
            || found.Snapshot.Allocation.ObjectKind
                != ZLinkPlacementObjectKind.UserSpot
            || !ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span, out var authority)
            || authority.State != ZLinkUserSpotAuthorityState.Ready)
            return null;
        return new SpotRef(
            spotId,
            found.Snapshot.ObjectGeneration,
            authority.MeshName,
            authority.NodeRid);
    }

    public async ValueTask<IReadOnlyList<ZLinkSpotInfo>> ListAsync(
        ZLinkFrameworkComponentState state,
        CancellationToken cancellationToken)
    {
        var results = new List<ZLinkSpotInfo>();
        foreach (var node in state.SpotNodes.Values) results.AddRange(await node.ListAsync(cancellationToken));

        return results
            .OrderBy(static info => info.SpotId, StringComparer.Ordinal)
            .ToArray();
    }

    public async ValueTask<bool> CloseAsync(
        ZLinkFrameworkComponentState state,
        SpotRef spot,
        CancellationToken cancellationToken)
    {
        var spotId = spot.SpotId;
        var deadlineAt = DateTimeOffset.UtcNow
            .Add(_frameworkRegistration.DefaultRequestTimeout);
        if (_locationStore is not null)
        {
            var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
            while (true)
            {
                var read = await _locationStore.ReadAuthorityAsync(
                        key,
                        cancellationToken)
                    .ConfigureAwait(false);
                if (read is not ZLinkAuthorityReadResult.Found found)
                    break;
                if (found.Snapshot.ObjectGeneration != spot.ObjectGeneration)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.InvalidOperation,
                        $"User Spot '{spotId}' generation is stale.");
                if (ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                        found.Snapshot.Payload.Span,
                        out var authority)
                    && found.Snapshot.Allocation.State
                    == ZLinkPlacementAllocationState.Active
                    && found.Snapshot.Allocation.ObjectKind
                    == ZLinkPlacementObjectKind.UserSpot
                    && authority.State == ZLinkUserSpotAuthorityState.Ready)
                {
                    if (!string.Equals(
                            authority.MeshName,
                            spot.MeshName,
                            StringComparison.Ordinal))
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Unavailable,
                            $"User Spot '{spotId}' mesh changed.",
                            ZLinkRetryAdvice.RetryAfterBackoff);
                    var source = state.SpotNodes.Values.FirstOrDefault(node =>
                        string.Equals(
                            node.Registration.SpotNodeName,
                            authority.MeshName,
                            StringComparison.Ordinal));
                    if (source is not null
                        && authority.NodeRid != source.Node.RoutingId)
                    {
                        var remaining = deadlineAt - DateTimeOffset.UtcNow;
                        if (remaining <= TimeSpan.Zero)
                            throw CloseDeadlineElapsed(spotId);
                        try
                        {
                            var closed = await source.Node.CloseUserSpotAsync(
                                    authority.NodeRid,
                                    new UserSpotCloseFence(
                                        spotId,
                                        found.Snapshot.ObjectGeneration,
                                        authority.NodeRid,
                                        authority.NodeGeneration,
                                        found.Snapshot.AuthorityOwnerGeneration,
                                        found.Snapshot.StoreVersion),
                                    checked((ulong)deadlineAt
                                        .ToUnixTimeMilliseconds()),
                                    remaining,
                                    cancellationToken)
                                .ConfigureAwait(false);
                            return closed.Closed;
                        }
                        catch (ZLinkFrameworkException exception)
                            when (exception.Kind
                                  == ZLinkFrameworkErrorKind.Unavailable
                                  && exception.RetryAdvice
                                  == ZLinkRetryAdvice.RetryAfterBackoff)
                        {
                            // The owner can be between publication and local
                            // materialization while relocation completes.
                        }
                    }
                    else
                    {
                        foreach (var node in state.SpotNodes.Values)
                        {
                            if (await node.CloseAsync(spotId, cancellationToken)
                                .ConfigureAwait(false))
                                return true;
                            if (await node.GetAsync(spotId, cancellationToken)
                                .ConfigureAwait(false) is not null)
                                return false;
                        }
                    }
                }

                if (DateTimeOffset.UtcNow >= deadlineAt)
                    throw CloseDeadlineElapsed(spotId);
                await Task.Delay(
                        TimeSpan.FromMilliseconds(10),
                        cancellationToken)
                    .ConfigureAwait(false);
            }
        }
        foreach (var node in state.SpotNodes.Values)
            if (await node.CloseAsync(spotId, cancellationToken)
                    .ConfigureAwait(false))
                return true;

        return false;
    }

    private static ZLinkFrameworkException CloseDeadlineElapsed(
        string spotId) =>
        new(
            ZLinkFrameworkErrorKind.DeadlineExceeded,
            $"Timed out while closing User Spot '{spotId}'.");

    internal ValueTask<bool> CloseLocalByIdAsync(
        ZLinkFrameworkComponentState state,
        string spotId,
        CancellationToken cancellationToken) =>
        CloseLocalAsync(state, spotId, cancellationToken);

    private static async ValueTask<bool> CloseLocalAsync(
        ZLinkFrameworkComponentState state,
        string spotId,
        CancellationToken cancellationToken)
    {
        foreach (var node in state.SpotNodes.Values)
            if (await node.CloseAsync(spotId, cancellationToken))
                return true;
        return false;
    }

    public async ValueTask<ZLinkSpotActorJoinResult> JoinActorAsync(
        ZLinkFrameworkComponentState state,
        string spotId,
        IZLinkActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken,
        DateTimeOffset? absoluteDeadline = null)
    {
        var activation = GetActivationBySpotId(state, spotId)
                         ?? throw new InvalidOperationException($"SPOT '{spotId}' is not active.");

        return await activation.JoinActorAsync(
            actor,
            request,
            cancellationToken,
            absoluteDeadline);
    }

    public async ValueTask<bool> TryNotifyJoinedSpotActorDisconnectedAsync(
        ZLinkFrameworkComponentState state,
        string actorId,
        CancellationToken cancellationToken)
    {
        foreach (var activation in state.SpotNodes.Values.SelectMany(static node => node.Spots))
        {
            if (!activation.TryGetJoinedActor(actorId, out var actor) || actor is null) continue;

            await activation.NotifyActorDisconnectedAsync(actor, cancellationToken)
                .ConfigureAwait(false);
            return true;
        }

        return false;
    }

    public ZLinkSpotMonitoringSnapshot GetMonitoringSnapshot(
        ZLinkFrameworkComponentState state,
        string spotNodeName)
    {
        return GetNode(state, spotNodeName).GetMonitoringSnapshot();
    }

    private static ZLinkSpotNodeRuntime GetNode(
        ZLinkFrameworkComponentState state,
        string spotNodeName)
    {
        return state.SpotNodes.TryGetValue(spotNodeName, out var node)
            ? node
            : throw new ZLinkConfigurationException($"SPOT node '{spotNodeName}' is not registered.");
    }

    private static ZLinkSpotNodeRuntime GetNodeForSpotFactory(
        ZLinkFrameworkComponentState state,
        Type spotType)
    {
        foreach (var node in state.SpotNodes.Values)
            if (node.SpotFactories.Contains(spotType))
                return node;

        throw new ZLinkConfigurationException($"SPOT factory '{spotType}' is not registered.");
    }

    public ZLinkSpotActivation? GetActivationBySpotId(
        ZLinkFrameworkComponentState state,
        string spotId)
    {
        foreach (var node in state.SpotNodes.Values)
        {
            var activation = node.Spots.FirstOrDefault(current => current.SpotId == spotId);
            if (activation is not null) return activation;
        }

        return null;
    }
}

internal readonly record struct EntrySpotActorReplyDispatchResult(
    bool Handled,
    ZLinkActorReply? Reply);
