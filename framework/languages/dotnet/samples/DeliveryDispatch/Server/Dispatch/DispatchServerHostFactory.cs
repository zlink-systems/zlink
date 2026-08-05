using Microsoft.Extensions.Configuration;

using DeliveryDispatch.Server.Configuration;
using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Locations;
using Zlink.Samples.Logging;

namespace DeliveryDispatch.Server.Dispatch;

public static class DispatchServerHostFactory
{
    public static WebApplication Build(SampleConfiguration configuration)
    {
        var topology = configuration.Topology;
        var builder = WebApplication.CreateBuilder();
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        SampleLogging.Configure(
            builder.Logging,
            configuration.Role.LogDir,
            "dispatch");
        builder.WebHost.UseUrls(topology.DispatchHttpUrl);
        builder.Services.AddSingleton(configuration);
        builder.Services.AddSingleton(topology);
        builder.Services.AddSingleton<DispatchWorkQueue>();
        builder.Services.AddSingleton<DeliveryOfferStore>();
        builder.Services.AddSingleton<CourierSelectionPolicy>();
        builder.Services.AddSingleton<CourierOfferPort>();
        builder.Services.AddSingleton<DeliveryStatusPublisher>();
        builder.Services.AddSingleton<DispatchWorker>();
        builder.Services.AddHostedService<DispatchQueuePump>();
        builder.Services.AddHostedService<OfferDeadlineSweeper>();
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
            {
                redis.ConnectionString = topology.RedisEndpoint;
                redis.KeyPrefix = topology.RedisKeyPrefix;
            }));
            options.ConfigureDispatch()
                .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddHandlersFromAssemblyOf(typeof(DispatchServerHostFactory));
            var mesh = options.AddRouteMesh(SampleNames.CourierMeshName)
                .Listen(topology.MeshEndpoint)
                .SetRoutingIdPrefix("delivery-dispatch");
            mesh.Objects().Client();
            var dispatchChannel = options.AddClientServerChannel(SampleNames.DispatchChannel);
            dispatchChannel.Client();
            dispatchChannel.Server()
                .Listen()
                .AddHandlerGroup(SampleNames.DispatchChannel);
            options.AddClientServerChannel(SampleNames.TrackingRouteChannel).Client();
        });

        var app = builder.Build();
        app.MapGet("/health", async (
            IZLinkLocationReadiness readiness,
            CancellationToken cancellationToken) =>
        {
            var courierOwnerReady = await readiness.IsPeerReadyAsync(
                SampleNames.CourierMeshName,
                ZLinkLocationRole.Spot,
                nodeRid: null,
                cancellationToken);

            return courierOwnerReady
                ? Results.Ok(new { ready = true, role = "dispatch" })
                : Results.StatusCode(StatusCodes.Status503ServiceUnavailable);
        });
        app.MapPost("/deliveries", async (
            CreateDeliveryReq request,
            Zlink.Framework.Contracts.Channels.IZLinkRouteClient channels,
            ILoggerFactory loggerFactory,
            CancellationToken cancellationToken) =>
        {
            var assign = new AssignDeliveryMsg(
                request.DeliveryId,
                request.CustomerId,
                request.PickupAddress,
                request.DropoffAddress);
            await channels.SendToChannel(SampleNames.DispatchChannel, assign)
                .Async(cancellationToken);
            loggerFactory.CreateLogger("DeliveryDispatch.Server.Dispatch")
                .LogInformation("deliverydispatch api: created delivery={DeliveryId}", request.DeliveryId);
            return Results.Ok(new CreateDeliveryRes(request.DeliveryId));
        });
        return app;
    }
}
