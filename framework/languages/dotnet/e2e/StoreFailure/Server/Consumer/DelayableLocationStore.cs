using Zlink.Framework.LocationProvider;

namespace StoreFailure.Server.Consumer;

internal sealed class LocationStoreDelayState
{
    private int _delayMilliseconds;

    public int DelayMilliseconds => Volatile.Read(ref _delayMilliseconds);

    public void SetDelay(TimeSpan delay)
    {
        var milliseconds = (int)Math.Clamp(delay.TotalMilliseconds, 0, 5000);
        Volatile.Write(ref _delayMilliseconds, milliseconds);
    }
}

/// <summary>
/// Injects the configured delay before every public location-store operation.
/// The inner store continues to own all location semantics.
/// </summary>
internal sealed class DelayableLocationStore(
    IZLinkLocationStore inner,
    LocationStoreDelayState delayState) :
    IZLinkLocationStore
{
    private async ValueTask DelayAsync(CancellationToken cancellationToken)
    {
        var delay = delayState.DelayMilliseconds;
        if (delay > 0)
            await Task.Delay(delay, cancellationToken);
    }

    public async ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default)
    {
        await DelayAsync(cancellationToken);
        return await inner.ReadAsync(key, cancellationToken);
    }

    public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default)
    {
        await DelayAsync(cancellationToken);
        return await inner.WriteAsync(request, cancellationToken);
    }

    public async ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default)
    {
        await DelayAsync(cancellationToken);
        return await inner.ScanAsync(request, cancellationToken);
    }
}
