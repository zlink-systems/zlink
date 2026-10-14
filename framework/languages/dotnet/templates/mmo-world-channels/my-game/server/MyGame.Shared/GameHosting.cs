using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.AspNetCore.Builder;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;

namespace MyGame.Shared;

public static class GameHosting
{
    public static void Configure(
        HostApplicationBuilder builder,
        NodeSettings settings,
        Action<IZLinkFrameworkOptions> configure
    )
    {
        builder.Services.AddSingleton(settings);
        ConfigureCore(builder.Services, builder.Logging, options =>
        {
            options.AddLocationStore(
                new ZLinkRedisLocationStore(redis =>
                {
                    redis.ConnectionString = settings.RedisEndpoint;
                    redis.KeyPrefix = settings.KeyPrefix;
                })
            );
            options.AddRelocationStore(
                new ZLinkRedisRelocationStore(redis =>
                {
                    redis.ConnectionString = settings.RedisEndpoint;
                    redis.KeyPrefix = settings.KeyPrefix + GameNames.RelocationKeyPrefix;
                })
            );
            configure(options);
        });
    }

    public static void ConfigureAccounts(WebApplicationBuilder builder) =>
        ConfigureCore(builder.Services, builder.Logging, _ => { });

    private static void ConfigureCore(IServiceCollection services, ILoggingBuilder logging,
        Action<IZLinkFrameworkOptions> configure)
    {
        logging.ClearProviders();
        logging.AddConsole(options => options.LogToStandardErrorThreshold = LogLevel.Trace);
        logging.AddSimpleConsole(options => options.SingleLine = true);
        services.AddZLinkFramework(options =>
        {
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            configure(options);
        });
    }
}
