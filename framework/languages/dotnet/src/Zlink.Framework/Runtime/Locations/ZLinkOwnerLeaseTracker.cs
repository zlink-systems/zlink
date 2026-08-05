namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Reads exact owner tokens for descriptor admission. The provider does not
/// expose a global lease list; each descriptor supplies the owner identity
/// that must be fenced.
/// </summary>
internal sealed class ZLinkOwnerLeaseTracker
{
    private readonly IZLinkLocationRepository _store;
    private readonly ZLinkLocationOptions _options;
    private readonly TimeProvider _time;
    private readonly ZLinkLocationStoreHealth? _health;
    private readonly object _cacheGate = new();
    private readonly Dictionary<string, Snapshot> _cache =
        new(StringComparer.Ordinal);
    internal TimeProvider TimeProvider => _time;

    internal ZLinkOwnerLeaseTracker(
        IZLinkLocationRepository store,
        ZLinkLocationOptions options,
        TimeProvider? timeProvider = null,
        ZLinkLocationStoreHealth? health = null)
    {
        _store = store;
        _options = options;
        _time = timeProvider ?? TimeProvider.System;
        _health = health;
    }

    internal async ValueTask<bool> IsOwnerLiveAsync(
        string ownerId,
        CancellationToken cancellationToken = default)
    {
        var (snapshot, fromCache) = await GetSnapshotAsync(ownerId, cancellationToken)
            .ConfigureAwait(false);
        if (snapshot.Token is not null)
            return IsUnexpired(snapshot);

        // A cached "no lease" answer must not decide a caller that treats the
        // result as terminal. An owner that has just published its lease still
        // reads as missing for the rest of the polling interval, and callers
        // like the remote join target resolver turn that into a NotFound they
        // never retry. Confirm a negative against the store once before it is
        // allowed to stand. Positive answers keep using the cache.
        if (!fromCache) return false;
        var confirmed = await RefreshSnapshotAsync(ownerId, cancellationToken)
            .ConfigureAwait(false);
        return confirmed.Token is not null && IsUnexpired(confirmed);
    }

    internal async ValueTask<bool> IsOwnerTokenLiveAsync(
        ZLinkLocationOwnerToken token,
        CancellationToken cancellationToken = default)
    {
        var (snapshot, fromCache) = await GetSnapshotAsync(token.OwnerId, cancellationToken)
            .ConfigureAwait(false);
        if (snapshot.Token == token) return IsUnexpired(snapshot);
        if (!fromCache) return false;
        var confirmed = await RefreshSnapshotAsync(token.OwnerId, cancellationToken)
            .ConfigureAwait(false);
        return confirmed.Token == token && IsUnexpired(confirmed);
    }

    /// <summary>
    /// Returns the conservative time left before the owner must stop accepting
    /// new work. The lease can remain present after this point, but the
    /// fencing margin is reserved for the owner to seal admission before the
    /// provider expiry boundary.
    /// </summary>
    internal async ValueTask<TimeSpan?> GetOwnerTokenRemainingAdmissionLifetimeAsync(
        ZLinkLocationOwnerToken token,
        CancellationToken cancellationToken = default)
    {
        var (snapshot, _) = await GetSnapshotAsync(token.OwnerId, cancellationToken)
            .ConfigureAwait(false);
        if (snapshot.Token != token) return null;
        var remaining = snapshot.LeaseExpiresAt - snapshot.StoreNow
                        - _time.GetElapsedTime(snapshot.FetchedAt)
                        - _options.OwnerLeaseFencingMargin;
        return remaining > TimeSpan.Zero ? remaining : null;
    }

    private async ValueTask<(Snapshot Snapshot, bool FromCache)> GetSnapshotAsync(
        string ownerId,
        CancellationToken cancellationToken)
    {
        Snapshot? current;
        lock (_cacheGate)
            _cache.TryGetValue(ownerId, out current);
        if (current is not null
            && _time.GetElapsedTime(current.FetchedAt) < _options.PollingInterval)
        {
            return (current, true);
        }

        return (await RefreshSnapshotAsync(ownerId, cancellationToken)
            .ConfigureAwait(false), false);
    }

    private async ValueTask<Snapshot> RefreshSnapshotAsync(
        string ownerId,
        CancellationToken cancellationToken)
    {
        var fetchedAt = _time.GetTimestamp();
        var read = await ZLinkLocationStoreRead.ExecuteAsync(
                _health,
                "owner-lease-read",
                cancellationToken,
                storeToken => _store.ReadOwnerLeaseAsync(ownerId, storeToken))
            .ConfigureAwait(false);
        var refreshed = read switch
        {
            ZLinkOwnerLeaseReadResult.Found found => new Snapshot(
                found.Token,
                found.LeaseExpiresAt,
                found.StoreNow,
                fetchedAt),
            ZLinkOwnerLeaseReadResult.Missing => new Snapshot(
                null,
                DateTimeOffset.MinValue,
                DateTimeOffset.MinValue,
                fetchedAt),
            _ => throw new ArgumentOutOfRangeException(nameof(read))
        };
        lock (_cacheGate)
            _cache[ownerId] = refreshed;
        return refreshed;
    }

    private bool IsUnexpired(Snapshot snapshot) =>
        snapshot.LeaseExpiresAt - snapshot.StoreNow
        - _time.GetElapsedTime(snapshot.FetchedAt) > TimeSpan.Zero;

    private sealed record Snapshot(
        ZLinkLocationOwnerToken? Token,
        DateTimeOffset LeaseExpiresAt,
        DateTimeOffset StoreNow,
        long FetchedAt);
}
