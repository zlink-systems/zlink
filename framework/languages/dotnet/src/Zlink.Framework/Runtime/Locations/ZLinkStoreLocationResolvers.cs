using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

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
    private readonly ZLinkLiveLocationRows _liveRows;
    private readonly ZLinkLocationStoreHealth? _health;
    private readonly ZLinkOwnerLeaseTracker _leaseTracker;
    private readonly ZLinkLocationOptions _options;
    private readonly TimeProvider _time;
    private readonly ZLinkStateLane _lane = new();
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
        TimeProvider? timeProvider = null
    )
    {
        _store = store;
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
        return live;
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
        ZLinkLocationResolutionKind Kind
    )> ResolveSpotRowWithStatusAsync(
        ZLinkSpotLocationKey key,
        CancellationToken cancellationToken = default
    )
    {
        var result = await ResolveSpotRowCoreAsync(key, cancellationToken).ConfigureAwait(false);
        return (result.Row, result.Kind);
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
        ZLinkLocationResolutionKind Kind
    )> ResolveSpotRowCoreAsync(ZLinkSpotLocationKey key, CancellationToken cancellationToken)
    {
        var cached = await TryGetCachedAsync(_spotRoutes, key).ConfigureAwait(false);
        if (cached.Found)
        {
            if (ZLinkFrameworkDebugLog.SpotDiscoveryEnabled)
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"resolve_spot_row spot={key.SpotId} source=cache hit={cached.Row is not null}"
                );
            return (cached.Row, true, ZLinkLocationResolutionKind.Ready);
        }

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
        var raw = ProjectSpot(authority);
        var (row, liveRowPresent) = await _liveRows
            .ResolveWithPresenceAsync(raw, static row => row.OwnerId, cancellationToken)
            .ConfigureAwait(false);
        if (row is null)
        {
            InvalidateSpotRoute(key);
            return (
                null,
                liveRowPresent,
                raw is not null ? ZLinkLocationResolutionKind.KnownUnavailable
                : IsClosingSpot(authority) ? ZLinkLocationResolutionKind.Closing
                : ZLinkLocationResolutionKind.Missing
            );
        }

        if (IsClosingSpot(authority))
        {
            InvalidateSpotRoute(key);
            var remaining = await _leaseTracker
                .GetOwnerTokenRemainingAdmissionLifetimeAsync(
                    new ZLinkLocationOwnerToken(row.OwnerId, row.LeaseGeneration),
                    cancellationToken
                )
                .ConfigureAwait(false);
            return remaining is null
                ? (null, liveRowPresent, ZLinkLocationResolutionKind.KnownUnavailable)
                : (row, liveRowPresent, ZLinkLocationResolutionKind.Closing);
        }

        if (
            !await AdmitAndCacheReadyRouteAsync(_spotRoutes, key, row, authority, cancellationToken)
                .ConfigureAwait(false)
        )
        {
            InvalidateSpotRoute(key);
            return (null, liveRowPresent, ZLinkLocationResolutionKind.KnownUnavailable);
        }
        return (row, liveRowPresent, ZLinkLocationResolutionKind.Ready);
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

        var authority = await ZLinkLocationStoreRead
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
        var raw = ProjectActor(authority, key.ActorId);
        var (row, liveRowPresent) = await _liveRows
            .ResolveWithPresenceAsync(raw, static row => row.OwnerId, cancellationToken)
            .ConfigureAwait(false);
        // Reference generation 0 marks a claimed-but-unpublished actor:
        // the claim precedes activation, so such a row is never a
        // resolvable reference (40-location-runtime §6).
        if (row is not null)
            row = row.ActorRef.ObjectGeneration > 0 ? row : null;
        if (row is null)
        {
            InvalidateActorRoute(key);
            return (
                null,
                liveRowPresent,
                raw is null
                    ? ZLinkLocationResolutionKind.Missing
                    : ZLinkLocationResolutionKind.KnownUnavailable
            );
        }

        if (
            !await AdmitAndCacheReadyRouteAsync(
                    _actorRoutes,
                    key,
                    row,
                    authority,
                    cancellationToken
                )
                .ConfigureAwait(false)
        )
        {
            InvalidateActorRoute(key);
            return (null, liveRowPresent, ZLinkLocationResolutionKind.KnownUnavailable);
        }
        return (row, liveRowPresent, ZLinkLocationResolutionKind.Ready);
    }

    internal void InvalidateSpotRoute(ZLinkSpotLocationKey key)
    {
        AwaitStateLane(_lane.RunAsync(() => _spotRoutes.Remove(key)));
    }

    internal void InvalidateActorRoute(ZLinkActorLocationKey key)
    {
        AwaitStateLane(_lane.RunAsync(() => _actorRoutes.Remove(key)));
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
            var ownerLeaseAge = _time.GetElapsedTime(route.OwnerLeaseLifetimeMeasuredAt);
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

    private async ValueTask<bool> AdmitAndCacheReadyRouteAsync<TKey, TRow>(
        Dictionary<TKey, CachedRoute<TRow>> routes,
        TKey key,
        TRow row,
        ZLinkAuthorityReadResult authority,
        CancellationToken cancellationToken
    )
        where TKey : notnull
        where TRow : class
    {
        if (authority is not ZLinkAuthorityReadResult.Found found)
            return false;

        // Measure the lease lifetime from before the asynchronous read. If the
        // continuation is delayed, this makes cache expiry earlier rather than
        // extending it past the owner's admission deadline.
        var ownerLeaseLifetimeMeasuredAt = _time.GetTimestamp();
        var remaining = await _leaseTracker
            .GetOwnerTokenRemainingAdmissionLifetimeAsync(
                new ZLinkLocationOwnerToken(
                    found.Snapshot.OwnerId,
                    found.Snapshot.OwnerLeaseGeneration
                ),
                cancellationToken
            )
            .ConfigureAwait(false);
        if (remaining is not { } leaseLifetime)
            return false;

        var maxAge = _options.RouteCacheMaxAge;
        if (maxAge <= TimeSpan.Zero)
            return true;

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
            _health?.RecoveryGeneration ?? 0
        );
        await _lane.RunAsync(() => routes[key] = route).ConfigureAwait(false);
        return true;
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

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
            || !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                snapshot.Payload.Span,
                out canonical
            )
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
