using Microsoft.Extensions.Hosting;

namespace Zlink.Framework.AspNetCore;

internal sealed class ZLinkFrameworkHostedService(
    ZLinkFrameworkHostRuntimeCoordinator runtime) : IHostedService
{
    public Task StartAsync(CancellationToken cancellationToken) =>
        runtime.StartAsync(cancellationToken);

    public Task StopAsync(CancellationToken cancellationToken) =>
        runtime.StopAsync(cancellationToken);
}
