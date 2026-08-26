using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Zlink.Framework.Contracts.Configuration;

namespace Bingo.Server.Configuration;

public enum BingoReadyKind
{
    PeerRoute,
    MeshRoute
}

public sealed record BingoReadyReport(
    BingoReadyKind Kind,
    string NodeId,
    string RuntimeName,
    string? EvidenceName = null);

public sealed class BingoMeshStatusReporter(
    IEnumerable<BingoReadyReport> reports,
    IZLinkRouteMeshRuntime routeMesh,
    ILogger<BingoMeshStatusReporter> logger) : IHostedService
{
    private readonly List<BingoReadyReport> _pending = reports.ToList();
    private CancellationTokenSource? _stopping;
    private Task? _reporting;

    public Task StartAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        _stopping = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        _reporting = ReportReadinessAsync(_stopping.Token);
        return Task.CompletedTask;
    }

    public async Task StopAsync(CancellationToken cancellationToken)
    {
        if (_stopping is null || _reporting is null) return;

        await _stopping.CancelAsync();
        try
        {
            await _reporting.WaitAsync(cancellationToken);
        }
        catch (OperationCanceledException) when (_stopping.IsCancellationRequested)
        {
        }
        finally
        {
            _stopping.Dispose();
        }
    }

    private async Task ReportReadinessAsync(CancellationToken cancellationToken)
    {
        while (_pending.Count > 0)
        {
            for (var index = _pending.Count - 1; index >= 0; index--)
            {
                var report = _pending[index];
                bool ready;
                try
                {
                    ready = IsReady(report);
                }
                catch (InvalidOperationException)
                {
                    continue;
                }
                if (!ready) continue;

                LogReady(report);
                _pending.RemoveAt(index);
            }

            if (_pending.Count > 0)
                await Task.Delay(TimeSpan.FromMilliseconds(100), cancellationToken);
        }
    }

    private bool IsReady(BingoReadyReport report)
    {
        return report.Kind switch
        {
            BingoReadyKind.PeerRoute => routeMesh.GetStatus(report.RuntimeName).Peers
                .Any(static peer => peer.State == ZLinkPeerState.Ready),
            BingoReadyKind.MeshRoute => routeMesh.GetStatus(report.RuntimeName).IsReady,
            _ => false
        };
    }

    private void LogReady(BingoReadyReport report)
    {
        switch (report.Kind)
        {
            case BingoReadyKind.PeerRoute:
                logger.LogInformation(
                    "bingo-ready kind=peer-route node={NodeId} peer={PeerNodeId}",
                    report.NodeId,
                    report.EvidenceName);
                break;
            case BingoReadyKind.MeshRoute:
                logger.LogInformation(
                    "bingo-ready kind=mesh-route node={NodeId} mesh={MeshName}",
                    report.NodeId,
                    report.EvidenceName);
                break;
        }
    }
}
