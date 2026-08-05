namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Public messaging lookup surfaces: global SpotId or ActorId to a full
/// spot address. The authoritative row supplies MeshName; direct lookup
/// never requires the caller to select a Mesh. The actor lookup derives the address from the actor
/// row's spot kind — the
/// entry spot address is the node itself. The returned opaque handle keeps
/// its logical lookup key and receives location-event updates without
/// exposing address refresh policy to callers.
/// </summary>
internal sealed class ZLinkLocationAddressResolvers
{
    private readonly ZLinkStoreLocationResolvers _rows;
    private readonly ZLinkSpotHandleRegistry? _handles;

    internal ZLinkLocationAddressResolvers(ZLinkStoreLocationResolvers rows)
    {
        _rows = rows;
    }

    internal ZLinkLocationAddressResolvers(
        ZLinkStoreLocationResolvers rows,
        ZLinkSpotHandleRegistry handles)
        : this(rows)
    {
        _handles = handles;
    }

    internal async ValueTask<ZLinkResolvedSpotHandle?> ResolveSpotHandleAsync(
        string spotId,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(spotId);
        var key = new ZLinkSpotLocationKey(spotId);
        var resolution = await _rows.ResolveSpotRowWithStatusAsync(
                key,
                cancellationToken)
            .ConfigureAwait(false);
        ThrowIfKnownUnavailable(resolution.Kind, $"Spot '{spotId}'");
        var row = resolution.Row;
        if (row is null) return null;
        var handle = new ZLinkResolvedSpotHandle(
            ToSnapshot(row),
            row.SpotGeneration,
            ct => RefreshSpotAsync(key, ct),
            () => _rows.InvalidateSpotRoute(key));
        _handles?.RegisterSpot(key, handle);
        return handle;
    }

    internal async ValueTask<ZLinkResolvedSpotHandle?> ResolveActorSpotHandleAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        var key = new ZLinkActorLocationKey(actorId);
        var resolution = await _rows.ResolveActorRowWithStatusAsync(
                key,
                cancellationToken)
            .ConfigureAwait(false);
        ThrowIfKnownUnavailable(resolution.Kind, $"Actor '{actorId}'");
        var row = resolution.Row;
        if (row is null)
        {
            return null;
        }

        // Entry actors use their node's Entry Spot snapshot; actors in a user
        // Spot use that Spot's current snapshot. Membership epoch orders the
        // handle updates because the addressed spot generation resets when an
        // actor moves back to the entry spot.
        var handle = new ZLinkResolvedSpotHandle(
            ToSnapshot(row),
            row.MembershipEpoch,
            ct => RefreshActorAsync(key, ct),
            () => _rows.InvalidateActorRoute(key));
        _handles?.RegisterActor(key, handle);
        return handle;
    }

    private async ValueTask<(ZLinkSpotHandleSnapshot Snapshot, ulong Version)?> RefreshSpotAsync(
        ZLinkSpotLocationKey key,
        CancellationToken cancellationToken)
    {
        var resolution = await _rows.ResolveSpotRowWithStatusAsync(
                key,
                cancellationToken)
            .ConfigureAwait(false);
        ThrowIfKnownUnavailable(resolution.Kind, $"Spot '{key.SpotId}'");
        return resolution.Row is not { } row
            ? null
            : (ToSnapshot(row), row.SpotGeneration);
    }

    private async ValueTask<(ZLinkSpotHandleSnapshot Snapshot, ulong Version)?> RefreshActorAsync(
        ZLinkActorLocationKey key,
        CancellationToken cancellationToken)
    {
        var resolution = await _rows.ResolveActorRowWithStatusAsync(
                key,
                cancellationToken)
            .ConfigureAwait(false);
        ThrowIfKnownUnavailable(resolution.Kind, $"Actor '{key.ActorId}'");
        return resolution.Row is not { } row
            ? null
            : (ToSnapshot(row), row.MembershipEpoch);
    }

    private static void ThrowIfKnownUnavailable(
        ZLinkLocationResolutionKind kind,
        string target)
    {
        if (kind != ZLinkLocationResolutionKind.KnownUnavailable)
            return;
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Unavailable,
            $"{target} is currently unavailable.");
    }

    private ZLinkSpotHandleSnapshot ToSnapshot(ZLinkResolvedSpotLocation row)
        => new(
            row.MeshName,
            row.OwnerNodeRid,
            row.SpotId,
            row.SpotGeneration,
            row.SpotKind,
            row.AuthorityOwnerGeneration,
            row.OwnerNodeGeneration,
            checked((ulong)row.LeaseGeneration));

    internal ZLinkSpotHandleSnapshot ToSnapshot(ZLinkResolvedActorLocation row)
        => row.SpotKind == ZLinkSpotKind.Entry || string.IsNullOrEmpty(row.SpotId)
            ? new ZLinkSpotHandleSnapshot(
                row.MeshName,
                row.OwnerNodeRid,
                row.SpotId,
                row.SpotGeneration,
                ZLinkSpotKind.Entry,
                row.AuthorityOwnerGeneration,
                row.OwnerNodeGeneration,
                checked((ulong)row.LeaseGeneration))
            : new ZLinkSpotHandleSnapshot(
                row.MeshName,
                row.OwnerNodeRid,
                row.SpotId,
                row.SpotGeneration,
                ZLinkSpotKind.User,
                row.AuthorityOwnerGeneration,
                row.OwnerNodeGeneration,
                checked((ulong)row.LeaseGeneration));
}

internal readonly record struct ZLinkSpotHandleSnapshot(
    string RouterChannelId,
    RoutingId NodeRid,
    string SpotId,
    ulong Generation,
    ZLinkSpotKind SpotKind = ZLinkSpotKind.User,
    ulong AuthorityOwnerGeneration = 0,
    ulong NodeGeneration = 0,
    ulong OwnerLeaseGeneration = 0);

internal sealed class ZLinkResolvedSpotHandle
{
    private readonly Func<CancellationToken, ValueTask<(ZLinkSpotHandleSnapshot Snapshot, ulong Version)?>> _refresh;
    private readonly Action? _invalidateRoute;
    private readonly object _gate = new();
    private ZLinkHandleAvailability _availability = ZLinkHandleAvailability.Available;
    private ulong _version;
    private ZLinkSpotHandleSnapshot _snapshot;

    internal ZLinkResolvedSpotHandle(
        ZLinkSpotHandleSnapshot initialSnapshot,
        ulong version,
        Func<CancellationToken, ValueTask<(ZLinkSpotHandleSnapshot Snapshot, ulong Version)?>> refresh,
        Action? invalidateRoute = null)
    {
        _refresh = refresh;
        _invalidateRoute = invalidateRoute;
        _snapshot = initialSnapshot;
        _version = version;
    }

    internal ZLinkSpotHandleSnapshot Snapshot
    {
        get
        {
            lock (_gate)
            {
                if (_availability != ZLinkHandleAvailability.Available)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        "The resolved spot handle is no longer available.");
                return _snapshot;
            }
        }
    }

    internal string MeshName { get { lock (_gate) return _snapshot.RouterChannelId; } }

    internal string SpotId { get { lock (_gate) return _snapshot.SpotId; } }

    internal void Update(ZLinkSpotHandleSnapshot snapshot, ulong version)
    {
        lock (_gate)
        {
            if (version < _version
                || (version == _version
                    && _availability == ZLinkHandleAvailability.Removed)) return;
            _snapshot = snapshot;
            _version = version;
            _availability = ZLinkHandleAvailability.Available;
        }
    }

    internal void Invalidate(ulong version)
    {
        lock (_gate)
        {
            if (version < _version) return;
            _version = version;
            _availability = ZLinkHandleAvailability.Removed;
        }
    }

    /// <summary>Invalidates at the current version: a later update with a
    /// strictly newer version resurrects the handle, a stale replay of the
    /// same version does not.</summary>
    internal void InvalidateCurrent()
    {
        lock (_gate)
        {
            _availability = ZLinkHandleAvailability.Removed;
        }
    }

    internal async ValueTask<bool> RefreshAsync(CancellationToken cancellationToken)
    {
        var refreshed = await _refresh(cancellationToken).ConfigureAwait(false);
        if (refreshed is not { } current) return false;
        Update(current.Snapshot, current.Version);
        return true;
    }

    internal void InvalidateRoute() => _invalidateRoute?.Invoke();
}

internal enum ZLinkHandleAvailability
{
    Available,
    Removed
}

internal static class ZLinkSpotHandleRequestExecution
{
    internal static async ValueTask<T> ExecuteAsync<T>(
        ZLinkResolvedSpotHandle handle,
        Func<ZLinkSpotHandleSnapshot, ValueTask<T>> operation,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        // The target may already have accepted the operation before a stale
        // response is observed. Retrying this operation against a freshly
        // resolved owner could therefore execute it twice. A later call may
        // use a refreshed handle; this call keeps its original route.
        try
        {
            return await operation(handle.Snapshot).ConfigureAwait(false);
        }
        catch (ZLinkFrameworkException error) when (IsStaleRoute(error))
        {
            handle.InvalidateRoute();
            throw;
        }
    }

    internal static bool IsStaleRoute(ZLinkFrameworkException error) =>
        error.Kind is ZLinkFrameworkErrorKind.NotFound
            or ZLinkFrameworkErrorKind.Unavailable;
}
