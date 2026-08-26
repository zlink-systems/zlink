using Microsoft.Extensions.Hosting;
using Systems.Zlink;
using Zlink.Framework.Contracts.Configuration;

namespace GameQuest.GameApi;

internal sealed record GameQuestSpotRouteReadiness(string NodeId, string MeshName);

internal sealed class GameQuestSpotRouteReadinessReporter(
    IEnumerable<GameQuestSpotRouteReadiness> readiness,
    IZLinkRouteMeshRuntime routeMesh,
    ILogger<GameQuestSpotRouteReadinessReporter> logger) : IHostedService
{
    private readonly List<GameQuestSpotRouteReadiness> pending = readiness.ToList();
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
                    if (!status.IsReady || !status.Peers.Any(peer => peer.State == ZLinkPeerState.Ready))
                        continue;
                }
                catch (InvalidOperationException)
                {
                    // The public runtime status becomes available during Framework startup.
                    continue;
                }

                logger.LogInformation(
                    "gamequest-ready kind=spot-route node={NodeId} mesh={MeshName}",
                    report.NodeId,
                    report.MeshName);
                pending.RemoveAt(index);
            }

            if (pending.Count > 0)
                await Task.Delay(TimeSpan.FromMilliseconds(100), cancellationToken);
        }
    }
}
