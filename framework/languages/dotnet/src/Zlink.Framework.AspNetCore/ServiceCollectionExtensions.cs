using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.DependencyInjection.Extensions;
using Microsoft.Extensions.Diagnostics.HealthChecks;
using Microsoft.Extensions.Hosting;

namespace Zlink.Framework.AspNetCore;

public static class ServiceCollectionExtensions
{
    public static IServiceCollection AddZLinkFramework(
        this IServiceCollection services,
        Action<IZLinkFrameworkOptions> configure)
    {
        ArgumentNullException.ThrowIfNull(services);
        ArgumentNullException.ThrowIfNull(configure);

        if (services.Any(static descriptor => descriptor.ServiceType == typeof(ZLinkFrameworkRegistration)))
            throw new ZLinkConfigurationException("ZLink framework is already configured.");

        var registration = new ZLinkFrameworkRegistration();
        var builder = new ZLinkFrameworkOptionsBuilder(registration);

        configure(builder);
        ZLinkFrameworkRegistrationValidator.Validate(registration);

        ZLinkFrameworkServiceRegistrar.AddFrameworkRuntime(services, registration);

        return services;
    }

    public static IServiceCollection AddZLinkHttpClient(
        this IServiceCollection services,
        string name,
        Action<ZLinkHttpClientBuilder> configure)
    {
        ArgumentNullException.ThrowIfNull(services);
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        ArgumentNullException.ThrowIfNull(configure);

        services.TryAddSingleton<IZLinkHttpExecutionScheduler, ZLinkSpotHttpExecutionScheduler>();
        services.AddKeyedSingleton<ZLinkHttpServerClient>(name, (provider, _) =>
        {
            var builder = ZLinkHttpClient.Create();
            configure(builder);
            return builder.BuildServer(provider.GetRequiredService<IZLinkHttpExecutionScheduler>());
        });
        return services;
    }

    public static IHealthChecksBuilder AddZLinkDrainHealthCheck(
        this IHealthChecksBuilder builder)
    {
        ArgumentNullException.ThrowIfNull(builder);

        return builder.AddCheck<ZLinkDrainHealthCheck>(
            "zlink-drain",
            failureStatus: HealthStatus.Unhealthy,
            tags: ["ready"]);
    }
}
