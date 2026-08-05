using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Diagnostics.HealthChecks;

namespace Zlink.Framework.AspNetCore;

internal sealed class ZLinkDrainHealthCheck(IServiceProvider services) : IHealthCheck
{
    public Task<HealthCheckResult> CheckHealthAsync(
        HealthCheckContext context,
        CancellationToken cancellationToken = default)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        var ready = services.GetRequiredService<IZLinkFrameworkRuntime>().Status.IsReady;
        return Task.FromResult(
            ready
                ? HealthCheckResult.Healthy("ZLink accepts new assignments.")
                : HealthCheckResult.Unhealthy("ZLink is draining and rejects new assignments."));
    }
}
