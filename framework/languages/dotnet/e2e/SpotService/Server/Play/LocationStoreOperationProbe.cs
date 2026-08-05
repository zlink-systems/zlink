using Zlink.Framework.LocationProvider;

namespace SpotService.Server.Play;

internal sealed class LocationStoreOperationProbe(IZLinkLocationStore inner)
    : IZLinkLocationStore
{
    private string _spotId = string.Empty;
    private long _reads;
    private long _writes;

    public void Reset(string spotId)
    {
        _spotId = spotId;
        Interlocked.Exchange(ref _reads, 0);
        Interlocked.Exchange(ref _writes, 0);
    }

    public (long Reads, long Writes) Snapshot() =>
        (Interlocked.Read(ref _reads), Interlocked.Read(ref _writes));

    public ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default)
    {
        if (Matches(key)) Interlocked.Increment(ref _reads);
        return inner.ReadAsync(key, cancellationToken);
    }

    public ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default)
    {
        if (request.Conditions.Any(condition => condition switch
            {
                ZLinkStoreCondition.Missing value => Matches(value.Key),
                ZLinkStoreCondition.Version value => Matches(value.Key),
                _ => false
            })
            || request.Mutations.Any(mutation => mutation switch
            {
                ZLinkStoreMutation.Put value => Matches(value.Key),
                ZLinkStoreMutation.Delete value => Matches(value.Key),
                _ => false
            }))
        {
            Interlocked.Increment(ref _writes);
        }

        return inner.WriteAsync(request, cancellationToken);
    }

    public ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default) =>
        inner.ScanAsync(request, cancellationToken);

    private bool Matches(ZLinkStoreKey key) =>
        !string.IsNullOrEmpty(_spotId)
        && key.Value.Contains(_spotId, StringComparison.Ordinal);
}
