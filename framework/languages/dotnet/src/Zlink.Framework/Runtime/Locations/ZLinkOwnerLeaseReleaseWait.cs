using System.Collections.Concurrent;

namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Remembers the first local monotonic observation of an invalid authority owner token.
/// Fixed RID claims keep a separate descriptor-scoped observation.
/// </summary>
internal sealed class ZLinkOwnerLeaseReleaseWait(TimeProvider timeProvider)
{
    private readonly ConcurrentDictionary<string, Observation> _observations = new(
        StringComparer.Ordinal
    );

    private readonly ConcurrentDictionary<ZLinkLocationOwnerToken, long> _authorityObservations =
        new();

    internal int Count => _observations.Count + _authorityObservations.Count;

    internal bool HasElapsed(ZLinkLocationOwnerToken owner, TimeSpan fencingMargin) =>
        fencingMargin <= TimeSpan.Zero || HasElapsedSince(ObserveAuthority(owner), fencingMargin);

    internal void Observe(ZLinkLocationOwnerToken owner) => _ = ObserveAuthority(owner);

    private long ObserveAuthority(ZLinkLocationOwnerToken owner) =>
        _authorityObservations.GetOrAdd(
            owner,
            static (_, time) => time.GetTimestamp(),
            timeProvider
        );

    internal bool HasElapsed(
        string recordKey,
        ZLinkLocationOwnerToken? owner,
        TimeSpan fencingMargin
    )
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(recordKey);
        if (fencingMargin <= TimeSpan.Zero)
            return true;

        return HasElapsedSince(ObserveCore(recordKey, owner).Timestamp, fencingMargin);
    }

    private bool HasElapsedSince(long firstObserved, TimeSpan fencingMargin)
    {
        var elapsed = timeProvider.GetElapsedTime(firstObserved, timeProvider.GetTimestamp());
        return elapsed >= fencingMargin
            && elapsed - fencingMargin >= fencingMargin
            && elapsed - fencingMargin - fencingMargin >= fencingMargin;
    }

    internal void ForgetIfTokenMatches(string recordKey, ZLinkLocationOwnerToken? owner)
    {
        if (_observations.TryGetValue(recordKey, out var observed) && observed.Owner == owner)
            _observations.TryRemove(new KeyValuePair<string, Observation>(recordKey, observed));
    }

    internal void ForgetIfTokenChanged(string recordKey, ZLinkLocationOwnerToken? owner)
    {
        if (_observations.TryGetValue(recordKey, out var observed) && observed.Owner != owner)
            _observations.TryRemove(new KeyValuePair<string, Observation>(recordKey, observed));
    }

    private Observation ObserveCore(string recordKey, ZLinkLocationOwnerToken? owner)
    {
        while (true)
        {
            if (!_observations.TryGetValue(recordKey, out var observed))
            {
                var created = new Observation(owner, timeProvider.GetTimestamp());
                if (_observations.TryAdd(recordKey, created))
                    return created;
                continue;
            }

            if (observed.Owner == owner)
                return observed;

            var updated = new Observation(owner, timeProvider.GetTimestamp());
            if (_observations.TryUpdate(recordKey, updated, observed))
                return updated;
        }
    }

    private sealed class Observation(ZLinkLocationOwnerToken? owner, long timestamp)
    {
        internal ZLinkLocationOwnerToken? Owner { get; } = owner;

        internal long Timestamp { get; } = timestamp;
    }
}
