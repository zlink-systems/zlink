using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Zlink.Framework.Contracts.Configuration;

namespace TicTacToe.Server.Configuration;

internal enum TicTacToeReadyKind
{
    PeerRoute,
    SpotRoute
}

internal sealed record TicTacToeMeshReadiness(
    TicTacToeReadyKind Kind,
    string NodeId,
    string MeshName,
    string? PeerNodeId = null);

internal sealed class TicTacToeMeshReadinessReporter(
    IEnumerable<TicTacToeMeshReadiness> readiness,
    IZLinkRouteMeshRuntime routeMesh,
    ILogger<TicTacToeMeshReadinessReporter> logger) : IHostedService
{
    private readonly List<TicTacToeMeshReadiness> pending = readiness.ToList();
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
                    if (!IsReady(report)) continue;
                }
                catch (InvalidOperationException)
                {
                    // The public runtime status becomes available during Framework startup.
                    continue;
                }

                LogReady(report);
                pending.RemoveAt(index);
            }

            if (pending.Count > 0)
                await Task.Delay(TimeSpan.FromMilliseconds(100), cancellationToken);
        }
    }

    private bool IsReady(TicTacToeMeshReadiness report)
    {
        var status = routeMesh.GetStatus(report.MeshName);
        return report.Kind switch
        {
            // Spec 10.1 wants the named peer confirmed. Without a fixed RID (see PlayServer) the
            // peer cannot be named: peer status carries no endpoint. Deviation tracked in
            // doc/plan/spec-server-reorg/spec-gap.ko.md.
            // Spec 10.1: confirm the peer named in the row, not merely that some peer is ready.
            // The spot mesh uses a fixed RID so the expected peer can be named at all.
            TicTacToeReadyKind.PeerRoute => status.Peers.Any(peer =>
                peer.NodeRid == SampleNodes.RouteMeshRoutingId(report.PeerNodeId!)
                && peer.State == ZLinkPeerState.Ready),
            // Spec 10.1: this row means the Api node can reach the Play spot mesh, not merely
            // that the mesh object exists. Mesh IsReady can be true before any object-capable Play
            // peer is admitted, which lets the client start too early and fail its first JoinSpot.
            TicTacToeReadyKind.SpotRoute => status.IsReady
                && status.Peers.Any(peer => peer.State == ZLinkPeerState.Ready),
            _ => false
        };
    }

    private void LogReady(TicTacToeMeshReadiness report)
    {
        switch (report.Kind)
        {
            case TicTacToeReadyKind.PeerRoute:
                logger.LogInformation(
                    "tictactoe-ready kind=peer-route node={NodeId} peer={PeerNodeId}",
                    report.NodeId,
                    report.PeerNodeId);
                break;
            case TicTacToeReadyKind.SpotRoute:
                logger.LogInformation(
                    "tictactoe-ready kind=spot-route node={NodeId} mesh={MeshName}",
                    report.NodeId,
                    report.MeshName);
                break;
        }
    }
}
