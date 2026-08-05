using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Default resolvers reading the registered stores. Direct object resolution
/// retains only positive Ready routes and bounds each entry by both its
/// captured cache age and owner-lease admission lifetime. Missing, Creating,
/// and failed reads never become cache entries.
/// </summary>
internal sealed class ZLinkStoreLocationResolvers :
    IZLinkMeshNodeLocationResolver
{
    private const int MaximumStaleSnapshotRetries = 3;
    private readonly IZLinkLocationRepository _store;
    private readonly ZLinkObservedLocationGenerations _observed;
    private readonly ZLinkLiveLocationRows _liveRows;
    private readonly ZLinkLocationStoreHealth? _health;
    private readonly ZLinkOwnerLeaseTracker _leaseTracker;
    private readonly ZLinkLocationOptions _options;
    private readonly TimeProvider _time;
    private readonly object _routeCacheGate = new();
    private readonly Dictionary<ZLinkSpotLocationKey, CachedRoute<ZLinkResolvedSpotLocation>>
        _spotRoutes = [];
    private readonly Dictionary<ZLinkActorLocationKey, CachedRoute<ZLinkResolvedActorLocation>>
        _actorRoutes = [];

    internal ZLinkStoreLocationResolvers(
        IZLinkLocationRepository store,
        ZLinkOwnerLeaseTracker leaseTracker,
        ZLinkObservedLocationGenerations observed,
        ZLinkLocationStoreHealth? health = null,
        ZLinkLocationOptions? options = null,
        TimeProvider? timeProvider = null)
    {
        _store = store;
        _observed = observed;
        _health = health;
        _leaseTracker = leaseTracker;
        _options = options ?? new ZLinkLocationOptions();
        _time = timeProvider ?? leaseTracker.TimeProvider;
        _liveRows = new ZLinkLiveLocationRows(leaseTracker);
    }

    public async ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
        string meshName,
        CancellationToken cancellationToken = default)
    {
        for (var attempt = 0; ; attempt++)
        {
            var rows = await ZLinkLocationStoreRead.ExecuteAsync(
                _health,
                "mesh-node-resolver-read",
                cancellationToken,
                storeToken => _store.ListAllMeshNodesAsync(meshName, storeToken))
                .ConfigureAwait(false);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"autoconnect_store_snapshot mesh={meshName} raw_rows={rows.Count} "
                + $"raw_rids={string.Join(',', rows.Select(static row => row.Rid.ToString()))}");
            _observed.ReconcileDescriptors(meshName, rows);

            // The shared acceptance policy rejects lagging lifecycle generation
            // and descriptor revision views. A local descriptor update may
            // finish after this snapshot begins but before its rows are
            // filtered. Retry that bounded race without accepting the older
            // row; a retired owner remains rejected on every attempt.
            var rejectedByOlderRevision = false;
            var live = await _liveRows.FilterAsync(
                    rows,
                    static row => row.OwnerId,
                    row =>
                    {
                        var accepted = _observed.AcceptDescriptor(
                            row,
                            out var olderRevision);
                        rejectedByOlderRevision |= olderRevision;
                        return accepted;
                    },
                    cancellationToken,
                    static row => row.LeaseGeneration)
                .ConfigureAwait(false);
            if (!rejectedByOlderRevision
                || attempt >= MaximumStaleSnapshotRetries)
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"autoconnect_live_snapshot mesh={meshName} live_rows={live.Count} "
                    + $"live_rids={string.Join(',', live.Select(static row => row.Rid.ToString()))}");
                return live;
            }

            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"autoconnect_snapshot_retry mesh={meshName} "
                + $"reason=older_revision attempt={attempt + 1}");
            await Task.Yield();
        }
    }

    internal async ValueTask<ZLinkResolvedSpotLocation?> ResolveSpotRowAsync(
        ZLinkSpotLocationKey key,
        CancellationToken cancellationToken = default)
    {
        var result = await ResolveSpotRowWithStatusAsync(
                key,
                cancellationToken)
            .ConfigureAwait(false);
        return result.Row;
    }

    internal async ValueTask<
        (ZLinkResolvedSpotLocation? Row, ZLinkLocationResolutionKind Kind)>
        ResolveSpotRowWithStatusAsync(
            ZLinkSpotLocationKey key,
            CancellationToken cancellationToken = default)
    {
        var result = await ResolveSpotRowCoreAsync(key, cancellationToken)
            .ConfigureAwait(false);
        return (result.Row, result.Kind);
    }

    internal async ValueTask<ZLinkResolvedActorLocation?> ResolveActorRowAsync(
        ZLinkActorLocationKey key,
        CancellationToken cancellationToken = default)
    {
        var result = await ResolveActorRowWithStatusAsync(key, cancellationToken)
            .ConfigureAwait(false);
        return result.Row;
    }

    internal async ValueTask<
        (ZLinkResolvedActorLocation? Row, ZLinkLocationResolutionKind Kind)>
        ResolveActorRowWithStatusAsync(
            ZLinkActorLocationKey key,
            CancellationToken cancellationToken = default)
    {
        var result = await ResolveActorRowCoreAsync(key, cancellationToken)
            .ConfigureAwait(false);
        return (result.Row, result.Kind);
    }

    /// <summary>Resolve plus live-row presence: callers that retry a transient
    /// resolve window (a claimed-but-unpublished generation-0 row, a lagging
    /// replica view) need to distinguish it from a confirmed miss. A row owned
    /// by an expired process is a miss even while stale storage remains.</summary>
    internal async ValueTask<(ZLinkResolvedActorLocation? Row, bool RowPresent)>
        ResolveActorRowWithPresenceAsync(
            ZLinkActorLocationKey key,
            CancellationToken cancellationToken = default)
    {
        var result = await ResolveActorRowCoreAsync(key, cancellationToken)
            .ConfigureAwait(false);
        return (result.Row, result.LiveRowPresent);
    }

    private async ValueTask<
        (ZLinkResolvedSpotLocation? Row,
            bool LiveRowPresent,
            ZLinkLocationResolutionKind Kind)>
        ResolveSpotRowCoreAsync(
            ZLinkSpotLocationKey key,
            CancellationToken cancellationToken)
    {
        if (TryGetCached(_spotRoutes, key, out var cached))
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"resolve_spot_row spot={key.SpotId} source=cache hit={cached is not null}");
            return (cached, true, ZLinkLocationResolutionKind.Ready);
        }

        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"resolve_spot_row spot={key.SpotId} source=store");
        var authority = await ZLinkLocationStoreRead.ExecuteAsync(
            _health,
            "ZLinkSpotLocation-resolver-read",
            cancellationToken,
            storeToken => _store.ReadAuthorityAsync(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(key.SpotId),
                storeToken)).ConfigureAwait(false);
        var raw = ProjectSpot(authority);
        var (row, liveRowPresent) = await _liveRows.ResolveWithPresenceAsync(
            raw,
            static row => row.OwnerId,
            row => _observed.AcceptSpot(row),
            cancellationToken).ConfigureAwait(false);
        // A missing row and a row whose owner lease expired both end the
        // incarnation. Storage can retain the expired row until a successor
        // claims it, so raw presence alone must not preserve the old floor.
        // A live but older replica row still reports LiveRowPresent=true and
        // therefore cannot reset the floor.
        if (!liveRowPresent) _observed.ForgetSpot(key);
        if (row is null)
        {
            InvalidateSpotRoute(key);
            return (
                null,
                liveRowPresent,
                raw is null
                    ? ZLinkLocationResolutionKind.Missing
                    : ZLinkLocationResolutionKind.KnownUnavailable);
        }

        if (!await AdmitAndCacheReadyRouteAsync(
                _spotRoutes,
                key,
                row,
                authority,
                cancellationToken)
            .ConfigureAwait(false))
        {
            _observed.ForgetSpot(key);
            InvalidateSpotRoute(key);
            return (
                null,
                liveRowPresent,
                ZLinkLocationResolutionKind.KnownUnavailable);
        }
        return (row, liveRowPresent, ZLinkLocationResolutionKind.Ready);
    }

    private async ValueTask<
        (ZLinkResolvedActorLocation? Row,
            bool LiveRowPresent,
            ZLinkLocationResolutionKind Kind)>
        ResolveActorRowCoreAsync(
            ZLinkActorLocationKey key,
            CancellationToken cancellationToken)
    {
        if (TryGetCached(_actorRoutes, key, out var cached))
            return (cached, true, ZLinkLocationResolutionKind.Ready);

        var authority = await ZLinkLocationStoreRead.ExecuteAsync(
            _health,
            "ZLinkActorLocation-resolver-read",
            cancellationToken,
            storeToken => _store.ReadAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(key.ActorId),
                storeToken)).ConfigureAwait(false);
        var raw = ProjectActor(authority);
        var (row, liveRowPresent) = await _liveRows.ResolveWithPresenceAsync(
            raw,
            static row => row.OwnerId,
            // Reference generation 0 marks a claimed-but-unpublished actor:
            // the claim precedes activation, so such a row is never a
            // resolvable reference (40-location-runtime §6).
            row => row.ActorRef.ObjectGeneration > 0 && _observed.AcceptActor(row),
            cancellationToken).ConfigureAwait(false);
        // An expired owner ends the incarnation even when its stale row remains
        // in storage. Forget the old membership/generation floor so the next
        // owner can publish its fresh per-instance axes. Do not forget for a
        // live lagging replica row: LiveRowPresent remains true in that case.
        if (!liveRowPresent) _observed.ForgetActor(key);
        if (row is null)
        {
            InvalidateActorRoute(key);
            return (
                null,
                liveRowPresent,
                raw is null
                    ? ZLinkLocationResolutionKind.Missing
                    : ZLinkLocationResolutionKind.KnownUnavailable);
        }

        if (!await AdmitAndCacheReadyRouteAsync(
                _actorRoutes,
                key,
                row,
                authority,
                cancellationToken)
            .ConfigureAwait(false))
        {
            _observed.ForgetActor(key);
            InvalidateActorRoute(key);
            return (
                null,
                liveRowPresent,
                ZLinkLocationResolutionKind.KnownUnavailable);
        }
        return (row, liveRowPresent, ZLinkLocationResolutionKind.Ready);
    }

    internal void InvalidateSpotRoute(ZLinkSpotLocationKey key)
    {
        lock (_routeCacheGate)
            _spotRoutes.Remove(key);
    }

    internal void InvalidateActorRoute(ZLinkActorLocationKey key)
    {
        lock (_routeCacheGate)
            _actorRoutes.Remove(key);
    }

    internal bool InvalidateMessageFollowRoute(
        ZLinkServiceWireCodec.MessageFollowRecord record)
    {
        var source = record.Source;
        var key = source.IsActor
            ? new ZLinkActorLocationKey(source.ObjectId)
            : default;
        lock (_routeCacheGate)
        {
            if (source.IsActor)
            {
                if (!_actorRoutes.TryGetValue(key, out var cached))
                    return false;
                var row = cached.Row;
                if (row.ActorId != source.ObjectId
                    || row.ActorRef.ObjectGeneration != source.ObjectGeneration
                    || row.OwnerNodeRid != source.TargetNodeRid
                    || row.ActorRef.NodeRid != source.TargetNodeRid
                    || row.OwnerNodeGeneration != source.TargetNodeGeneration
                    || row.AuthorityOwnerGeneration
                       != source.AuthorityOwnerGeneration
                    || row.LeaseGeneration <= 0
                    || (ulong)row.LeaseGeneration != source.OwnerLeaseGeneration)
                    return false;
                return _actorRoutes.Remove(key);
            }

            var spotKey = new ZLinkSpotLocationKey(source.ObjectId);
            if (!_spotRoutes.TryGetValue(spotKey, out var spotCached))
                return false;
            var spot = spotCached.Row;
            if (spot.SpotId != source.ObjectId
                || spot.SpotGeneration != source.ObjectGeneration
                || spot.OwnerNodeRid != source.TargetNodeRid
                || spot.OwnerNodeGeneration != source.TargetNodeGeneration
                || spot.AuthorityOwnerGeneration
                   != source.AuthorityOwnerGeneration
                || spot.LeaseGeneration <= 0
                || (ulong)spot.LeaseGeneration != source.OwnerLeaseGeneration)
                return false;
            return _spotRoutes.Remove(spotKey);
        }
    }

    internal bool HasCachedActorRoute(ZLinkActorLocationKey key)
    {
        lock (_routeCacheGate)
            return _actorRoutes.ContainsKey(key);
    }

    private bool TryGetCached<TKey, TRow>(
        Dictionary<TKey, CachedRoute<TRow>> routes,
        TKey key,
        out TRow? row)
        where TKey : notnull
        where TRow : class
    {
        lock (_routeCacheGate)
        {
            if (!routes.TryGetValue(key, out var route))
            {
                row = null;
                return false;
            }

            var cacheAge = _time.GetElapsedTime(route.StoredAt);
            var ownerLeaseAge = _time.GetElapsedTime(
                route.OwnerLeaseLifetimeMeasuredAt);
            if (cacheAge >= route.MaxAge
                || ownerLeaseAge >= route.OwnerLeaseLifetime
                || (_health is not null
                    && route.StoreRecoveryGeneration != _health.RecoveryGeneration))
            {
                routes.Remove(key);
                row = null;
                return false;
            }

            row = route.Row;
            return true;
        }
    }

    private async ValueTask<bool> AdmitAndCacheReadyRouteAsync<TKey, TRow>(
        Dictionary<TKey, CachedRoute<TRow>> routes,
        TKey key,
        TRow row,
        ZLinkAuthorityReadResult authority,
        CancellationToken cancellationToken)
        where TKey : notnull
        where TRow : class
    {
        if (authority is not ZLinkAuthorityReadResult.Found found)
            return false;

        // Measure the lease lifetime from before the asynchronous read. If the
        // continuation is delayed, this makes cache expiry earlier rather than
        // extending it past the owner's admission deadline.
        var ownerLeaseLifetimeMeasuredAt = _time.GetTimestamp();
        var remaining = await _leaseTracker.GetOwnerTokenRemainingAdmissionLifetimeAsync(
                new ZLinkLocationOwnerToken(
                    found.Snapshot.OwnerId,
                    found.Snapshot.OwnerLeaseGeneration),
                cancellationToken)
            .ConfigureAwait(false);
        if (remaining is not { } leaseLifetime) return false;

        var maxAge = _options.RouteCacheMaxAge;
        if (maxAge <= TimeSpan.Zero) return true;

        var route = new CachedRoute<TRow>(
            row,
            found.Snapshot.StoreVersion,
            found.Snapshot.ObjectGeneration,
            found.Snapshot.AuthorityOwnerGeneration,
            found.Snapshot.OwnerLeaseGeneration,
            _time.GetTimestamp(),
            maxAge,
            ownerLeaseLifetimeMeasuredAt,
            leaseLifetime,
            _health?.RecoveryGeneration ?? 0);
        lock (_routeCacheGate)
            routes[key] = route;
        return true;
    }

    private static ZLinkResolvedSpotLocation? ProjectSpot(
        ZLinkAuthorityReadResult authority)
    {
        if (authority is not ZLinkAuthorityReadResult.Found found)
        {
            //  "No row in the store" and "row present but rejected" are
            //  different failures that both surfaced as the same bare null.
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"project_spot_no_authority result={authority.GetType().Name}");
            return null;
        }
        var snapshot = found.Snapshot;
        var userDecoded = ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
            snapshot.Payload.Span,
            out var user);
        if (userDecoded)
            //  A user-spot row that decodes but fails a guard used to vanish as
            //  a bare null, which reads the same as "no row at all" at the
            //  caller. Name the values the guards compare.
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"project_user_spot spot={user.SpotId} state={user.State} "
                + $"payload_owner={user.OwnerId} snapshot_owner={snapshot.OwnerId} "
                + $"payload_lease={user.OwnerLeaseGeneration} "
                + $"snapshot_lease={snapshot.OwnerLeaseGeneration}");
        if (userDecoded
            && user.State == ZLinkUserSpotAuthorityState.Ready
            && user.OwnerId == snapshot.OwnerId
            && snapshot.OwnerLeaseGeneration > 0
            && user.OwnerLeaseGeneration
               == (ulong)snapshot.OwnerLeaseGeneration)
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
                snapshot.AuthorityOwnerGeneration);
        if (ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var instance)
            && instance.State == ZLinkInstanceSpotAuthorityState.Ready
            && instance.OwnerId == snapshot.OwnerId
            && snapshot.OwnerLeaseGeneration > 0
            && instance.OwnerLeaseGeneration
               == (ulong)snapshot.OwnerLeaseGeneration)
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
                snapshot.AuthorityOwnerGeneration);
        if (!TryReadCommittedCanonicalTarget(
                snapshot,
                out var canonical,
                out var targetNodeRid))
            return null;
        if (ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                canonical.SteadyAuthorityPayload.Span,
                out user)
            && user.State == ZLinkUserSpotAuthorityState.Ready
            && user.MeshName == snapshot.Allocation.Descriptor.MeshName)
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
                snapshot.AuthorityOwnerGeneration);
        if (ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                canonical.SteadyAuthorityPayload.Span,
                out instance)
            && instance.State == ZLinkInstanceSpotAuthorityState.Ready
            && instance.MeshName == snapshot.Allocation.Descriptor.MeshName)
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
                snapshot.AuthorityOwnerGeneration);
        return null;
    }

    private static ZLinkResolvedActorLocation? ProjectActor(
        ZLinkAuthorityReadResult authority)
    {
        if (authority is not ZLinkAuthorityReadResult.Found found)
            return null;
        var snapshot = found.Snapshot;
        if (!ZLinkActorAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var actor)
            || actor.State != ZLinkActorAuthorityState.Ready
            || actor.OwnerId != snapshot.OwnerId
            || snapshot.OwnerLeaseGeneration <= 0
            || actor.OwnerLeaseGeneration
               != (ulong)snapshot.OwnerLeaseGeneration)
        {
            if (!TryReadCommittedCanonicalTarget(
                    snapshot,
                    out var canonical,
                    out var targetNodeRid)
                || !ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                    canonical.SteadyAuthorityPayload.Span,
                    out actor)
                || actor.State != ZLinkActorAuthorityState.Ready
                || actor.MeshName
                   != snapshot.Allocation.Descriptor.MeshName)
                return null;
            return new ZLinkResolvedActorLocation(
                actor.MeshName,
                actor.ActorId,
                actor.StableType,
                new ActorRef(
                    actor.ActorId,
                    snapshot.ObjectGeneration,
                    actor.MeshName,
                    targetNodeRid),
                targetNodeRid,
                canonical.State.TargetNodeGeneration,
                actor.CurrentSpotId,
                actor.CurrentSpotGeneration,
                actor.CurrentSpotKind,
                snapshot.AuthorityOwnerGeneration,
                snapshot.OwnerId,
                snapshot.OwnerLeaseGeneration,
                snapshot.StoreNow,
                snapshot.AuthorityOwnerGeneration);
        }
        return new ZLinkResolvedActorLocation(
            actor.MeshName,
            actor.ActorId,
            actor.StableType,
            new ActorRef(
                actor.ActorId,
                snapshot.ObjectGeneration,
                actor.MeshName,
                actor.NodeRid),
            actor.NodeRid,
            actor.NodeGeneration,
            actor.CurrentSpotId,
            actor.CurrentSpotGeneration,
            actor.CurrentSpotKind,
            snapshot.AuthorityOwnerGeneration,
            snapshot.OwnerId,
            snapshot.OwnerLeaseGeneration,
            snapshot.StoreNow,
            snapshot.AuthorityOwnerGeneration);
    }

    private static bool TryReadCommittedCanonicalTarget(
        ZLinkAuthoritySnapshot snapshot,
        out ZLinkCanonicalRelocationAuthorityProjection canonical,
        out RoutingId targetNodeRid)
    {
        canonical = null!;
        targetNodeRid = default;
        if (snapshot.OwnerLeaseGeneration <= 0
            || !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                snapshot.Payload.Span,
                out canonical)
            || canonical.Phase is < 4 or > 8
            || canonical.TargetOwnerId != snapshot.OwnerId
            || canonical.TargetOwnerLeaseGeneration
               != (ulong)snapshot.OwnerLeaseGeneration
            || canonical.State.TargetNodeGeneration
               != snapshot.Allocation.DescriptorLifecycleGeneration)
            return false;
        try
        {
            targetNodeRid = RoutingId.FromHex(
                canonical.State.TargetNodeRid);
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
        long StoreRecoveryGeneration)
        where TRow : class;
}
