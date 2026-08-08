namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotNodeBundleRegistry(
    ZLinkFrameworkRegistration frameworkRegistration,
    IZLinkBackendSpotNode node,
    CancellationToken stopToken) : IAsyncDisposable
{
    private readonly object _gate = new();
    private readonly Dictionary<ZLinkChannelName, ZLinkSpotPublisherBundle> _publisherBundles = [];
    private Task? _disposeTask;
    private bool _closed;

    public ValueTask DisposeAsync()
    {
        lock (_gate)
        {
            if (_disposeTask is not null) return new ValueTask(_disposeTask);
            _closed = true;
            var publishers = _publisherBundles.Values.ToArray();
            _publisherBundles.Clear();
            return new ValueTask(_disposeTask = DisposeCoreAsync(publishers));
        }
    }

    private static async Task DisposeCoreAsync(ZLinkSpotPublisherBundle[] publishers)
    {
        foreach (var publisher in publishers) await publisher.DisposeAsync();
    }

    public ZLinkSpotPublisherBundle GetOrCreatePublisherBundle(string channelName)
    {
        var channel = ZLinkChannelName.FromBoundary(channelName, nameof(channelName));
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_closed, this);
            if (_publisherBundles.TryGetValue(channel, out var existing)) return existing;

            var bundle = CreatePublisherBundle();

            _publisherBundles.Add(channel, bundle);
            return bundle;
        }
    }

    private ZLinkSpotPublisherBundle CreatePublisherBundle()
    {
        var publisher = node.CreateSpot();
        return new ZLinkSpotPublisherBundle(
            publisher,
            new ZLinkAsyncSubmitter(
                publisher.OnSendReady,
                frameworkRegistration.DefaultSocketSendTimeout,
                stopToken));
    }
}
