using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Owns the location lifecycle subdomains for this runtime and routes
/// ownership-loss events to the subdomain that tracks the affected row kind.
/// </summary>
internal sealed class ZLinkLocationLifecycle : IAsyncDisposable
{
    private readonly ZLinkLocationRuntime _runtime;
    private readonly ZLinkStateLane _lane = new();
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
        return new ValueTask(AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_disposeTask is not null)
                return _disposeTask;

            Interlocked.Exchange(ref _disposed, 1);
            _runtime.OwnershipLost -= OnOwnershipLost;
            _disposeTask = StartDetached(DisposeCoreAsync);
            return _disposeTask;
        })));
    }

    private async Task DisposeCoreAsync()
    {
        await PauseBackgroundWorkCoreAsync(pauseActorOwnership: false).ConfigureAwait(false);
        await ActorOwnership.DisposeAsync().ConfigureAwait(false);
        _backgroundStop.Dispose();
        _backgroundDrainGate.Dispose();
    }

    internal ValueTask PauseBackgroundWorkAsync()
        => PauseBackgroundWorkCoreAsync(pauseActorOwnership: true);

    internal void ResumeBackgroundWork()
    {
        var stopped = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_disposed != 0 || !_backgroundStopping) return null;
            if (_backgroundStop.IsCancellationRequested)
            {
                var cancelled = _backgroundStop;
                _backgroundStop = new CancellationTokenSource();
                _backgroundStopping = false;
                return cancelled;
            }
            _backgroundStopping = false;
            return null;
        }));
        stopped?.Dispose();
        AwaitStateLane(ActorOwnership.ResumeBackgroundWorkAsync());
    }

    internal void ResetGeneration()
    {
        AwaitStateLane(SpotLocations.ResetGenerationAsync());
        AwaitStateLane(ActorOwnership.ResetGenerationAsync());
    }

    private void OnOwnershipLost(ZLinkLocationKind kind, string canonicalKey)
    {
        Func<CancellationToken, ValueTask>? deactivate = null;
        if (kind == ZLinkLocationKind.Actor)
        {
            deactivate = AwaitStateLane(
                ActorOwnership.TakeOwnershipLostDeactivationAsync(canonicalKey));
        }
        else if (kind == ZLinkLocationKind.Spot)
        {
            deactivate = AwaitStateLane(
                SpotLocations.TakeOwnershipLostDeactivationAsync(canonicalKey));
        }

        if (deactivate is not null)
            TryRunBackground(deactivate);
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    private async ValueTask PauseBackgroundWorkCoreAsync(bool pauseActorOwnership)
    {
        await _backgroundDrainGate.WaitAsync(CancellationToken.None).ConfigureAwait(false);
        try
        {
            var tasks = await _lane.RunAsync(() =>
            {
                _backgroundStopping = true;
                var currentStop = _backgroundStop;
                currentStop.Cancel();
                return _backgroundTasks.ToArray();
            }).ConfigureAwait(false);

            if (tasks.Length != 0) await Task.WhenAll(tasks).ConfigureAwait(false);
            if (pauseActorOwnership)
                await ActorOwnership.PauseBackgroundWorkAsync().ConfigureAwait(false);

            await _lane.RunAsync(() =>
            {
                _backgroundTasks.RemoveWhere(static task => task.IsCompleted);
                _backgroundStopping = true;
            }).ConfigureAwait(false);
        }
        finally
        {
            _backgroundDrainGate.Release();
        }
    }

    private void RemoveCompletedTasks()
    {
        AwaitStateLane(_lane.RunAsync(
            () => _backgroundTasks.RemoveWhere(static task => task.IsCompleted)));
    }

    private bool TryRunBackground(Func<CancellationToken, ValueTask> operation)
    {
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_disposed != 0 || _backgroundStopping) return false;
            var stopToken = _backgroundStop.Token;
            var task = StartDetached(() => RunGuardedAsync(
                () => operation(stopToken),
                stopToken));
            _backgroundTasks.Add(task);
            StartDetachedContinuation(task);
            return true;
        }));
    }

    private Task StartDetached(Func<Task> operation)
    {
        if (ExecutionContext.IsFlowSuppressed())
            return Task.Run(operation);
        using (ExecutionContext.SuppressFlow())
            return Task.Run(operation);
    }

    private void StartDetachedContinuation(Task task)
    {
        if (ExecutionContext.IsFlowSuppressed())
        {
            _ = task.ContinueWith(
                static (_, state) => ((ZLinkLocationLifecycle)state!).RemoveCompletedTasks(),
                this,
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
            return;
        }

        using (ExecutionContext.SuppressFlow())
            _ = task.ContinueWith(
                static (_, state) => ((ZLinkLocationLifecycle)state!).RemoveCompletedTasks(),
                this,
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
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
