using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Configuration;

namespace DeliveryDispatch.Server.Configuration;

public enum DeliveryDispatchReadyKind
{
    Route,
    ActorRoute
}

public sealed record DeliveryDispatchReadiness(
    DeliveryDispatchReadyKind Kind,
    string NodeId,
    string MeshName,
    int RequiredReadyPeers,
    string? TargetNodeId = null);

/// <summary>
/// Reports sample-owned readiness evidence from the public route-mesh status surface.  It never
/// sends a request: readiness is observed passively before the client starts.
/// </summary>
public sealed class DeliveryDispatchReadinessReporter(
    IEnumerable<DeliveryDispatchReadiness> readiness,
    IZLinkRouteMeshRuntime routeMesh,
    ILogger<DeliveryDispatchReadinessReporter> logger) : IHostedService
{
    private readonly List<DeliveryDispatchReadiness> pending = readiness.ToList();
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
        try
        {
            await reporting.WaitAsync(cancellationToken);
        }
        catch (OperationCanceledException) when (stopping.IsCancellationRequested)
        {
        }
        finally
        {
            stopping.Dispose();
        }
    }

    private async Task ReportAsync(CancellationToken cancellationToken)
    {
        while (pending.Count > 0)
        {
            for (var index = pending.Count - 1; index >= 0; index--)
            {
                var report = pending[index];
                try
                {
                    var status = routeMesh.GetStatus(report.MeshName);
                    if (!status.IsReady
                        || status.Peers.Count(peer => peer.State == ZLinkPeerState.Ready)
                        < report.RequiredReadyPeers)
                    {
                        continue;
                    }
                }
                catch (InvalidOperationException)
                {
                    // Framework startup has not published this mesh status yet.
                    continue;
                }

                if (report.Kind == DeliveryDispatchReadyKind.Route)
                {
                    logger.LogInformation(
                        "deliverydispatch-ready kind=route node={NodeId}",
                        report.NodeId);
                }
                else
                {
                    logger.LogInformation(
                        "deliverydispatch-ready kind=actor-route node={NodeId} target={TargetNodeId}",
                        report.NodeId,
                        report.TargetNodeId);
                }

                pending.RemoveAt(index);
            }

            if (pending.Count > 0)
                await Task.Delay(TimeSpan.FromMilliseconds(100), cancellationToken);
        }
    }
}
