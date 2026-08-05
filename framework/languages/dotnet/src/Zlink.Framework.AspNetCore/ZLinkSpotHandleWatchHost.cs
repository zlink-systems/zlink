using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.AspNetCore;

internal sealed class ZLinkSpotHandleWatchHost(
    IZLinkLocationWatchStore? watchStore,
    ZLinkStoreLocationResolvers rows,
    ZLinkSpotHandleRegistry? handles,
    ZLinkLocationOptions options) : IHostedService, IAsyncDisposable
{
    private readonly object _lifecycleGate = new();
    private CancellationTokenSource? _stop;
    private Task[] _watches = [];
    private Task? _shutdown;

    public Task StartAsync(CancellationToken cancellationToken)
    {
        lock (_lifecycleGate)
        {
            if (_stop is not null || _shutdown is not null)
                throw new InvalidOperationException("The location watch host cannot be started more than once.");

            _stop = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            var watches = new List<Task>();
            if (handles is not null)
                watches.Add(PollAsync(_stop.Token));
            if (watchStore is not null)
            {
                watches.Add(WatchAsync(ZLinkLocationKind.Spot, _stop.Token));
                watches.Add(WatchAsync(ZLinkLocationKind.Actor, _stop.Token));
            }
            _watches = watches.ToArray();
        }
        return Task.CompletedTask;
    }

    public async Task StopAsync(CancellationToken cancellationToken)
    {
        await ShutdownAsync().WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask DisposeAsync()
    {
        await ShutdownAsync().ConfigureAwait(false);
    }

    private Task ShutdownAsync()
    {
        lock (_lifecycleGate)
        {
            if (_shutdown is not null) return _shutdown;
            var stop = _stop;
            var watches = _watches;
            _stop = null;
            _watches = [];
            return _shutdown = stop is null
                ? Task.CompletedTask
                : ShutdownCoreAsync(stop, watches);
        }
    }

    private static async Task ShutdownCoreAsync(
        CancellationTokenSource stop,
        Task[] watches)
    {
        try
        {
            await stop.CancelAsync().ConfigureAwait(false);
            try
            {
                await Task.WhenAll(watches).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
        }
        finally
        {
            stop.Dispose();
        }
    }

    private async Task WatchAsync(ZLinkLocationKind kind, CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                await foreach (var change in watchStore!.WatchAsync(
                                   new ZLinkLocationWatchFilter(kind), cancellationToken)
                                   .ConfigureAwait(false))
                    await ApplyAsync(change, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch
            {
                var retryDelay = options.PollingInterval > TimeSpan.Zero
                    ? options.PollingInterval
                    : TimeSpan.FromMilliseconds(100);
                await Task.Delay(retryDelay, cancellationToken).ConfigureAwait(false);
            }
        }
    }

    private async Task PollAsync(CancellationToken cancellationToken)
    {
        var interval = options.PollingInterval > TimeSpan.Zero
            ? options.PollingInterval
            : TimeSpan.FromMilliseconds(100);
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(interval, cancellationToken).ConfigureAwait(false);
                // Each handle refreshes through its own logical lookup key;
                // a key with no current row invalidates at its current
                // version so a strictly newer row can resurrect it.
                foreach (var handle in handles?.SnapshotLiveHandles()
                         ?? Array.Empty<ZLinkResolvedSpotHandle>())
                {
                    if (!await handle.RefreshAsync(cancellationToken).ConfigureAwait(false))
                        handle.InvalidateCurrent();
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch
            {
                // The next polling interval retries every still-live key.
            }
        }
    }

    internal async ValueTask ApplyAsync(
        ZLinkLocationChanged change,
        CancellationToken cancellationToken)
    {
        // Watch is an optimization only: a lost or lagging event is always
        // corrected by the polling refresh, so an upsert whose row is not
        // yet readable is simply left to the next poll.
        if (change.Key is ZLinkLocationKey.Spot(var spotKey))
        {
            // A watch notification carries a newer store observation. Drop
            // the positive route before resolving so the update cannot be
            // satisfied by the older cached StoreVersion.
            rows.InvalidateSpotRoute(spotKey);
            if (change.ChangeType is ZLinkLocationChangeType.Removed or ZLinkLocationChangeType.Expired)
            {
                handles?.RemoveSpot(spotKey, change.Generation);
                return;
            }

            var row = await rows.ResolveSpotRowAsync(spotKey, cancellationToken).ConfigureAwait(false);
            if (row is not null) handles?.UpdateSpot(row);
            return;
        }

        if (change.Key is not ZLinkLocationKey.Actor(var actorKey)) return;
        rows.InvalidateActorRoute(actorKey);
        if (change.ChangeType is ZLinkLocationChangeType.Removed or ZLinkLocationChangeType.Expired)
        {
            handles?.RemoveActor(actorKey);
            return;
        }

        var actor = await rows.ResolveActorRowAsync(actorKey, cancellationToken).ConfigureAwait(false);
        if (actor is not null) handles?.UpdateActor(actor);
    }
}
