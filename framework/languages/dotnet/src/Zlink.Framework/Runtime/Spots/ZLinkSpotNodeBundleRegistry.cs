using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotNodeBundleRegistry(
    IZLinkBackendSpotNode node) : IAsyncDisposable
{
    private readonly ZLinkStateLane _lane = new();
    private readonly object _disposeGate = new();
    private readonly Dictionary<ZLinkChannelName, ZLinkSpotPublisherBundle> _publisherBundles = [];
    private Task? _disposeTask;
    private Task? _disposeCompletionTask;
    private bool _closed;

    public ValueTask DisposeAsync()
    {
        lock (_disposeGate)
        {
            _disposeTask ??= DisposeAsyncCore();
            return new ValueTask(_disposeTask);
        }
    }

    private async Task DisposeAsyncCore()
    {
        var prepared = await _lane.RunAsync(() =>
        {
            if (_disposeCompletionTask is not null)
                return new DisposePreparation(_disposeCompletionTask, null);
            _closed = true;
            var publishers = _publisherBundles.Values.ToArray();
            _publisherBundles.Clear();
            var completion = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _disposeCompletionTask = completion.Task;
            return new DisposePreparation(completion.Task, publishers, completion);
        }).ConfigureAwait(false);
        if (prepared.Publishers is { } publishers)
            _ = DisposeCoreAsync(publishers, prepared.Completion!);
        await prepared.Task.ConfigureAwait(false);
    }

    private async Task DisposeCoreAsync(
        ZLinkSpotPublisherBundle[] publishers,
        TaskCompletionSource completion)
    {
        try
        {
            foreach (var publisher in publishers) await publisher.DisposeAsync();
            await _lane.DisposeAsync().ConfigureAwait(false);
            completion.TrySetResult();
        }
        catch (Exception error)
        {
            try
            {
                await _lane.DisposeAsync().ConfigureAwait(false);
            }
            catch (Exception closeError)
            {
                completion.TrySetException(new AggregateException(error, closeError));
                return;
            }
            completion.TrySetException(error);
        }
    }

    public ValueTask<ZLinkSpotPublisherBundle> GetOrCreatePublisherBundleAsync(
        string channelName)
    {
        var channel = ZLinkChannelName.FromBoundary(channelName, nameof(channelName));
        return _lane.RunAsync(() =>
        {
            ObjectDisposedException.ThrowIf(_closed, this);
            if (_publisherBundles.TryGetValue(channel, out var existing)) return existing;

            var bundle = CreatePublisherBundle();

            _publisherBundles.Add(channel, bundle);
            return bundle;
        });
    }

    private ZLinkSpotPublisherBundle CreatePublisherBundle()
    {
        return new ZLinkSpotPublisherBundle(node.CreateSpot());
    }

    private readonly record struct DisposePreparation(
        Task Task,
        ZLinkSpotPublisherBundle[]? Publishers,
        TaskCompletionSource? Completion = null);
}
