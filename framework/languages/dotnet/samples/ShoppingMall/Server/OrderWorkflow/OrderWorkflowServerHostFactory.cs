using Microsoft.Extensions.Configuration;

using ShoppingMall.Server.Configuration;
using ShoppingMall.Server.OrderWorkflow.Application.OrderWorkflow;
using ShoppingMall.Server.OrderWorkflow.Application.SelfCheck;
using ShoppingMall.Server.OrderWorkflow.Infrastructure.ZLink.Spots.OrderWorkflowSpot;
using ShoppingMall.Server.Shared.Ports.Outbound;
using ShoppingMall.Server.Shared.Store;
using ShoppingMall.Shared.Contracts;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Locations.Redis;
using Zlink.Samples.Logging;

namespace ShoppingMall.Server.OrderWorkflow;

public static class OrderWorkflowServerHostFactory
{
    public static WebApplication Build(
        SampleTopology topology,
        WorkflowInstanceTopology instance,
        string logDirectory,
        string[]? args = null)
    {
        var builder = WebApplication.CreateBuilder(args ?? []);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        SampleLogging.Configure(
            builder.Logging,
            logDirectory,
            instance.InstanceId);

        builder.WebHost.UseUrls(instance.HttpUrl);
        builder.Services.AddSingleton(topology);
        builder.Services.AddSingleton(instance);
        builder.Services.AddSingleton(new RedisCommerceStores(topology));
        builder.Services.AddSingleton<IOrderEventStore>(static provider =>
            provider.GetRequiredService<RedisCommerceStores>());
        builder.Services.AddSingleton<IOrderReadModelStore>(static provider =>
            provider.GetRequiredService<RedisCommerceStores>());
        builder.Services.AddSingleton<ICommerceStateStore>(static provider =>
            provider.GetRequiredService<RedisCommerceStores>());
        builder.Services.AddSingleton<OrderWorkflowService>();
        builder.Services.AddSingleton<OrderWorkflowSelfCheckService>();

        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = topology.RedisEndpoint;
                redis.KeyPrefix = topology.RedisKeyPrefix;
            }));
            options.AddRelocationStore(new ZLinkRedisRelocationStore(redis =>
            {
                redis.ConnectionString = topology.RedisEndpoint;
                redis.KeyPrefix = $"{topology.RedisKeyPrefix}relocation:";
            }));
            options.ConfigureDispatch()
                .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddHandlersFromAssemblyOf(typeof(OrderWorkflowServerHostFactory));
            var mesh = options.AddRouteMesh(SampleNames.MeshName)
                .Listen(instance.MeshEndpoint)
                .SetRoutingIdPrefix("order-workflow");
            mesh.Objects().Server().AddInstanceSpotFactory<OrderWorkflowSpot>(
                SampleNames.OrderWorkflowSpotType, factory => factory.RecreateOnRelocation());
            mesh.Channel(SampleNames.OrderProjectionChannel).Server();
        });

        var app = builder.Build();
        app.Use(async (context, next) =>
        {
            try
            {
                await next(context);
            }
            catch (Exception error)
            {
                app.Services.GetRequiredService<ILoggerFactory>()
                    .CreateLogger("ShoppingMall.Server.OrderWorkflow")
                    .LogError(
                        error,
                        "shoppingmall workflow http handler failed: endpoint={Endpoint} error={Error}",
                        context.Request.Path.Value,
                        error.Message);
                throw;
            }
        });
        app.MapGet("/health", () => Results.Ok(new { ready = true, instance = instance.InstanceId }));
        app.MapPost("/self-check/projection/{orderId}/delete", async (
            string orderId,
            IOrderReadModelStore readModels,
            CancellationToken cancellationToken) =>
        {
            await readModels.DeleteAsync(orderId, cancellationToken);
            return Results.Ok();
        });
        app.MapPost("/self-check/projection/{orderId}/rebuild", async (
            string orderId,
            IZLinkSpotClient routes,
            CancellationToken cancellationToken) =>
        {
            var response = await routes
                .RequestToSpot(orderId, new RebuildOrderProjectionReq(orderId))
                .InstanceSpot(SampleNames.OrderWorkflowSpotType)
                .InMesh(SampleNames.MeshName)
                .Async<RebuildOrderProjectionRes>(cancellationToken);
            return Results.Ok(response);
        });
        return app;
    }
}
