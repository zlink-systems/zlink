using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Zlink.Framework.Contracts.Configuration;

namespace ShoppingMall.Server.Configuration;

/// <summary>
/// Emits sample-owned RouteMesh readiness after passively observing both workflow peers.
/// It never creates a request to establish the route.
/// </summary>
public sealed class ShoppingMallReadinessReporter(
    ApiInstanceTopology instance,
    IZLinkRouteMeshRuntime routeMesh,
    ILogger<ShoppingMallReadinessReporter> logger) : IHostedService
{
    private CancellationTokenSource? stopping;
    private Task? reporting;

    public Task StartAsync(CancellationToken cancellationToken)
    {
        stopping = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        reporting = ReportAsync(stopping.Token);
        return Task.CompletedTask;
    }

    public async Task StopAsync(CancellationToken cancellationToken)
    {
        if (stopping is null || reporting is null) return;

        await stopping.CancelAsync();
        try { await reporting.WaitAsync(cancellationToken); }
        catch (OperationCanceledException) when (stopping.IsCancellationRequested) { }
        finally { stopping.Dispose(); }
    }

    private async Task ReportAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                var status = routeMesh.GetStatus(SampleNames.MeshName);
                if (status.IsReady && status.Peers.Count(static peer => peer.State == ZLinkPeerState.Ready) >= 2)
                {
                    logger.LogInformation("shoppingmall-ready kind=object-route node={NodeId} target=workflow-a", instance.InstanceId);
                    logger.LogInformation("shoppingmall-ready kind=object-route node={NodeId} target=workflow-b", instance.InstanceId);
                    return;
                }
            }
            catch (InvalidOperationException)
            {
                // Framework startup has not published RouteMesh status yet.
            }

            await Task.Delay(TimeSpan.FromMilliseconds(100), cancellationToken);
        }
    }
}
