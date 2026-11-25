using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Default resolvers reading the registered stores. Direct object resolution
/// retains only positive Ready routes and bounds each entry by both its
/// captured cache age and owner-lease admission lifetime. Missing, Creating,
/// and failed reads never become cache entries.
/// </summary>
internal sealed class ZLinkStoreLocationResolvers : IZLinkMeshNodeLocationResolver
{
    private readonly IZLinkLocationRepository _store;
    private readonly ZLinkLocationRuntime? _runtime;
    private readonly ZLinkLiveLocationRows _liveRows;
    private readonly ZLinkLocationStoreHealth? _health;
    private readonly ZLinkOwnerLeaseTracker _leaseTracker;
    private readonly ZLinkLocationOptions _options;
    private readonly TimeProvider _time;
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<string, MeshObservation> _meshObservations = new(
        StringComparer.Ordinal
    );
    private long _nextPlacementSelection;

    private sealed record MeshObservation(IReadOnlyList<ZLinkMeshNodeDescriptor> Nodes)
    {
        internal TaskCompletionSource Changed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    internal ZLinkStateLane Lane => _lane;

    // Only in-flight reads retain observation state; the latest read may cache
    // only when every authority observation agrees with its StoreVersion.
    private readonly Dictionary<ZLinkSpotLocationKey, SpotRouteRead> _spotRouteReads = [];

    private sealed class SpotRouteRead(ZLinkSpotLocationKey key)
    {
        internal ZLinkSpotLocationKey Key { get; } = key;
        internal string? StoreVersion { get; set; }
    }

    private readonly Dictionary<
        ZLinkSpotLocationKey,
        CachedRoute<ZLinkResolvedSpotLocation>
    > _spotRoutes = [];
    private readonly Dictionary<
        ZLinkActorLocationKey,
        CachedRoute<ZLinkResolvedActorLocation>
    > _actorRoutes = [];

    internal ZLinkStoreLocationResolvers(
        IZLinkLocationRepository store,
        ZLinkOwnerLeaseTracker leaseTracker,
        ZLinkLocationStoreHealth? health = null,
        ZLinkLocationOptions? options = null,
        TimeProvider? timeProvider = null,
        ZLinkLocationRuntime? runtime = null
    )
    {
        _store = store;
        _runtime = runtime;
        _health = health;
        _leaseTracker = leaseTracker;
        _options = options ?? new ZLinkLocationOptions();
        _time = timeProvider ?? leaseTracker.TimeProvider;
        _liveRows = new ZLinkLiveLocationRows(leaseTracker);
    }

    public async ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
        string meshName,
        CancellationToken cancellationToken = default
    )
    {
        var previous = await _lane
            .RunAsync(() => _meshObservations.GetValueOrDefault(meshName))
            .ConfigureAwait(false);
        var rows = await ZLinkLocationStoreRead
            .ExecuteAsync(
                _health,
                "mesh-node-resolver-read",
                cancellationToken,
                storeToken => _store.ListAllMeshNodesAsync(meshName, storeToken)
            )
            .ConfigureAwait(false);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"autoconnect_store_snapshot mesh={meshName} raw_rows={rows.Count} raw_rids={string.Join(',', rows.Select(static row => row.Rid.ToString()))}"
        );

        var live = await _liveRows
            .FilterAsync(
                rows,
                static row => row.OwnerId,
                cancellationToken,
                static row => row.LeaseGeneration
            )
            .ConfigureAwait(false);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"autoconnect_live_snapshot mesh={meshName} live_rows={live.Count} live_rids={string.Join(',', live.Select(static row => row.Rid.ToString()))}"
        );
        await _lane
            .RunAsync(() =>
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (!ReferenceEquals(previous, _meshObservations.GetValueOrDefault(meshName)))
                    return;
                _meshObservations[meshName] = new MeshObservation(live);
                previous?.Changed.TrySetResult();
            })
            .ConfigureAwait(false);
        return live;
    }

    internal async ValueTask<ZLinkMeshNodeDescriptor?> SelectPlacementAsync(
        string meshName,
        ZLinkPlacementObjectKind kind,
        string stableType,
        Func<
            IReadOnlyList<ZLinkMeshNodeDescriptor>,
            IReadOnlyList<ZLinkMeshNodeDescriptor>
        > admitted,
        IReadOnlySet<(RoutingId Rid, ulong Generation)> excluded,
        CancellationToken cancellationToken
    )
    {
        var refreshAvailable = true;
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var observation = await _lane
                .RunAsync(() => _meshObservations.GetValueOrDefault(meshName))
                .ConfigureAwait(false);
            var candidates =
                observation
                    ?.Nodes.Where(candidate =>
                        !excluded.Contains((candidate.Rid, candidate.LifecycleGeneration))
                        && HasPlacementCapacity(candidate, kind, stableType)
                        && _leaseTracker.HasObservedAdmissionLease(
                            new ZLinkLocationOwnerToken(
                                candidate.OwnerId,
                                candidate.LeaseGeneration
                            )
                        )
                    )
                    .OrderBy(static candidate => candidate.Rid, ZLinkRoutingIdOrder.Instance)
                    .ToArray()
                ?? [];
            var eligible = admitted(
                candidates
                    .Where(static candidate =>
                        candidate.State == ZLinkFrameworkRuntimeState.Serving
                    )
                    .ToArray()
            );
            if (
                !ReferenceEquals(
                    observation,
                    await _lane
                        .RunAsync(() => _meshObservations.GetValueOrDefault(meshName))
                        .ConfigureAwait(false)
                )
            )
                continue;

            cancellationToken.ThrowIfCancellationRequested();
            if (eligible.Count > 0)
                return ZLinkWeightedSelector.Select(
                    eligible,
                    static candidate => candidate.PlacementWeight,
                    ref _nextPlacementSelection
                );
            if (observation is not null && candidates.Length > 0)
            {
                await observation.Changed.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
                continue;
            }
            if (!refreshAvailable)
                return null;
            refreshAvailable = false;
            await ListLiveMeshNodesAsync(meshName, cancellationToken)
                .AsTask()
                .WaitAsync(cancellationToken)
                .ConfigureAwait(false);
        }
    }

    internal static bool HasPlacementCapacity(
        ZLinkMeshNodeDescriptor candidate,
        ZLinkPlacementObjectKind kind,
        string stableType
    )
    {
        if (
            candidate.State
                is not (ZLinkFrameworkRuntimeState.Preparing or ZLinkFrameworkRuntimeState.Serving)
            || candidate.ObjectRole != ZLinkMeshNodeObjectRole.Server
            || candidate.PlacementWeight <= 0
            || !candidate.ObjectCapabilities.Any(capability =>
                capability.ObjectKind == kind
                && string.Equals(capability.StableType, stableType, StringComparison.Ordinal)
                && (
                    kind != ZLinkPlacementObjectKind.Actor
                    || (candidate.EntrySpotId is not null && capability.EntryAccepted)
                )
            )
        )
            return false;

        if (kind == ZLinkPlacementObjectKind.Actor)
        {
            var actors = candidate.Capacity.Actors;
            return actors.Limit == 0 || actors.Active + (long)actors.Reserved < actors.Limit;
        }

        var spots = candidate.Capacity.Spots;
        return (spots.Limit == 0 || spots.Active + (long)spots.Reserved < spots.Limit)
            && candidate.Capacity.SpotTypes.Any(capacity =>
                capacity.ObjectKind == kind
                && string.Equals(capacity.StableType, stableType, StringComparison.Ordinal)
                && (
                    capacity.Limit == 0
                    || capacity.Active + (long)capacity.Reserved < capacity.Limit
                )
            );
    }

    internal async ValueTask<ZLinkResolvedSpotLocation?> ResolveSpotRowAsync(
        ZLinkSpotLocationKey key,
        CancellationToken cancellationToken = default
    )
    {
        var result = await ResolveSpotRowWithStatusAsync(key, cancellationToken)
            .ConfigureAwait(false);
        return result.Row;
    }

    internal async ValueTask<(
        ZLinkResolvedSpotLocation? Row,
        ZLinkLocationResolutionKind Kind,
        ZLinkAuthoritySnapshot? Authority
    )> ResolveSpotRowWithStatusAsync(
        ZLinkSpotLocationKey key,
        CancellationToken cancellationToken = default
    )
    {
        var result = await ResolveSpotRowCoreAsync(key, cancellationToken).ConfigureAwait(false);
        return (result.Row, result.Kind, result.Authority);
    }

    internal async ValueTask<ZLinkResolvedActorLocation?> ResolveActorRowAsync(
        ZLinkActorLocationKey key,
        CancellationToken cancellationToken = default
    )
    {
        var result = await ResolveActorRowWithStatusAsync(key, cancellationToken)
            .ConfigureAwait(false);
        return result.Row;
    }

    internal async ValueTask<(
        ZLinkResolvedActorLocation? Row,
        ZLinkLocationResolutionKind Kind
    )> ResolveActorRowWithStatusAsync(
        ZLinkActorLocationKey key,
        CancellationToken cancellationToken = default
    )
    {
        var result = await ResolveActorRowCoreAsync(key, cancellationToken).ConfigureAwait(false);
        return (result.Row, result.Kind);
    }

    /// <summary>Resolve plus live-row presence: callers that retry a transient
    /// resolve window (a claimed-but-unpublished generation-0 row, a lagging
    /// replica view) need to distinguish it from a confirmed miss. A row owned
    /// by an expired process is a miss even while stale storage remains.</summary>
    internal async ValueTask<(
        ZLinkResolvedActorLocation? Row,
        bool RowPresent
    )> ResolveActorRowWithPresenceAsync(
        ZLinkActorLocationKey key,
        CancellationToken cancellationToken = default
    )
    {
        var result = await ResolveActorRowCoreAsync(key, cancellationToken).ConfigureAwait(false);
        return (result.Row, result.LiveRowPresent);
    }

    private async ValueTask<(
        ZLinkResolvedSpotLocation? Row,
        bool LiveRowPresent,
        ZLinkLocationResolutionKind Kind,
        ZLinkAuthoritySnapshot? Authority
    )> ResolveSpotRowCoreAsync(ZLinkSpotLocationKey key, CancellationToken cancellationToken)
    {
        var cached = await TryGetCachedAsync(_spotRoutes, key).ConfigureAwait(false);
        if (cached.Found)
        {
            if (ZLinkFrameworkDebugLog.SpotDiscoveryEnabled)
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"resolve_spot_row spot={key.SpotId} source=cache hit={cached.Row is not null}"
                );
            return (cached.Row, true, ZLinkLocationResolutionKind.Ready, null);
        }

        var read = new SpotRouteRead(key);
        await _lane.RunAsync(() => _spotRouteReads[key] = read).ConfigureAwait(false);
        try
        {
            _runtime?.EnsureOwnerAdmissionOpen();
            if (ZLinkFrameworkDebugLog.SpotDiscoveryEnabled)
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"resolve_spot_row spot={key.SpotId} source=store"
                );
            var authority = await ZLinkLocationStoreRead
                .ExecuteAsync(
                    _health,
                    "ZLinkSpotLocation-resolver-read",
                    cancellationToken,
                    storeToken =>
                        _store.ReadAuthorityAsync(
                            ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(key.SpotId),
                            storeToken
                        )
                )
                .ConfigureAwait(false);
            var snapshot = (authority as ZLinkAuthorityReadResult.Found)?.Snapshot;
            var raw = ProjectSpot(authority);
            var (row, lease) = await _liveRows
                .ResolveWithPresenceAsync(raw, static row => row.OwnerId, cancellationToken)
                .ConfigureAwait(false);
            var liveRowPresent = lease is not null;
            if (row is null)
            {
                await InvalidateSpotRouteAsync(key).ConfigureAwait(false);
                return (
                    null,
                    liveRowPresent,
                    raw is not null ? ZLinkLocationResolutionKind.KnownUnavailable
                    : IsClosingSpot(authority) ? ZLinkLocationResolutionKind.Closing
                    : ZLinkLocationResolutionKind.Missing,
                    snapshot
                );
            }

            if (IsClosingSpot(authority))
            {
                await InvalidateSpotRouteAsync(key).ConfigureAwait(false);
                var admission = _leaseTracker.GetAdmissionLifetime(
                    lease!,
                    new ZLinkLocationOwnerToken(row.OwnerId, row.LeaseGeneration)
                );
                return admission is null
                    ? (null, liveRowPresent, ZLinkLocationResolutionKind.KnownUnavailable, snapshot)
                    : (row, liveRowPresent, ZLinkLocationResolutionKind.Closing, snapshot);
            }

            var readyAdmission = GetReadyRouteAdmission(authority, lease!);
            if (readyAdmission is not { } admitted)
            {
                await InvalidateSpotRouteAsync(key).ConfigureAwait(false);
                return (
                    null,
                    liveRowPresent,
                    ZLinkLocationResolutionKind.KnownUnavailable,
                    snapshot
                );
            }
            await CacheReadyRouteAsync(_spotRoutes, key, row, admitted, read).ConfigureAwait(false);
            return (row, liveRowPresent, ZLinkLocationResolutionKind.Ready, null);
        }
        finally
        {
            await _lane
                .RunAsync(() =>
                {
                    if (
                        _spotRouteReads.TryGetValue(key, out var current)
                        && ReferenceEquals(current, read)
                    )
                        _spotRouteReads.Remove(key);
                })
                .ConfigureAwait(false);
        }
    }

    private async ValueTask<(
        ZLinkResolvedActorLocation? Row,
        bool LiveRowPresent,
        ZLinkLocationResolutionKind Kind
    )> ResolveActorRowCoreAsync(ZLinkActorLocationKey key, CancellationToken cancellationToken)
    {
        var cached = await TryGetCachedAsync(_actorRoutes, key).ConfigureAwait(false);
        if (cached.Found)
            return (cached.Row, true, ZLinkLocationResolutionKind.Ready);

        var authority = await ReadActorAuthorityAsync(key, cancellationToken).ConfigureAwait(false);
        var (row, present, kind, admission) = await ResolveActorAuthorityAsync(
                key,
                authority,
                cancellationToken
            )
            .ConfigureAwait(false);
        if (row is not null && admission is { } admitted)
            await CacheReadyRouteAsync(_actorRoutes, key, row, admitted).ConfigureAwait(false);
        else
            await InvalidateActorRouteAsync(key).ConfigureAwait(false);
        return (row, present, kind);
    }

    /// <summary>Reads current Actor authority for observation.</summary>
    internal async ValueTask<ZLinkResolvedActorLocation?> ReadActorRowAsync(
        ZLinkActorLocationKey key,
        CancellationToken cancellationToken = default
    ) => (await ReadActorForConfirmationAsync(key, cancellationToken).ConfigureAwait(false)).Row;

    internal async ValueTask<ZLinkResolvedActorLocation> ReadActorSessionBindingRowAsync(
        ActorRef actor,
        CancellationToken cancellationToken
    )
    {
        var key = new ZLinkActorLocationKey(actor.ActorId);
        var (row, authority) = await ReadActorForConfirmationAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (authority is ZLinkAuthorityReadResult.Missing)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor '{actor.ActorId}' authority was not found."
            );
        if (row is not null && row.ActorRef.ObjectGeneration != actor.ObjectGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actor.ActorId}' binding ObjectGeneration {actor.ObjectGeneration} differs from the current {row.ActorRef.ObjectGeneration}."
            );
        if (row is null || row.ActorRef != actor)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor '{actor.ActorId}' binding route is unavailable."
            );
        return row;
    }

    private async ValueTask<(
        ZLinkResolvedActorLocation? Row,
        ZLinkAuthorityReadResult Authority
    )> ReadActorForConfirmationAsync(ZLinkActorLocationKey key, CancellationToken cancellationToken)
    {
        // Compare the opaque StoreVersion against the entry that preceded the read.
        // A concurrent direct lookup can install a newer route while this read runs.
        var cached = await _lane
            .RunAsync(() => _actorRoutes.GetValueOrDefault(key))
            .ConfigureAwait(false);
        var authority = await ReadActorAuthorityAsync(key, cancellationToken).ConfigureAwait(false);
        var (row, _, _, _) = await ResolveActorAuthorityAsync(key, authority, cancellationToken)
            .ConfigureAwait(false);
        if (
            cached is not null
            && (
                row is null
                || authority is not ZLinkAuthorityReadResult.Found found
                || cached.StoreVersion != found.Snapshot.StoreVersion
            )
        )
            await _lane
                .RunAsync(() =>
                {
                    if (ReferenceEquals(_actorRoutes.GetValueOrDefault(key), cached))
                        _actorRoutes.Remove(key);
                })
                .ConfigureAwait(false);

        return (row, authority);
    }

    private async ValueTask<ZLinkAuthorityReadResult> ReadActorAuthorityAsync(
        ZLinkActorLocationKey key,
        CancellationToken cancellationToken
    )
    {
        _runtime?.EnsureOwnerAdmissionOpen();
        return await ZLinkLocationStoreRead
            .ExecuteAsync(
                _health,
                "ZLinkActorLocation-resolver-read",
                cancellationToken,
                storeToken =>
                    _store.ReadAuthorityAsync(
                        ZLinkActorAuthorityPayloadCodec.AuthorityKey(key.ActorId),
                        storeToken
                    )
            )
            .ConfigureAwait(false);
    }

    private async ValueTask<(
        ZLinkResolvedActorLocation? Row,
        bool LiveRowPresent,
        ZLinkLocationResolutionKind Kind,
        (ZLinkAuthoritySnapshot Snapshot, long MeasuredAt, TimeSpan Lifetime)? Admission
    )> ResolveActorAuthorityAsync(
        ZLinkActorLocationKey key,
        ZLinkAuthorityReadResult authority,
        CancellationToken cancellationToken
    )
    {
        var raw = ProjectActor(authority, key.ActorId);
        var (row, lease) = await _liveRows
            .ResolveWithPresenceAsync(raw, static row => row.OwnerId, cancellationToken)
            .ConfigureAwait(false);
        var liveRowPresent = lease is not null;
        // Reference generation 0 marks a claimed-but-unpublished actor:
        // the claim precedes activation, so such a row is never a
        // resolvable reference (40-location-runtime §6).
        if (row is not null)
            row = row.ActorRef.ObjectGeneration > 0 ? row : null;
        if (row is null)
        {
            return (
                null,
                liveRowPresent,
                raw is null
                    ? ZLinkLocationResolutionKind.Missing
                    : ZLinkLocationResolutionKind.KnownUnavailable,
                null
            );
        }

        var admission = GetReadyRouteAdmission(authority, lease!);
        return admission is null
            ? (null, liveRowPresent, ZLinkLocationResolutionKind.KnownUnavailable, null)
            : (row, liveRowPresent, ZLinkLocationResolutionKind.Ready, admission);
    }

    internal ValueTask ObserveAuthorityAsync(ZLinkAuthorityKey key, string? storeVersion = null)
    {
        if (!ZLinkAuthorityKeyCodec.TryDecodeSpot(key, out var spotId))
            return ValueTask.CompletedTask;
        var spotKey = new ZLinkSpotLocationKey(spotId);
        return _lane.RunAsync(() =>
        {
            if (_spotRouteReads.TryGetValue(spotKey, out var read))
            {
                if (
                    storeVersion is null
                    || (
                        read.StoreVersion is not null
                        && !string.Equals(read.StoreVersion, storeVersion, StringComparison.Ordinal)
                    )
                )
                    _spotRouteReads.Remove(spotKey);
                else
                    read.StoreVersion = storeVersion;
            }
            // StoreVersion is opaque: a different observation invalidates the snapshot.
            if (
                _spotRoutes.TryGetValue(spotKey, out var cached)
                && !string.Equals(cached.StoreVersion, storeVersion, StringComparison.Ordinal)
            )
                _spotRoutes.Remove(spotKey);
        });
    }

    internal ValueTask InvalidateSpotRouteAsync(ZLinkSpotLocationKey key) =>
        _lane.RunAsync(() =>
        {
            _spotRouteReads.Remove(key);
            _spotRoutes.Remove(key);
        });

    internal ValueTask InvalidateActorRouteAsync(ZLinkActorLocationKey key) =>
        _lane.RunAsync(() =>
        {
            _actorRoutes.Remove(key);
        });

    internal void InvalidateActorRoute(
        ZLinkActorLocationKey key,
        ActorRef actorRef,
        ulong ownerNodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration
    )
    {
        ValueTask invalidation;
        try
        {
            invalidation = _lane.RunAsync(() =>
            {
                if (!_actorRoutes.TryGetValue(key, out var cached))
                    return ValueTask.CompletedTask;
                var row = cached.Row;
                if (
                    row.ActorRef != actorRef
                    || row.OwnerNodeGeneration != ownerNodeGeneration
                    || row.AuthorityOwnerGeneration != authorityOwnerGeneration
                    || row.LeaseGeneration <= 0
                    || (ulong)row.LeaseGeneration != ownerLeaseGeneration
                )
                    return ValueTask.CompletedTask;
                _actorRoutes.Remove(key);
                return ValueTask.CompletedTask;
            });
        }
        catch (Exception failure)
            when (failure is ObjectDisposedException or InvalidOperationException)
        {
            ZLinkFrameworkDebugLog.TaskFailure("actor-route-invalidation", failure);
            return;
        }
        _ = ObserveActorRouteInvalidationAsync(invalidation);
    }

    private static async Task ObserveActorRouteInvalidationAsync(ValueTask invalidation)
    {
        try
        {
            await invalidation.ConfigureAwait(false);
        }
        catch (Exception failure)
        {
            ZLinkFrameworkDebugLog.TaskFailure("actor-route-invalidation", failure);
        }
    }

    internal bool InvalidateMessageFollowRoute(ZLinkServiceWireCodec.MessageFollowRecord record)
    {
        var source = record.Source;
        var key = source.IsActor ? new ZLinkActorLocationKey(source.ObjectId) : default;
        return AwaitStateLane(
            _lane.RunAsync(() =>
            {
                if (source.IsActor)
                {
                    if (!_actorRoutes.TryGetValue(key, out var cached))
                        return false;
                    var row = cached.Row;
                    if (
                        row.ActorId != source.ObjectId
                        || row.ActorRef.ObjectGeneration != source.ObjectGeneration
                        || row.OwnerNodeRid != source.TargetNodeRid
                        || row.ActorRef.NodeRid != source.TargetNodeRid
                        || row.OwnerNodeGeneration != source.TargetNodeGeneration
                        || row.AuthorityOwnerGeneration != source.AuthorityOwnerGeneration
                        || row.LeaseGeneration <= 0
                        || (ulong)row.LeaseGeneration != source.OwnerLeaseGeneration
                    )
                        return false;
                    return _actorRoutes.Remove(key);
                }

                var spotKey = new ZLinkSpotLocationKey(source.ObjectId);
                if (!_spotRoutes.TryGetValue(spotKey, out var spotCached))
                    return false;
                var spot = spotCached.Row;
                if (
                    spot.SpotId != source.ObjectId
                    || spot.SpotGeneration != source.ObjectGeneration
                    || spot.OwnerNodeRid != source.TargetNodeRid
                    || spot.OwnerNodeGeneration != source.TargetNodeGeneration
                    || spot.AuthorityOwnerGeneration != source.AuthorityOwnerGeneration
                    || spot.LeaseGeneration <= 0
                    || (ulong)spot.LeaseGeneration != source.OwnerLeaseGeneration
                )
                    return false;
                return _spotRoutes.Remove(spotKey);
            })
        );
    }

    internal bool HasCachedActorRoute(ZLinkActorLocationKey key)
    {
        return AwaitStateLane(_lane.RunAsync(() => _actorRoutes.ContainsKey(key)));
    }

    private ValueTask<(bool Found, TRow? Row)> TryGetCachedAsync<TKey, TRow>(
        Dictionary<TKey, CachedRoute<TRow>> routes,
        TKey key
    )
        where TKey : notnull
        where TRow : class
    {
        return _lane.RunAsync(() =>
        {
            if (!routes.TryGetValue(key, out var route))
                return (false, (TRow?)null);

            var cacheAge = _time.GetElapsedTime(route.StoredAt);
            var ownerLeaseAge = _leaseTracker.TimeProvider.GetElapsedTime(
                route.OwnerLeaseLifetimeMeasuredAt
            );
            if (
                cacheAge >= route.MaxAge
                || ownerLeaseAge >= route.OwnerLeaseLifetime
                || (
                    _health is not null
                    && route.StoreRecoveryGeneration != _health.RecoveryGeneration
                )
            )
            {
                routes.Remove(key);
                return (false, (TRow?)null);
            }

            return (true, route.Row);
        });
    }

    private (
        ZLinkAuthoritySnapshot Snapshot,
        long MeasuredAt,
        TimeSpan Lifetime
    )? GetReadyRouteAdmission(
        ZLinkAuthorityReadResult authority,
        ZLinkOwnerLeaseTracker.Snapshot lease
    )
    {
        if (authority is not ZLinkAuthorityReadResult.Found found)
            return null;

        var admission = _leaseTracker.GetAdmissionLifetime(
            lease,
            new ZLinkLocationOwnerToken(found.Snapshot.OwnerId, found.Snapshot.OwnerLeaseGeneration)
        );
        return admission is { } leaseLifetime
            ? (found.Snapshot, leaseLifetime.MeasuredAt, leaseLifetime.Lifetime)
            : null;
    }

    private async ValueTask CacheReadyRouteAsync<TKey, TRow>(
        Dictionary<TKey, CachedRoute<TRow>> routes,
        TKey key,
        TRow row,
        (ZLinkAuthoritySnapshot Snapshot, long MeasuredAt, TimeSpan Lifetime) admission,
        SpotRouteRead? read = null
    )
        where TKey : notnull
        where TRow : class
    {
        var maxAge = _options.RouteCacheMaxAge;
        if (maxAge <= TimeSpan.Zero)
            return;

        var (snapshot, measuredAt, lifetime) = admission;
        var route = new CachedRoute<TRow>(
            row,
            snapshot.StoreVersion,
            snapshot.ObjectGeneration,
            snapshot.AuthorityOwnerGeneration,
            snapshot.OwnerLeaseGeneration,
            _time.GetTimestamp(),
            maxAge,
            measuredAt,
            lifetime,
            _health?.RecoveryGeneration ?? 0
        );
        await _lane
            .RunAsync(() =>
            {
                if (
                    read is not null
                    && (
                        !_spotRouteReads.TryGetValue(read.Key, out var current)
                        || !ReferenceEquals(current, read)
                        || (
                            read.StoreVersion is not null
                            && !string.Equals(
                                read.StoreVersion,
                                route.StoreVersion,
                                StringComparison.Ordinal
                            )
                        )
                    )
                )
                    return;
                routes[key] = route;
            })
            .ConfigureAwait(false);
    }

    // Closing 상태의 User 또는 Instance Spot은 기존 owner route를 유지한다.
    // owner가 원 message intent에 따라 Close 결과를 결정한다(08-routing resolver 계약).
    private static bool IsClosingSpot(ZLinkAuthorityReadResult authority) =>
        authority is ZLinkAuthorityReadResult.Found found
        && (
            ZLinkUserSpotAuthorityPayloadCodec.TryDecode(found.Snapshot.Payload.Span, out var user)
                && user.State == ZLinkUserSpotAuthorityState.Closing
            || ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var instance
            )
                && instance.State == ZLinkInstanceSpotAuthorityState.Closing
        );

    internal static SpotRef? ProjectReadyUserSpot(string spotId, ZLinkAuthorityReadResult authority)
    {
        if (
            authority is not ZLinkAuthorityReadResult.Found found
            || found.Snapshot.Allocation.State != ZLinkPlacementAllocationState.Active
            || found.Snapshot.Allocation.ObjectKind != ZLinkPlacementObjectKind.UserSpot
            || found.Snapshot.OwnerLeaseGeneration <= 0
        )
            return null;

        var snapshot = found.Snapshot;
        var descriptor = snapshot.Allocation.Descriptor;
        if (
            snapshot.Relocation is not null
            || !ZLinkUserSpotAuthorityPayloadCodec.TryDecode(snapshot.Payload.Span, out var user)
        )
        {
            if (
                !TryReadCommittedCanonicalTarget(snapshot, out var canonical, out _)
                || canonical.Phase < (byte)ServiceWireCodec.RelocationPhase.Activated
                || canonical.TargetAttemptGeneration == 0
                || canonical.TargetOwnerLeaseGeneration != (ulong)snapshot.OwnerLeaseGeneration
                || canonical.State.TargetNodeGeneration
                    != snapshot.Allocation.DescriptorLifecycleGeneration
                || !ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                    canonical.SteadyAuthorityPayload.Span,
                    out user
                )
            )
                return null;
        }
        else if (
            user.OwnerId != snapshot.OwnerId
            || user.OwnerLeaseGeneration != (ulong)snapshot.OwnerLeaseGeneration
            || user.NodeRid != descriptor.Rid
            || user.NodeGeneration != snapshot.Allocation.DescriptorLifecycleGeneration
        )
            return null;

        return
            user.State == ZLinkUserSpotAuthorityState.Ready
            && user.SpotId == spotId
            && user.MeshName == descriptor.MeshName
            ? new SpotRef(spotId, snapshot.ObjectGeneration, descriptor.MeshName, descriptor.Rid)
            : null;
    }

    private static ZLinkResolvedSpotLocation? ProjectSpot(ZLinkAuthorityReadResult authority)
    {
        if (authority is not ZLinkAuthorityReadResult.Found found)
        {
            //  "No row in the store" and "row present but rejected" are
            //  different failures that both surfaced as the same bare null.
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"project_spot_no_authority result={authority.GetType().Name}"
            );
            return null;
        }
        var snapshot = found.Snapshot;
        var userDecoded = ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
            snapshot.Payload.Span,
            out var user
        );
        if (userDecoded)
            //  A user-spot row that decodes but fails a guard used to vanish as
            //  a bare null, which reads the same as "no row at all" at the
            //  caller. Name the values the guards compare.
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"project_user_spot spot={user.SpotId} state={user.State} payload_owner={user.OwnerId} snapshot_owner={snapshot.OwnerId} payload_lease={user.OwnerLeaseGeneration} snapshot_lease={snapshot.OwnerLeaseGeneration}"
            );
        if (
            userDecoded
            && user.State
                is ZLinkUserSpotAuthorityState.Ready
                    or ZLinkUserSpotAuthorityState.Closing
            && user.OwnerId == snapshot.OwnerId
            && snapshot.OwnerLeaseGeneration > 0
            && (
                snapshot.Relocation is null
                || user.OwnerLeaseGeneration == (ulong)snapshot.OwnerLeaseGeneration
                    && user.NodeRid == snapshot.Allocation.Descriptor.Rid
                    && user.NodeGeneration == snapshot.Allocation.DescriptorLifecycleGeneration
            )
        )
            return new ZLinkResolvedSpotLocation(
                user.MeshName,
                user.SpotId,
                snapshot.ObjectGeneration,
                user.NodeRid,
                user.NodeGeneration,
                ZLinkSpotKind.User,
                user.StableType,
                snapshot.OwnerId,
                snapshot.OwnerLeaseGeneration,
                snapshot.StoreNow,
                snapshot.AuthorityOwnerGeneration,
                snapshot.StoreVersion
            );
        if (
            ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var instance
            )
            && instance.State
                is ZLinkInstanceSpotAuthorityState.Ready
                    or ZLinkInstanceSpotAuthorityState.Closing
            && instance.OwnerId == snapshot.OwnerId
            && snapshot.OwnerLeaseGeneration > 0
            && (
                snapshot.Relocation is null
                || instance.OwnerLeaseGeneration == (ulong)snapshot.OwnerLeaseGeneration
                    && instance.NodeRid == snapshot.Allocation.Descriptor.Rid
                    && instance.NodeGeneration == snapshot.Allocation.DescriptorLifecycleGeneration
            )
        )
            return new ZLinkResolvedSpotLocation(
                instance.MeshName,
                instance.SpotId,
                snapshot.ObjectGeneration,
                instance.NodeRid,
                instance.NodeGeneration,
                ZLinkSpotKind.Instance,
                instance.StableType,
                snapshot.OwnerId,
                snapshot.OwnerLeaseGeneration,
                snapshot.StoreNow,
                snapshot.AuthorityOwnerGeneration,
                snapshot.StoreVersion
            );
        if (!TryReadCommittedCanonicalTarget(snapshot, out var canonical, out var targetNodeRid))
            return null;
        if (
            ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                canonical.SteadyAuthorityPayload.Span,
                out user
            )
            && user.State == ZLinkUserSpotAuthorityState.Ready
            && user.MeshName == snapshot.Allocation.Descriptor.MeshName
        )
            return new ZLinkResolvedSpotLocation(
                user.MeshName,
                user.SpotId,
                snapshot.ObjectGeneration,
                targetNodeRid,
                canonical.State.TargetNodeGeneration,
                ZLinkSpotKind.User,
                user.StableType,
                snapshot.OwnerId,
                snapshot.OwnerLeaseGeneration,
                snapshot.StoreNow,
                snapshot.AuthorityOwnerGeneration,
                snapshot.StoreVersion
            );
        if (
            ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                canonical.SteadyAuthorityPayload.Span,
                out instance
            )
            && instance.State == ZLinkInstanceSpotAuthorityState.Ready
            && instance.MeshName == snapshot.Allocation.Descriptor.MeshName
        )
            return new ZLinkResolvedSpotLocation(
                instance.MeshName,
                instance.SpotId,
                snapshot.ObjectGeneration,
                targetNodeRid,
                canonical.State.TargetNodeGeneration,
                ZLinkSpotKind.Instance,
                instance.StableType,
                snapshot.OwnerId,
                snapshot.OwnerLeaseGeneration,
                snapshot.StoreNow,
                snapshot.AuthorityOwnerGeneration,
                snapshot.StoreVersion
            );
        return null;
    }

    private static ZLinkResolvedActorLocation? ProjectActor(
        ZLinkAuthorityReadResult authority,
        string actorId
    )
    {
        if (authority is not ZLinkAuthorityReadResult.Found found)
            return null;
        var snapshot = found.Snapshot;
        if (
            !ZLinkActorAuthorityPayloadCodec.TryDecode(snapshot.Payload.Span, out var actor)
            || actor.State != ZLinkActorAuthorityState.Ready
            || actor.OwnerId != snapshot.OwnerId
            || snapshot.OwnerLeaseGeneration <= 0
            || actor.OwnerLeaseGeneration != (ulong)snapshot.OwnerLeaseGeneration
            || snapshot.Relocation is not null
                && (
                    actor.NodeRid != snapshot.Allocation.Descriptor.Rid
                    || actor.NodeGeneration != snapshot.Allocation.DescriptorLifecycleGeneration
                )
        )
        {
            if (
                !TryReadCommittedCanonicalTarget(snapshot, out var canonical, out var targetNodeRid)
                || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                    canonical.SteadyAuthorityPayload.Span,
                    out actor
                )
                || actor.State != ZLinkActorAuthorityState.Ready
                || actor.MeshName != snapshot.Allocation.Descriptor.MeshName
            )
                return ProjectCanonicalActor(snapshot, actorId);
            return new ZLinkResolvedActorLocation(
                actor.MeshName,
                actor.ActorId,
                actor.StableType,
                new ActorRef(
                    actor.ActorId,
                    snapshot.ObjectGeneration,
                    actor.MeshName,
                    targetNodeRid
                ),
                targetNodeRid,
                canonical.State.TargetNodeGeneration,
                actor.CurrentSpotId,
                actor.CurrentSpotGeneration,
                actor.CurrentSpotKind,
                snapshot.AuthorityOwnerGeneration,
                snapshot.OwnerId,
                snapshot.OwnerLeaseGeneration,
                snapshot.StoreNow,
                snapshot.AuthorityOwnerGeneration
            );
        }
        return new ZLinkResolvedActorLocation(
            actor.MeshName,
            actor.ActorId,
            actor.StableType,
            new ActorRef(actor.ActorId, snapshot.ObjectGeneration, actor.MeshName, actor.NodeRid),
            actor.NodeRid,
            actor.NodeGeneration,
            actor.CurrentSpotId,
            actor.CurrentSpotGeneration,
            actor.CurrentSpotKind,
            snapshot.AuthorityOwnerGeneration,
            snapshot.OwnerId,
            snapshot.OwnerLeaseGeneration,
            snapshot.StoreNow,
            snapshot.AuthorityOwnerGeneration
        );
    }

    // The outer authority row is the cross-language contract: payload is
    // application-defined opaque bytes (21-location-runtime §2.4).  Keep the
    // native codec for its membership projection, but never require a foreign
    // writer's payload dialect merely to resolve the owner route.
    private static ZLinkResolvedActorLocation? ProjectCanonicalActor(
        ZLinkAuthoritySnapshot snapshot,
        string actorId
    )
    {
        if (
            snapshot.Allocation.ObjectKind != ZLinkPlacementObjectKind.Actor
            || snapshot.Allocation.State != ZLinkPlacementAllocationState.Active
            || snapshot.ObjectGeneration == 0
            || snapshot.OwnerLeaseGeneration <= 0
            || string.IsNullOrWhiteSpace(actorId)
            || string.IsNullOrWhiteSpace(snapshot.Allocation.StableType)
            || string.IsNullOrWhiteSpace(snapshot.Allocation.Descriptor.MeshName)
        )
            return null;
        var nodeRid = snapshot.Allocation.Descriptor.Rid;
        if (nodeRid.IsEmpty)
            return null;
        return new ZLinkResolvedActorLocation(
            snapshot.Allocation.Descriptor.MeshName,
            actorId,
            snapshot.Allocation.StableType,
            new ActorRef(
                actorId,
                snapshot.ObjectGeneration,
                snapshot.Allocation.Descriptor.MeshName,
                nodeRid
            ),
            nodeRid,
            snapshot.Allocation.DescriptorLifecycleGeneration,
            string.Empty,
            0,
            ZLinkSpotKind.Entry,
            snapshot.AuthorityOwnerGeneration,
            snapshot.OwnerId,
            snapshot.OwnerLeaseGeneration,
            snapshot.StoreNow,
            snapshot.AuthorityOwnerGeneration
        );
    }

    private static bool TryReadCommittedCanonicalTarget(
        ZLinkAuthoritySnapshot snapshot,
        out ZLinkCanonicalRelocationAuthorityProjection canonical,
        out RoutingId targetNodeRid
    )
    {
        canonical = null!;
        targetNodeRid = default;
        if (
            snapshot.OwnerLeaseGeneration <= 0
            || !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(snapshot, out canonical)
            || canonical.Phase is < 4 or > 8
            || canonical.TargetOwnerId != snapshot.OwnerId
        )
            return false;
        try
        {
            targetNodeRid = RoutingId.FromHex(canonical.State.TargetNodeRid);
        }
        catch (ArgumentException)
        {
            return false;
        }
        return targetNodeRid == snapshot.Allocation.Descriptor.Rid;
    }

    private sealed record CachedRoute<TRow>(
        TRow Row,
        string StoreVersion,
        ulong ObjectGeneration,
        ulong AuthorityOwnerGeneration,
        long OwnerLeaseGeneration,
        long StoredAt,
        TimeSpan MaxAge,
        long OwnerLeaseLifetimeMeasuredAt,
        TimeSpan OwnerLeaseLifetime,
        long StoreRecoveryGeneration
    )
        where TRow : class;
}
