using SpotService.Shared;
using Zlink.Framework.LocationProvider;

namespace SpotService.Server.Session;

internal sealed class LocationStoreReadProbe(IZLinkLocationStore inner)
    : IZLinkLocationStore
{
    private readonly object _gate = new();
    private string[] _actorIds = [];
    private long _matchingReads;
    private bool _blocked;

    public void Configure(IReadOnlyCollection<string> actorIds, bool blocked)
    {
        ArgumentNullException.ThrowIfNull(actorIds);
        var normalized = actorIds
            .Select(static actorId =>
                string.IsNullOrWhiteSpace(actorId)
                    ? throw new ArgumentException(
                        "Actor IDs cannot contain an empty value.",
                        nameof(actorIds))
                    : actorId)
            .Distinct(StringComparer.Ordinal)
            .ToArray();
        lock (_gate)
        {
            _actorIds = normalized;
            _blocked = blocked;
            Interlocked.Exchange(ref _matchingReads, 0);
        }
    }

    public LocationStoreReadProbeSnapshot Snapshot()
    {
        lock (_gate)
        {
            return new LocationStoreReadProbeSnapshot(
                Interlocked.Read(ref _matchingReads),
                _blocked,
                _actorIds);
        }
    }

    public ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default)
    {
        bool blocked;
        lock (_gate)
        {
            if (!_actorIds.Any(actorId =>
                    key.Value.Contains(actorId, StringComparison.Ordinal)))
                return inner.ReadAsync(key, cancellationToken);
            blocked = _blocked;
        }

        Interlocked.Increment(ref _matchingReads);
        if (blocked)
            throw new IOException(
                "SM-D4B blocked an Actor authority read after binding.");
        return inner.ReadAsync(key, cancellationToken);
    }

    public ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default) =>
        inner.WriteAsync(request, cancellationToken);

    public ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default) =>
        inner.ScanAsync(request, cancellationToken);

}
