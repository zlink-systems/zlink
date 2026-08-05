namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Owns the location lifecycle subdomains for this runtime and routes
/// ownership-loss events to the subdomain that tracks the affected row kind.
/// </summary>
internal sealed class ZLinkLocationLifecycle : IAsyncDisposable
{
    private readonly ZLinkLocationRuntime _runtime;
    private readonly object _backgroundGate = new();
    private readonly object _disposeStartGate = new();
    private readonly SemaphoreSlim _backgroundDrainGate = new(1, 1);
    private readonly HashSet<Task> _backgroundTasks = [];
    private CancellationTokenSource _backgroundStop = new();
    private bool _backgroundStopping;
    private int _disposed;
    private Task? _disposeTask;

    internal ZLinkLocationLifecycle(
        ZLinkLocationRuntime runtime,
        ZLinkStoreLocationResolvers resolver)
    {
        _runtime = runtime;
        _runtime.OwnershipLost += OnOwnershipLost;
        ActorOwnership = new ZLinkActorOwnershipCoordinator(runtime, resolver);
        SpotLocations = new ZLinkSpotLocationLifecycle(runtime);
    }

    internal ZLinkActorOwnershipCoordinator ActorOwnership { get; }

    internal ZLinkSpotLocationLifecycle SpotLocations { get; }

    internal ZLinkLocationOwnerToken OwnerToken => _runtime.AdmissionOwnerToken;

    internal ValueTask<ZLinkLocationWriteResult> WriteMeshNodeDescriptorAsync(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default) =>
        _runtime.WriteDescriptorAsync(descriptor, intent, cancellationToken);

    internal ValueTask<ZLinkLocationWriteResult> RemoveMeshNodeDescriptorAsync(
        ZLinkMeshNodeDescriptorKey key,
        CancellationToken cancellationToken = default) =>
        _runtime.RemoveDescriptorAsync(key, cancellationToken);

    internal ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListMeshNodesAsync(
        string meshName,
        CancellationToken cancellationToken = default) =>
        _runtime.Store.ListAllMeshNodesAsync(meshName, cancellationToken);

    internal async ValueTask<ZLinkFrameworkErrorKind> ClassifyMeshNodeClaimConflictAsync(
        string meshName,
        RoutingId routingId,
        string entrySpotId,
        CancellationToken cancellationToken = default)
    {
        try
        {
            var descriptors = await _runtime.Store
                .ListAllMeshNodesAsync(meshName, cancellationToken)
                .ConfigureAwait(false);
            if (descriptors.Any(descriptor => descriptor.Rid.Equals(routingId)))
                return ZLinkFrameworkErrorKind.AlreadyExists;
            if (descriptors.Any(
                    descriptor => string.Equals(
                        descriptor.EntrySpotId,
                        entrySpotId,
                        StringComparison.Ordinal)))
                return ZLinkFrameworkErrorKind.AlreadyExists;

            var authority = await _runtime.Store.ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(entrySpotId),
                    cancellationToken)
                .ConfigureAwait(false);
            if (authority is ZLinkAuthorityReadResult.Found)
                return ZLinkFrameworkErrorKind.AlreadyExists;
        }
        catch when (!cancellationToken.IsCancellationRequested)
        {
            // Conflict classification is diagnostic only. A failed follow-up
            // read must not turn the already terminal claim conflict into a
            // second store operation or a retry.
        }

        return ZLinkFrameworkErrorKind.AlreadyExists;
    }

    public ValueTask DisposeAsync()
    {
        lock (_disposeStartGate)
            return new ValueTask(_disposeTask ??= DisposeCoreAsync());
    }

    private async Task DisposeCoreAsync()
    {
        Interlocked.Exchange(ref _disposed, 1);
        _runtime.OwnershipLost -= OnOwnershipLost;
        await PauseBackgroundWorkCoreAsync(pauseActorOwnership: false).ConfigureAwait(false);
        await ActorOwnership.DisposeAsync().ConfigureAwait(false);
        _backgroundStop.Dispose();
        _backgroundDrainGate.Dispose();
    }

    internal ValueTask PauseBackgroundWorkAsync()
        => PauseBackgroundWorkCoreAsync(pauseActorOwnership: true);

    internal void ResumeBackgroundWork()
    {
        CancellationTokenSource? stopped = null;
        lock (_backgroundGate)
        {
            if (_disposed != 0 || !_backgroundStopping) return;
            if (_backgroundStop.IsCancellationRequested)
            {
                stopped = _backgroundStop;
                _backgroundStop = new CancellationTokenSource();
            }
            _backgroundStopping = false;
        }
        stopped?.Dispose();
        ActorOwnership.ResumeBackgroundWork();
    }

    internal void ResetGeneration()
    {
        SpotLocations.ResetGeneration();
        ActorOwnership.ResetGeneration();
    }

    private void OnOwnershipLost(ZLinkLocationKind kind, string canonicalKey)
    {
        Func<CancellationToken, ValueTask>? deactivate = null;
        if (kind == ZLinkLocationKind.Actor)
        {
            deactivate = ActorOwnership.TakeOwnershipLostDeactivation(canonicalKey);
        }
        else if (kind == ZLinkLocationKind.Spot)
        {
            deactivate = SpotLocations.TakeOwnershipLostDeactivation(canonicalKey);
        }

        if (deactivate is not null)
            TryRunBackground(deactivate);
    }

    private async ValueTask PauseBackgroundWorkCoreAsync(bool pauseActorOwnership)
    {
        await _backgroundDrainGate.WaitAsync(CancellationToken.None).ConfigureAwait(false);
        try
        {
            Task[] tasks;
            CancellationTokenSource stop;
            lock (_backgroundGate)
            {
                _backgroundStopping = true;
                stop = _backgroundStop;
                stop.Cancel();
                tasks = _backgroundTasks.ToArray();
            }

            if (tasks.Length != 0) await Task.WhenAll(tasks).ConfigureAwait(false);
            if (pauseActorOwnership)
                await ActorOwnership.PauseBackgroundWorkAsync().ConfigureAwait(false);

            lock (_backgroundGate)
            {
                _backgroundTasks.RemoveWhere(static task => task.IsCompleted);
                _backgroundStopping = true;
            }
        }
        finally
        {
            _backgroundDrainGate.Release();
        }
    }

    private void RemoveCompletedTasks()
    {
        lock (_backgroundGate)
            _backgroundTasks.RemoveWhere(static task => task.IsCompleted);
    }

    private bool TryRunBackground(Func<CancellationToken, ValueTask> operation)
    {
        lock (_backgroundGate)
        {
            if (_disposed != 0 || _backgroundStopping) return false;
            var stopToken = _backgroundStop.Token;
            var task = RunGuardedAsync(() => operation(stopToken), stopToken);
            _backgroundTasks.Add(task);
            _ = task.ContinueWith(
                static (_, state) => ((ZLinkLocationLifecycle)state!).RemoveCompletedTasks(),
                this,
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
            return true;
        }
    }

    private static async Task RunGuardedAsync(
        Func<ValueTask> operation,
        CancellationToken cancellationToken)
    {
        try
        {
            await operation().ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception exception)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery($"location lifecycle error: {exception.Message}");
        }
    }
}
