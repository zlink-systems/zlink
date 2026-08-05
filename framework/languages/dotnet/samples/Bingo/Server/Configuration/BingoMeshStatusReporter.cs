using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Zlink.Framework.Contracts.Configuration;

namespace Bingo.Server.Configuration;

public sealed record BingoMeshStatusReport(
    string Role,
    string MeshName);

public sealed class BingoMeshStatusReporter(
    BingoMeshStatusReport report,
    IZLinkRouteMeshRuntime routeMesh,
    ILogger<BingoMeshStatusReporter> logger) : IHostedService
{
    public Task StartAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var status = routeMesh.GetStatus(report.MeshName);
        logger.LogInformation(
            "bingo mesh status ready. role={Role} mesh={Mesh} ready={Ready}",
            report.Role,
            report.MeshName,
            status.IsReady);
        return Task.CompletedTask;
    }

    public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;
}
