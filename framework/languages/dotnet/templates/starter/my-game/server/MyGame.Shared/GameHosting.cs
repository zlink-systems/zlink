using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;

namespace MyGame.Shared;

public static class GameHosting
{
    public static readonly TimeSpan MessageFollow = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan RelocationDeadline = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan SafeDeadline = MessageFollow + RelocationDeadline;

    public static void Configure(
        HostApplicationBuilder builder,
        NodeSettings settings,
        Action<IZLinkFrameworkOptions> configure
    )
    {
        builder.Logging.ClearProviders();
        builder.Logging.AddConsole(options => options.LogToStandardErrorThreshold = LogLevel.Trace);
        builder.Logging.AddSimpleConsole(options => options.SingleLine = true);
        builder.Services.AddSingleton(settings);
        builder.Services.AddZLinkFramework(options =>
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
                    redis.KeyPrefix = settings.KeyPrefix + "relocation:";
                })
            );
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.ConfigureLocations().MessageFollowDuration = MessageFollow;
            configure(options);
        });
    }
}
