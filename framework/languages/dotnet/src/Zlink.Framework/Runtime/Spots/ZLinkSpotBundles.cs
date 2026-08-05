namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotPublisherBundle : IAsyncDisposable
{
    public ZLinkSpotPublisherBundle(
        IZLinkBackendSpot spot,
        ZLinkAsyncSubmitter? submitter = null)
    {
        Spot = spot;
        Submitter = submitter;
    }

    public IZLinkBackendSpot Spot { get; }

    public ZLinkAsyncSubmitter? Submitter { get; }

    public async ValueTask DisposeAsync()
    {
        if (Submitter is not null) await Submitter.DisposeAsync();

        await Spot.DisposeAsync();
    }
}