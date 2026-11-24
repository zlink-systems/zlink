using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotRuntimeManager(
    IServiceProvider services,
    ZLinkFrameworkRuntime runtime,
    ZLinkFrameworkRegistration registration,
    ZLinkLocationLifecycle? locationLifecycle,
    ZLinkOwnerLeaseTracker? leaseTracker,
    ZLinkDispatchErrorReporter dispatchErrors
)
{
    private static readonly TimeSpan LocationConvergencePollInterval = TimeSpan.FromMilliseconds(
        10
    );
    private readonly ZLinkFrameworkRegistration _frameworkRegistration = registration;
    private readonly IZLinkLocationRepository? _locationStore =
        registration.Locations.ResolveStore();
    private readonly ZLinkOwnerLeaseTracker? _leaseTracker = leaseTracker;
    private readonly ZLinkEntrySpotActorRouter _entrySpotActors = new(runtime);

    private readonly ZLinkSpotNodeInitializer _nodeInitializer = new(
        services,
        runtime,
        registration,
        locationLifecycle,
        leaseTracker,
        dispatchErrors
    );

    public ZLinkEntrySpotActorRouter EntrySpotActors => _entrySpotActors;

    public async ValueTask InitializeSpotNodesAsync(ZLinkFrameworkComponentState state)
    {
        await _nodeInitializer.InitializeAsync(state).ConfigureAwait(false);
    }

    public ZLinkSpotPublisherBundle GetPublisherBundle(
        ZLinkFrameworkComponentState state,
        string channelName
    )
    {
        if (state.SpotNodes.TryGetValue(channelName, out var node))
            return node.GetOrCreatePublisherBundle(channelName);

        throw new ZLinkConfigurationException(
            $"SPOT publisher mesh '{channelName}' is not registered."
        );
    }

    public async ValueTask<ZLinkSpotCreateResult> CreateAsync(
        ZLinkFrameworkComponentState state,
        Type spotType,
        ZLinkMessage request,
        CancellationToken cancellationToken
    )
    {
        var node = GetNodeForSpotFactory(state, spotType);
        var relocation = node.Registration.SpotRelocations.SingleOrDefault(pair =>
            pair.Value.InstanceType == spotType
        );
        if (!string.IsNullOrEmpty(relocation.Key) && _locationStore is not null)
        {
            return await TryRemoteCreateAsync(
                    node,
                    relocation.Key,
                    Guid.NewGuid().ToString("D"),
                    request,
                    _frameworkRegistration.DefaultRequestTimeout,
                    false,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        return await node.CreateAsync(spotType, request, cancellationToken);
    }

    public async ValueTask<ZLinkSpotCreateResult> GetOrCreateAsync(
        ZLinkFrameworkComponentState state,
        Type spotType,
        string spotId,
        ZLinkMessage request,
        CancellationToken cancellationToken
    )
    {
        var node = GetNodeForSpotFactory(state, spotType);
        var relocation = node.Registration.SpotRelocations.SingleOrDefault(pair =>
            pair.Value.InstanceType == spotType
        );
        if (!string.IsNullOrEmpty(relocation.Key) && _locationStore is not null)
        {
            return await TryRemoteCreateAsync(
                    node,
                    relocation.Key,
                    requestedSpotId: spotId,
                    request,
                    _frameworkRegistration.DefaultRequestTimeout,
                    true,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        return await node.GetOrCreateAsync(spotType, spotId, request, cancellationToken);
    }

    internal async ValueTask<ZLinkSpotCreateResult> CreateByStableTypeAsync(
        ZLinkFrameworkComponentState state,
        string stableType,
        string requestedSpotId,
        string? meshName,
        ZLinkMessage request,
        TimeSpan timeout,
        bool joinExisting,
        CancellationToken cancellationToken
    )
    {
        var candidates = state
            .SpotNodes.Values.Where(node =>
                node.Registration.ObjectRoleSelected
                && (
                    meshName is null
                    || string.Equals(
                        node.Registration.SpotNodeName,
                        meshName,
                        StringComparison.Ordinal
                    )
                )
            )
            .ToArray();
        if (candidates.Length == 0)
            throw new ZLinkFrameworkException(
                meshName is null
                    ? ZLinkFrameworkErrorKind.NotConfigured
                    : ZLinkFrameworkErrorKind.NotFound,
                meshName is null
                    ? "No object client MeshNode is registered."
                    : $"Object MeshNode '{meshName}' is not registered."
            );
        if (meshName is null && candidates.Length > 1)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "More than one object client MeshNode is registered."
            );
        return await TryRemoteCreateAsync(
                candidates[0],
                stableType,
                requestedSpotId,
                request,
                timeout,
                joinExisting,
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkSpotCreateResult> TryRemoteCreateAsync(
        ZLinkSpotNodeRuntime source,
        string stableType,
        string requestedSpotId,
        ZLinkMessage request,
        TimeSpan timeout,
        bool joinExisting,
        CancellationToken cancellationToken
    )
    {
        var locationStore =
            _locationStore
            ?? throw new InvalidOperationException(
                "Remote User Spot creation requires a Location Store."
            );
        var deadlineUnixMs = checked(
            (ulong)DateTimeOffset.UtcNow.Add(timeout).ToUnixTimeMilliseconds()
        );
        var deadlineAt = Stopwatch.GetElapsedTime(0) + timeout;
        using var deadlineToken = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken
        );
        deadlineToken.CancelAfter(timeout);
        try
        {
            var meshName = source.Registration.SpotNodeName;
            var encoded = request.Encode(_frameworkRegistration.Codecs);
            var applicationPayload = ZLinkApplicationPayloadEnvelopeCodec.Encode(
                ZLinkApplicationPayloadEnvelopeCodec.CreationPacketName,
                encoded.ContentType,
                encoded.Payload.Bytes.Span
            );
            var creationIntentReference = ZLinkInlineCreationIntentCodec.Encode(applicationPayload);
            var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(requestedSpotId);
            var applicationPayloadHash = System.Security.Cryptography.SHA256.HashData(
                applicationPayload
            );
            var creationRelocationPolicy = source.Registration.SpotRelocations.TryGetValue(
                stableType,
                out var policy
            )
                ? (byte?)policy.PolicyKind
                : null;
            var resolver =
                services.GetService<ZLinkStoreLocationResolvers>()
                ?? throw new ZLinkConfigurationException(
                    "User Spot creation requires the live MeshNode resolver."
                );
            var excluded = new HashSet<(RoutingId Rid, ulong Generation)>();
            ZLinkMeshNodeDescriptor target;
            ZLinkLocationOwnerToken owner;
            ZLinkObjectReservation snapshot;
            while (true)
            {
                var authority = await locationStore
                    .ReadAuthorityAsync(key, deadlineToken.Token)
                    .ConfigureAwait(false);
                if (authority is ZLinkAuthorityReadResult.Found observed)
                {
                    if (!joinExisting)
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.AlreadyExists,
                            $"User Spot '{requestedSpotId}' already exists."
                        );
                    var joined = await JoinExistingAsync(
                            requestedSpotId,
                            stableType,
                            observed.Snapshot,
                            creationRelocationPolicy,
                            deadlineAt,
                            deadlineToken.Token
                        )
                        .ConfigureAwait(false);
                    if (joined.HasValue)
                        return joined.Value;
                    continue;
                }
                var selectedTarget = await resolver
                    .SelectPlacementAsync(
                        meshName,
                        ZLinkPlacementObjectKind.UserSpot,
                        stableType,
                        candidates =>
                            ZLinkMeshNodeTargetAvailability.FilterAdmitted(
                                source.Node.RoutingId,
                                source.Node.LifecycleGeneration,
                                candidates,
                                source.Node.MeshPeers()
                            ),
                        excluded,
                        deadlineToken.Token
                    )
                    .ConfigureAwait(false);
                if (selectedTarget is null)
                {
                    if (
                        await locationStore
                            .ReadAuthorityAsync(key, deadlineToken.Token)
                            .ConfigureAwait(false) is ZLinkAuthorityReadResult.Found
                    )
                        continue;
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        $"No compatible User Spot target with placement capacity is available for '{stableType}'.",
                        ZLinkRetryAdvice.RetryAfterBackoff
                    );
                }
                target = selectedTarget;
                owner = new ZLinkLocationOwnerToken(target.OwnerId, target.LeaseGeneration);
                var creating = ZLinkUserSpotAuthorityPayloadCodec.Encode(
                    new ZLinkUserSpotAuthorityPayload(
                        ZLinkUserSpotAuthorityState.Creating,
                        stableType,
                        requestedSpotId,
                        owner.OwnerId,
                        checked((ulong)owner.LeaseGeneration),
                        meshName,
                        target.Rid,
                        target.LifecycleGeneration
                    )
                );
                var reserved = await locationStore
                    .ReserveAsync(
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
                                    1
                                )
                            ),
                            creationRelocationPolicy
                        ),
                        deadlineToken.Token
                    )
                    .ConfigureAwait(false);
                if (reserved is ZLinkObjectReserveResult.Reserved reservation)
                {
                    snapshot = reservation.Reservation;
                    break;
                }
                if (reserved is ZLinkObjectReserveResult.TypeMismatch)
                    throw SpotTypeMismatch(requestedSpotId, stableType);
                if (
                    reserved
                    is ZLinkObjectReserveResult.PlacementCapacityExhausted
                        or ZLinkObjectReserveResult.Conflict
                        (ZLinkAuthorityReadResult.Missing)
                )
                {
                    excluded.Add((target.Rid, target.LifecycleGeneration));
                    continue;
                }
                if (
                    reserved
                    is ZLinkObjectReserveResult.AlreadyExists
                        or ZLinkObjectReserveResult.Conflict
                )
                    continue;
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"User Spot '{requestedSpotId}' reservation changed.",
                    ZLinkRetryAdvice.RetryAfterBackoff
                );
            }
            var deadline = deadlineUnixMs;
            var fence = new ObjectReservationFence(
                snapshot.ReservationVersion,
                snapshot.StoreVersion,
                snapshot.ObjectGeneration,
                snapshot.AuthorityOwnerGeneration,
                target.Rid,
                target.LifecycleGeneration,
                owner.OwnerId,
                checked((ulong)owner.LeaseGeneration),
                1
            );
            (UserSpotCreateCompletion Completion, IReadOnlyList<Message> Reply) result;
            try
            {
                if (target.Rid == source.Node.RoutingId)
                {
                    result = await source
                        .CreateUserSpotLocalAsync(
                            requestedSpotId,
                            stableType,
                            fence,
                            deadline,
                            deadlineToken.Token
                        )
                        .ConfigureAwait(false);
                }
                else
                {
                    var remaining = deadlineAt - Stopwatch.GetElapsedTime(0);
                    if (remaining <= TimeSpan.Zero)
                        throw CreateUserSpotCreationDeadlineException(requestedSpotId);
                    result = await source
                        .Node.CreateUserSpotAsync(
                            target.Rid,
                            requestedSpotId,
                            stableType,
                            fence,
                            deadline,
                            remaining,
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                }
            }
            catch
            {
                await locationStore
                    .AbortAsync(snapshot, CancellationToken.None)
                    .ConfigureAwait(false);
                throw;
            }
            ZLinkMessage? reply = null;
            try
            {
                if (result.Reply.Count > 1)
                {
                    var header = ZLinkEnvelopeCodec.DecodeHeader(
                        result.Reply,
                        runtime.Flow.CaptureEnabled
                    );
                    reply = ZLinkMessage.FromEnvelopePayload(
                        header.ContentType,
                        result.Reply[1],
                        _frameworkRegistration.Codecs
                    );
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
                    target.Rid
                ),
                result.Completion.Result switch
                {
                    UserSpotCreateResult.Existing => ZLinkSpotCreateState.Existing,
                    UserSpotCreateResult.Created => ZLinkSpotCreateState.Created,
                    _ => ZLinkSpotCreateState.Rejected,
                },
                reply
            );
        }
        catch (OperationCanceledException error)
            when (!cancellationToken.IsCancellationRequested
                && deadlineToken.IsCancellationRequested
            )
        {
            throw CreateUserSpotCreationDeadlineException(requestedSpotId, error);
        }
    }

    private static ZLinkFrameworkException CreateUserSpotCreationDeadlineException(
        string spotId,
        Exception? innerException = null
    ) =>
        new(
            ZLinkFrameworkErrorKind.DeadlineExceeded,
            $"User Spot '{spotId}' creation deadline elapsed.",
            ZLinkRetryAdvice.RetryAfterBackoff,
            innerException
        );

    internal static bool IsEligibleCandidate(
        ZLinkMeshNodeDescriptor candidate,
        string stableType
    ) =>
        candidate.State == ZLinkFrameworkRuntimeState.Serving
        && ZLinkStoreLocationResolvers.HasPlacementCapacity(
            candidate,
            ZLinkPlacementObjectKind.UserSpot,
            stableType
        );

    private async ValueTask<ZLinkSpotCreateResult?> JoinExistingAsync(
        string spotId,
        string stableType,
        ZLinkAuthoritySnapshot current,
        byte? creationRelocationPolicy,
        TimeSpan deadline,
        CancellationToken cancellationToken
    )
    {
        while (true)
        {
            if (_leaseTracker is null)
                throw new ZLinkConfigurationException(
                    "Remote User Spot creation requires the owner lease tracker."
                );
            if (
                current.Allocation.ObjectKind != ZLinkPlacementObjectKind.UserSpot
                || !string.Equals(
                    current.Allocation.StableType,
                    stableType,
                    StringComparison.Ordinal
                )
            )
                throw SpotTypeMismatch(spotId, stableType);
            if (
                !await _leaseTracker
                    .IsOwnerTokenLiveAsync(
                        new ZLinkLocationOwnerToken(current.OwnerId, current.OwnerLeaseGeneration),
                        cancellationToken
                    )
                    .ConfigureAwait(false)
            )
            {
                var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
                await _locationStore!
                    .ReleaseEndedReservationAsync(
                        key,
                        current.StoreVersion,
                        new ZLinkCreationAuthorityRecovery(creationRelocationPolicy),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                var ownerRead = await _locationStore
                    .ReadAuthorityAsync(key, cancellationToken)
                    .ConfigureAwait(false);
                if (ownerRead is not ZLinkAuthorityReadResult.Found reobserved)
                    return null;
                if (reobserved.Snapshot.StoreVersion != current.StoreVersion)
                {
                    current = reobserved.Snapshot;
                    continue;
                }
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"User Spot '{spotId}' owner lease is not live.",
                    ZLinkRetryAdvice.RetryAfterStateChange
                );
            }
            if (
                ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    current.Payload.Span,
                    out var relocation
                )
            )
            {
                if (
                    !ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                        relocation.SteadyAuthorityPayload.Span,
                        out var steady
                    )
                    || !string.Equals(steady.SpotId, spotId, StringComparison.Ordinal)
                    || !string.Equals(steady.StableType, stableType, StringComparison.Ordinal)
                )
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        $"User Spot '{spotId}' relocation authority is invalid.",
                        ZLinkRetryAdvice.RetryAfterBackoff
                    );
                if (
                    current.Allocation.State == ZLinkPlacementAllocationState.Active
                    && relocation.Phase >= (byte)ServiceWireCodec.RelocationPhase.Committed
                    && !string.IsNullOrWhiteSpace(relocation.State.TargetNodeRid)
                )
                    return new ZLinkSpotCreateResult(
                        new SpotRef(
                            spotId,
                            current.ObjectGeneration,
                            steady.MeshName,
                            RoutingId.FromHex(relocation.State.TargetNodeRid)
                        ),
                        ZLinkSpotCreateState.Existing,
                        null
                    );
                if (Stopwatch.GetElapsedTime(0) >= deadline)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DeadlineExceeded,
                        $"Timed out while joining relocating User Spot '{spotId}'.",
                        ZLinkRetryAdvice.RetryAfterBackoff
                    );
                await Task.Delay(LocationConvergencePollInterval, cancellationToken)
                    .ConfigureAwait(false);
                var relocatingRead = await _locationStore!
                    .ReadAuthorityAsync(
                        ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (relocatingRead is not ZLinkAuthorityReadResult.Found relocatingFound)
                    return null;
                current = relocatingFound.Snapshot;
                continue;
            }
            if (
                !ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                    current.Payload.Span,
                    out var authority
                )
            )
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"User Spot '{spotId}' authority is invalid.",
                    ZLinkRetryAdvice.RetryAfterBackoff
                );
            if (
                current.Allocation.State == ZLinkPlacementAllocationState.Active
                && authority.State == ZLinkUserSpotAuthorityState.Ready
            )
                return new ZLinkSpotCreateResult(
                    new SpotRef(
                        spotId,
                        current.ObjectGeneration,
                        authority.MeshName,
                        authority.NodeRid
                    ),
                    ZLinkSpotCreateState.Existing,
                    null
                );
            if (
                !(
                    current.Allocation.State == ZLinkPlacementAllocationState.Reserved
                    && authority.State == ZLinkUserSpotAuthorityState.Creating
                )
                && !(
                    current.Allocation.State == ZLinkPlacementAllocationState.Active
                    && authority.State == ZLinkUserSpotAuthorityState.Closing
                )
            )
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"User Spot '{spotId}' creation changed.",
                    ZLinkRetryAdvice.RetryAfterBackoff
                );
            if (Stopwatch.GetElapsedTime(0) >= deadline)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    $"Timed out while joining User Spot '{spotId}' creation.",
                    ZLinkRetryAdvice.RetryAfterBackoff
                );

            if (
                current.Allocation.State == ZLinkPlacementAllocationState.Reserved
                && await _locationStore!
                    .ReleaseEndedReservationAsync(
                        ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId),
                        current.StoreVersion,
                        creationRecovery: null,
                        cancellationToken: cancellationToken
                    )
                    .ConfigureAwait(false)
            )
                return null;
            await Task.Delay(LocationConvergencePollInterval, cancellationToken)
                .ConfigureAwait(false);
            var read = await _locationStore!
                .ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                return null;
            current = found.Snapshot;
        }
    }

    private static ZLinkFrameworkException SpotTypeMismatch(string spotId, string stableType) =>
        new(
            ZLinkFrameworkErrorKind.TypeMismatch,
            $"User Spot '{spotId}' is not registered as '{stableType}'."
        );

    public async ValueTask<ZLinkSpotInfo?> GetAsync(
        ZLinkFrameworkComponentState state,
        string spotId,
        CancellationToken cancellationToken
    )
    {
        foreach (var node in state.SpotNodes.Values)
        {
            var info = await node.GetAsync(spotId, cancellationToken);
            if (info is not null)
                return info;
        }

        return null;
    }

    public async ValueTask<SpotRef?> ResolveAsync(
        string spotId,
        CancellationToken cancellationToken
    )
    {
        if (_locationStore is null)
            return null;
        var read = await _locationStore
            .ReadAuthorityAsync(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId),
                cancellationToken
            )
            .ConfigureAwait(false);
        return ZLinkStoreLocationResolvers.ProjectReadyUserSpot(spotId, read);
    }

    public async ValueTask<IReadOnlyList<ZLinkSpotInfo>> ListAsync(
        ZLinkFrameworkComponentState state,
        CancellationToken cancellationToken
    )
    {
        var results = new List<ZLinkSpotInfo>();
        foreach (var node in state.SpotNodes.Values)
            results.AddRange(await node.ListAsync(cancellationToken));

        return results.OrderBy(static info => info.SpotId, StringComparer.Ordinal).ToArray();
    }

    public async ValueTask<bool> CloseAsync(
        ZLinkFrameworkComponentState state,
        SpotRef spot,
        CancellationToken cancellationToken
    )
    {
        var spotId = spot.SpotId;
        var timeout = _frameworkRegistration.DefaultRequestTimeout;
        var deadlineUnixMs = checked(
            (ulong)DateTimeOffset.UtcNow.Add(timeout).ToUnixTimeMilliseconds()
        );
        var deadlineAt = Stopwatch.GetElapsedTime(0) + timeout;
        if (_locationStore is not null)
        {
            var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
            while (true)
            {
                var read = await _locationStore
                    .ReadAuthorityAsync(key, cancellationToken)
                    .ConfigureAwait(false);
                if (read is not ZLinkAuthorityReadResult.Found found)
                    break;
                if (
                    ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                        found.Snapshot.Payload.Span,
                        out var authority
                    )
                    && found.Snapshot.Allocation.State == ZLinkPlacementAllocationState.Active
                    && found.Snapshot.Allocation.ObjectKind == ZLinkPlacementObjectKind.UserSpot
                    // A Closing authority belongs to a Close that failed after
                    // step 1; its owner resumes it for this generation.
                    && authority.State
                        is ZLinkUserSpotAuthorityState.Ready
                            or ZLinkUserSpotAuthorityState.Closing
                )
                {
                    if (!string.Equals(authority.MeshName, spot.MeshName, StringComparison.Ordinal))
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Unavailable,
                            $"User Spot '{spotId}' mesh changed.",
                            ZLinkRetryAdvice.RetryAfterBackoff
                        );
                    var source = state.SpotNodes.Values.FirstOrDefault(node =>
                        string.Equals(
                            node.Registration.SpotNodeName,
                            authority.MeshName,
                            StringComparison.Ordinal
                        )
                    );
                    if (source is not null && authority.NodeRid != source.Node.RoutingId)
                    {
                        var remaining = deadlineAt - Stopwatch.GetElapsedTime(0);
                        if (remaining <= TimeSpan.Zero)
                            throw CloseDeadlineElapsed(spotId);
                        var closed = await source
                            .Node.CloseUserSpotAsync(
                                authority.NodeRid,
                                new UserSpotCloseFence(
                                    spotId,
                                    spot.ObjectGeneration,
                                    authority.NodeRid,
                                    authority.NodeGeneration,
                                    found.Snapshot.AuthorityOwnerGeneration,
                                    found.Snapshot.StoreVersion
                                ),
                                deadlineUnixMs,
                                remaining,
                                cancellationToken
                            )
                            .ConfigureAwait(false);
                        return closed.Closed;
                    }
                    if (source is not null)
                        return await source
                            .CloseAsync(spotId, spot.ObjectGeneration, cancellationToken)
                            .ConfigureAwait(false);
                }

                // Only an incarnation of this generation in a transient state is
                // waited for; another generation ends the Close now (spec §7).
                ZLinkSpotNodeCatalog.ThrowIfOtherIncarnation(
                    spotId,
                    spot.ObjectGeneration,
                    found.Snapshot.ObjectGeneration
                );
                if (Stopwatch.GetElapsedTime(0) >= deadlineAt)
                    throw CloseDeadlineElapsed(spotId);
                await Task.Delay(LocationConvergencePollInterval, cancellationToken)
                    .ConfigureAwait(false);
            }
        }
        foreach (var node in state.SpotNodes.Values)
            if (
                await node.CloseAsync(spotId, spot.ObjectGeneration, cancellationToken)
                    .ConfigureAwait(false)
            )
                return true;

        return false;
    }

    private static ZLinkFrameworkException CloseDeadlineElapsed(string spotId) =>
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            $"User Spot '{spotId}' close was not admitted before its deadline."
        );

    internal async ValueTask<bool> CloseFromContextAsync(
        ZLinkFrameworkComponentState state,
        ZLinkSpotActivation activation,
        CancellationToken cancellationToken
    )
    {
        return state.SpotNodes.TryGetValue(activation.SpotNodeName, out var node)
            && await node
                .Catalog.CloseFromContextAsync(activation, cancellationToken)
                .ConfigureAwait(false);
    }

    public async ValueTask<bool> TryNotifyJoinedSpotActorDisconnectedAsync(
        ZLinkFrameworkComponentState state,
        string actorId,
        CancellationToken cancellationToken
    )
    {
        foreach (var activation in state.SpotNodes.Values.SelectMany(static node => node.Spots))
        {
            if (!activation.TryGetJoinedActor(actorId, out var actor) || actor is null)
                continue;

            await activation
                .NotifyActorDisconnectedAsync(actor, cancellationToken)
                .ConfigureAwait(false);
            return true;
        }

        return false;
    }

    public ZLinkSpotMonitoringSnapshot GetMonitoringSnapshot(
        ZLinkFrameworkComponentState state,
        string spotNodeName
    )
    {
        return GetNode(state, spotNodeName).GetMonitoringSnapshot();
    }

    private static ZLinkSpotNodeRuntime GetNode(
        ZLinkFrameworkComponentState state,
        string spotNodeName
    )
    {
        return state.SpotNodes.TryGetValue(spotNodeName, out var node)
            ? node
            : throw new ZLinkConfigurationException(
                $"SPOT node '{spotNodeName}' is not registered."
            );
    }

    private static ZLinkSpotNodeRuntime GetNodeForSpotFactory(
        ZLinkFrameworkComponentState state,
        Type spotType
    )
    {
        foreach (var node in state.SpotNodes.Values)
            if (node.SpotFactories.Contains(spotType))
                return node;

        throw new ZLinkConfigurationException($"SPOT factory '{spotType}' is not registered.");
    }

    public ZLinkSpotActivation GetActivationBySpotId(
        ZLinkFrameworkComponentState state,
        string spotId
    )
    {
        foreach (var node in state.SpotNodes.Values)
        {
            var activation = node.Spots.FirstOrDefault(current => current.SpotId == spotId);
            if (activation is not null)
                return activation;
        }

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Unavailable,
            $"Actor Join target '{spotId}' is not active."
        );
    }
}

internal readonly record struct EntrySpotActorReplyDispatchResult(
    bool Handled,
    ZLinkActorReply? Reply
);
