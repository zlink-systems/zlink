using Microsoft.Extensions.Hosting;
using Systems.Zlink;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.AspNetCore;

internal sealed class ZLinkFrameworkHostedService(
    ZLinkFrameworkRuntime runtime,
    ZLinkRouteMeshRuntimeService routeMeshRuntime,
    ZLinkLocationRuntime? locationRuntime,
    ZLinkAutoConnectLifecycleCoordinator autoConnectLifecycle,
    ZLinkLocationLifecycle? locationLifecycle,
    ZLinkFrameworkMaintenanceRuntime maintenance) : IHostedService
{
    public async Task StartAsync(CancellationToken cancellationToken)
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
            var state = await runtime.EnsureStartedStateAsync(cancellationToken).ConfigureAwait(false);
            await autoConnectLifecycle.FrameworkReadyAsync(state, cancellationToken).ConfigureAwait(false);
            maintenance.MarkServing();
        }
        catch (Exception startFailure)
        {
            maintenance.MarkError();
            try
            {
                await StopCoreAsync().ConfigureAwait(false);
            }
            catch (Exception cleanupFailure)
            {
                throw new AggregateException(startFailure, cleanupFailure);
            }

            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(startFailure).Throw();
        }
    }

    public Task StopAsync(CancellationToken cancellationToken)
    {
        return DrainAndStopAsync(cancellationToken);
    }

    private async Task DrainAndStopAsync(CancellationToken cancellationToken)
    {
        try
        {
            await maintenance.ShutdownAsync(cancellationToken: cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            await maintenance.ShutdownAsync(cancellationToken: CancellationToken.None)
                .ConfigureAwait(false);
        }
        await StopCoreAsync().ConfigureAwait(false);
    }

    private async Task StopCoreAsync()
    {
        List<Exception>? failures = null;
        await TryStopAsync(
            () => autoConnectLifecycle.StopAsync(CancellationToken.None).AsTask()).ConfigureAwait(false);
        await TryStopAsync(() =>
        {
            routeMeshRuntime.Stop();
            return Task.CompletedTask;
        }).ConfigureAwait(false);
        await TryStopAsync(
            () => runtime.StopAsync(CancellationToken.None).AsTask()).ConfigureAwait(false);
        await TryStopAsync(
            () => locationRuntime?.RemoveOwnedRowsBeforeRoutingIdReleaseAsync(CancellationToken.None).AsTask()
                  ?? Task.CompletedTask).ConfigureAwait(false);
        await TryStopAsync(
            () => locationRuntime?.StopAsync(CancellationToken.None).AsTask() ?? Task.CompletedTask).ConfigureAwait(false);
        locationLifecycle?.ResetGeneration();

        if (failures is { Count: 1 }) throw failures[0];
        if (failures is { Count: > 1 }) throw new AggregateException(failures);
        return;

        async Task TryStopAsync(Func<Task> stop)
        {
            try
            {
                await stop().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }
        }
    }

}
