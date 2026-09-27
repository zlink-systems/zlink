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
        string[]? args = null
    )
    {
        var builder = WebApplication.CreateBuilder(args ?? []);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        SampleLogging.Configure(builder.Logging, logDirectory, instance.InstanceId);

        builder.WebHost.UseUrls(instance.HttpUrl);
        builder.Services.AddSingleton(topology);
        builder.Services.AddSingleton(instance);
        builder.Services.AddSingleton(new RedisCommerceStores(topology));
        builder.Services.AddSingleton<IOrderEventStore>(static provider =>
            provider.GetRequiredService<RedisCommerceStores>()
        );
        builder.Services.AddSingleton<IOrderReadModelStore>(static provider =>
            provider.GetRequiredService<RedisCommerceStores>()
        );
        builder.Services.AddSingleton<ICommerceStateStore>(static provider =>
            provider.GetRequiredService<RedisCommerceStores>()
        );
        builder.Services.AddSingleton<OrderWorkflowService>();
        builder.Services.AddSingleton<OrderWorkflowSelfCheckService>();
        builder.Services.AddSingleton<ShoppingMallPlannedRelocation>();
        builder.Services.AddHostedService(static provider =>
            provider.GetRequiredService<ShoppingMallPlannedRelocation>()
        );

        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(
                new ZLinkRedisLocationStore(redis =>
                {
                    redis.ConnectionString = topology.RedisEndpoint;
                    redis.KeyPrefix = topology.RedisKeyPrefix;
                })
            );
            options.AddRelocationStore(
                new ZLinkRedisRelocationStore(redis =>
                {
                    redis.ConnectionString = topology.RedisEndpoint;
                    redis.KeyPrefix = $"{topology.RedisKeyPrefix}relocation:";
                })
            );
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            options.AddHandlersFromAssemblyOf(typeof(OrderWorkflowServerHostFactory));
            // --8<-- [start:doc-sm-workflow-register]
            var mesh = options
                .AddRouteMesh(SampleNames.MeshName)
                .Listen(instance.MeshEndpoint)
                .SetRoutingIdPrefix("order-workflow");
            mesh.Objects()
                .Server()
                .AddInstanceSpotFactory<OrderWorkflowSpot>(
                    SampleNames.OrderWorkflowSpotType,
                    factory => factory.RecreateOnRelocation()
                );
            mesh.Channel(SampleNames.OrderProjectionChannel).Server();
            // --8<-- [end:doc-sm-workflow-register]
            //  Peers are found through the Location Store. Adding a manual peer connection here
            //  switches the node to manual acquisition, which conflicts with the routing ID prefix
            //  this mesh uses ("can use a routing ID prefix only with automatic discovery").
        });

        var app = builder.Build();
        app.Use(
            async (context, next) =>
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
                            error.Message
                        );
                    throw;
                }
            }
        );
        app.MapGet(
            "/health",
            () => Results.Ok(new { ready = true, instance = instance.InstanceId })
        );
        app.MapGet(
            "/self-check/mesh-ready",
            async (
                IZLinkRouteMeshRuntime routeMesh,
                IZLinkLocationRuntimeQuery locations,
                CancellationToken cancellationToken
            ) =>
            {
                var status = routeMesh.GetStatus(SampleNames.MeshName);
                var otherEndpoint = string.Equals(
                    instance.InstanceId,
                    "workflow-a",
                    StringComparison.Ordinal
                )
                    ? topology.WorkflowBMeshEndpoint
                    : topology.WorkflowAMeshEndpoint;
                var registered = await locations.ListTopologyAsync(
                    new ZLinkLocationTopologyFilter(SampleNames.MeshName),
                    cancellationToken: cancellationToken
                );
                var other = registered.Items.SingleOrDefault(entry =>
                    string.Equals(entry.Endpoint, otherEndpoint, StringComparison.Ordinal)
                );
                var ready =
                    status.IsReady
                    && other is not null
                    && status.Peers.Any(peer =>
                        peer.NodeRid == other.NodeRid && peer.State == ZLinkPeerState.Ready
                    );
                return Results.Ok(new { ready });
            }
        );
        app.MapPost(
            "/self-check/relocate/{orderId}",
            async (
                string orderId,
                IZLinkLocationRuntimeQuery locations,
                OrderWorkflowSelfCheckService selfChecks,
                ShoppingMallPlannedRelocation relocation,
                CancellationToken cancellationToken
            ) =>
            {
                var location = await locations.FindSpotLocationAsync(orderId, cancellationToken);
                var registeredTopology = await locations.ListTopologyAsync(
                    new ZLinkLocationTopologyFilter(SampleNames.MeshName),
                    cancellationToken: cancellationToken
                );
                if (location is null)
                    return Results.Ok(new PlannedRelocationRes(false, "OrderNotFound", "None"));
                var local = registeredTopology.Items.SingleOrDefault(entry =>
                    string.Equals(entry.Endpoint, instance.MeshEndpoint, StringComparison.Ordinal)
                );
                if (local is null || location.NodeRid != local.NodeRid)
                    return Results.Ok(new PlannedRelocationRes(false, "NotOwner", "None"));

                await selfChecks.ArmPlannedRelocationReplayAsync(orderId, cancellationToken);
                var started = relocation.Start();

                return Results.Ok(
                    new PlannedRelocationRes(
                        true,
                        started ? "Started" : "AlreadyStarted",
                        "None",
                        SourceInstanceId: instance.InstanceId
                    )
                );
            }
        );
        app.MapGet(
            "/self-check/owner/{orderId}",
            async (
                string orderId,
                IZLinkLocationRuntimeQuery locations,
                CancellationToken cancellationToken
            ) =>
            {
                var location = await locations.FindSpotLocationAsync(orderId, cancellationToken);
                var topology = await locations.ListTopologyAsync(
                    new ZLinkLocationTopologyFilter(SampleNames.MeshName),
                    cancellationToken: cancellationToken
                );
                var local = topology.Items.SingleOrDefault(entry =>
                    string.Equals(entry.Endpoint, instance.MeshEndpoint, StringComparison.Ordinal)
                );
                return Results.Ok(
                    new
                    {
                        isOwner = location is not null
                            && local is not null
                            && location.NodeRid == local.NodeRid,
                    }
                );
            }
        );
        app.MapGet(
            "/self-check/relocation-status",
            (ShoppingMallPlannedRelocation relocation) =>
            {
                var status = relocation.Status();
                return Results.Ok(
                    new PlannedRelocationRes(
                        true,
                        status.Outcome,
                        status.Reason,
                        State: status.State
                    )
                );
            }
        );
        return app;
    }

    private sealed record PlannedRelocationRes(
        bool IsOwner,
        string Outcome,
        string Reason,
        string? State = null,
        string? SourceInstanceId = null
    );
}

internal sealed class ShoppingMallPlannedRelocation(IZLinkFrameworkRuntime runtime)
    : BackgroundService
{
    private readonly object _gate = new();
    private readonly TaskCompletionSource _requested = new(
        TaskCreationOptions.RunContinuationsAsynchronously
    );
    private Task<ZLinkFrameworkRelocationResult>? _operation;
    private bool _started;

    public bool Start()
    {
        lock (_gate)
        {
            if (_started)
                return false;
            _started = true;
            _requested.TrySetResult();
            return true;
        }
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        await _requested.Task.WaitAsync(stoppingToken).ConfigureAwait(false);
        Task<ZLinkFrameworkRelocationResult> operation;
        lock (_gate)
        {
            _operation = runtime
                .RelocateAsync(
                    new ZLinkFrameworkRelocationOptions
                    {
                        Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                        Deadline = TimeSpan.FromSeconds(30),
                    }
                )
                .AsTask();
            operation = _operation;
        }
        await operation.ConfigureAwait(false);
    }

    public (string Outcome, string Reason, string State) Status()
    {
        lock (_gate)
        {
            if (_operation is null)
                return ("NotStarted", "None", runtime.Status.State.ToString());
            if (!_operation.IsCompleted)
                return ("InProgress", "None", runtime.Status.State.ToString());
            if (_operation.IsFaulted)
                return (
                    "Blocked",
                    _operation.Exception?.GetBaseException().Message ?? "RelocationFailed",
                    runtime.Status.State.ToString()
                );
            var result = _operation.GetAwaiter().GetResult();
            return (
                result.Outcome.ToString(),
                result.Reason.ToString(),
                runtime.Status.State.ToString()
            );
        }
    }
}
