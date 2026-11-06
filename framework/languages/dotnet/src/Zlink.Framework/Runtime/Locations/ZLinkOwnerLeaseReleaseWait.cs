using System.Collections.Concurrent;

namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Remembers the first local monotonic observation of an invalid owner token
/// for one Location record. The observation is process-local by design.
/// </summary>
internal sealed class ZLinkOwnerLeaseReleaseWait(TimeProvider timeProvider)
{
    private readonly ConcurrentDictionary<string, Observation> _observations = new(
        StringComparer.Ordinal
    );

    internal int Count => _observations.Count;

    internal bool HasElapsed(
        string recordKey,
        ZLinkLocationOwnerToken? owner,
        TimeSpan fencingMargin
    )
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(recordKey);
        if (fencingMargin <= TimeSpan.Zero)
            return true;

        var observed = ObserveCore(recordKey, owner);
        var elapsed = timeProvider.GetElapsedTime(observed.Timestamp, timeProvider.GetTimestamp());
        return elapsed >= fencingMargin
            && elapsed - fencingMargin >= fencingMargin
            && elapsed - fencingMargin - fencingMargin >= fencingMargin;
    }

    internal void Observe(string recordKey, ZLinkLocationOwnerToken? owner)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(recordKey);
        _ = ObserveCore(recordKey, owner);
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

    private sealed record Observation(ZLinkLocationOwnerToken? Owner, long Timestamp);
}
