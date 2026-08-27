using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Zlink.Framework.Contracts.Configuration;

namespace SupportChat.Server.Configuration;

public enum SupportChatReadyKind
{
    Public,
    Stream,
    SpotRoute
}

public sealed record SupportChatReadiness(
    SupportChatReadyKind Kind,
    string NodeId,
    string? MeshName = null);

public sealed class SupportChatReadinessReporter(
    IEnumerable<SupportChatReadiness> readiness,
    IHostApplicationLifetime applicationLifetime,
    IZLinkRouteMeshRuntime routeMesh,
    ILogger<SupportChatReadinessReporter> logger) : IHostedService
{
    private readonly List<SupportChatReadiness> pending = readiness.ToList();
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
        var applicationStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var registration = applicationLifetime.ApplicationStarted.Register(
            () => applicationStarted.TrySetResult());
        await applicationStarted.Task.WaitAsync(cancellationToken);

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

    private bool IsReady(SupportChatReadiness report)
    {
        if (report.Kind is SupportChatReadyKind.Public or SupportChatReadyKind.Stream)
            return true;

        var status = routeMesh.GetStatus(report.MeshName!);
        return status.IsReady && status.Peers.Any(peer => peer.State == ZLinkPeerState.Ready);
    }

    private void LogReady(SupportChatReadiness report)
    {
        switch (report.Kind)
        {
            case SupportChatReadyKind.Public:
                logger.LogInformation("supportchat-ready kind=public node={NodeId}", report.NodeId);
                break;
            case SupportChatReadyKind.Stream:
                logger.LogInformation("supportchat-ready kind=stream node={NodeId}", report.NodeId);
                break;
            case SupportChatReadyKind.SpotRoute:
                logger.LogInformation(
                    "supportchat-ready kind=spot-route node={NodeId} mesh={MeshName}",
                    report.NodeId,
                    report.MeshName);
                break;
        }
    }
}
