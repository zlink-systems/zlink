using Microsoft.Extensions.Configuration;

using DeliveryDispatch.Server.Configuration;
using DeliveryDispatch.Server.CustomerGateway.Spots.EntrySpot;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Samples.Logging;

namespace DeliveryDispatch.Server.CustomerGateway;

public static class CustomerGatewayHostFactory
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
            "customer-gateway");
        builder.Services.AddSingleton(configuration);
        builder.Services.AddSingleton(topology);
        builder.Services.AddSingleton<CustomerActorDirectory>();
        builder.Services.AddSingleton<CustomerActorAccess>();
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = topology.RedisEndpoint;
                redis.KeyPrefix = topology.RedisKeyPrefix;
            }));
            options.ConfigureDispatch()
                .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddHandlersFromAssemblyOf(typeof(CustomerGatewayHostFactory));
            var mesh = options.AddRouteMesh(SampleNames.CustomerMeshName)
                .Listen(topology.MeshEndpoint)
                .SetRoutingIdPrefix("customer-gateway");
            mesh.Objects().Server()
                .AddEntrySpot<CustomerEntrySpot>()
                .AddActorFactory<CustomerActor, CustomerActorFactory>(
                    SampleNames.CustomerActorType, factory => factory.DisableRelocation());
            options.AddStreamNode(SampleNames.CustomerStreamNode)
                .Bind(topology.CustomerStreamEndpoint)
                .EnableActorDispatch()
                .AddSession<CustomerSession>();
        });

        return builder.Build();
    }
}
