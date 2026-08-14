namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotPublisherBundle : IAsyncDisposable
{
    public ZLinkSpotPublisherBundle(IZLinkBackendSpot spot)
    {
        Spot = spot;
    }

    public IZLinkBackendSpot Spot { get; }

    public ValueTask DisposeAsync() => Spot.DisposeAsync();
}
