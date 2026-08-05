using Microsoft.Extensions.Configuration;

using DeliveryDispatch.Server.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Samples.Logging;

namespace DeliveryDispatch.Server.CourierSession;

public static class CourierSessionHostFactory
{
    public static IHost Build(SampleConfiguration configuration)
    {
        var topology = configuration.Topology;
        var builder = Host.CreateApplicationBuilder();
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        SampleLogging.Configure(
            builder.Logging,
            configuration.Role.LogDir,
            "courier-session");
        builder.Services.AddSingleton(configuration);
        builder.Services.AddSingleton(topology);
        builder.Services.AddSingleton<CourierSessionBinder>();
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = topology.RedisEndpoint;
                redis.KeyPrefix = topology.RedisKeyPrefix;
            }));
            options.ConfigureDispatch()
                .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddHandlersFromAssemblyOf(typeof(CourierSessionHostFactory));
            var mesh = options.AddRouteMesh(SampleNames.CourierMeshName)
                .Listen(topology.MeshEndpoint)
                .SetRoutingIdPrefix("courier-session");
            mesh.Objects().Client();
            options.AddStreamNode(SampleNames.CourierStreamNode)
                .Bind(topology.CourierStreamEndpoint)
                .EnableActorDispatch()
                .AddSession<CourierSession>();
        });

        return builder.Build();
    }
}
