namespace Zlink.Framework.Runtime.Host;

/// <summary>
/// Owns the start and stop order of Framework runtime resources independently
/// from the host integration that invokes this lifecycle.
/// </summary>
internal sealed class ZLinkFrameworkHostRuntimeCoordinator(
    ZLinkFrameworkRuntime runtime,
    ZLinkRouteMeshRuntimeService routeMeshRuntime,
    ZLinkLocationRuntime? locationRuntime,
    ZLinkAutoConnectLifecycleCoordinator autoConnectLifecycle,
    ZLinkLocationLifecycle? locationLifecycle,
    ZLinkFrameworkMaintenanceRuntime maintenance)
{
    internal async Task StartAsync(CancellationToken cancellationToken)
    {
        try
        {
            if (locationRuntime is not null)
                await locationRuntime.StartAsync(
                        runtime.PrepareLocationNodeRoutingId(),
                        cancellationToken)
                    .ConfigureAwait(false);

            await runtime.StartAsync(cancellationToken).ConfigureAwait(false);
            routeMeshRuntime.Start();
            var state = await runtime.EnsureStartedStateAsync(cancellationToken)
                .ConfigureAwait(false);
            await autoConnectLifecycle.FrameworkReadyAsync(state, cancellationToken)
                .ConfigureAwait(false);
            maintenance.MarkServing();
        }
        catch (Exception startFailure)
        {
            maintenance.MarkError();
            try
            {
                await CloseResourcesAsync().ConfigureAwait(false);
            }
            catch (Exception cleanupFailure)
            {
                throw new AggregateException(startFailure, cleanupFailure);
            }

            System.Runtime.ExceptionServices.ExceptionDispatchInfo
                .Capture(startFailure)
                .Throw();
        }
    }

    internal async Task StopAsync(CancellationToken cancellationToken)
    {
        try
        {
            await maintenance.ShutdownAsync(cancellationToken: cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
            await maintenance.ShutdownAsync(cancellationToken: CancellationToken.None)
                .ConfigureAwait(false);
        }
        await CloseResourcesAsync().ConfigureAwait(false);
    }

    private async Task CloseResourcesAsync()
    {
        List<Exception>? failures = null;
        await CaptureAsync(
            () => autoConnectLifecycle.StopAsync(CancellationToken.None).AsTask())
            .ConfigureAwait(false);
        await CaptureAsync(() =>
        {
            routeMeshRuntime.Stop();
            return Task.CompletedTask;
        }).ConfigureAwait(false);
        await CaptureAsync(
            () => runtime.StopAsync(CancellationToken.None).AsTask())
            .ConfigureAwait(false);
        await CaptureAsync(
            () => locationRuntime?.RemoveOwnedRowsBeforeRoutingIdReleaseAsync(
                    CancellationToken.None).AsTask()
                ?? Task.CompletedTask)
            .ConfigureAwait(false);
        await CaptureAsync(
            () => locationRuntime?.StopAsync(CancellationToken.None).AsTask()
                ?? Task.CompletedTask)
            .ConfigureAwait(false);
        locationLifecycle?.ResetGeneration();

        if (failures is { Count: 1 }) throw failures[0];
        if (failures is { Count: > 1 }) throw new AggregateException(failures);
        return;

        async Task CaptureAsync(Func<Task> close)
        {
            try
            {
                await close().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }
    }
}
