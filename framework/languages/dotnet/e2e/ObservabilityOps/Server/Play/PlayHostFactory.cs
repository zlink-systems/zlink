using Microsoft.Extensions.Configuration;

using ObservabilityOps.Server.Play.Infrastructure;
using ObservabilityOps.Server.Play.Spots;
using ObservabilityOps.Server.Play.Support;
using ObservabilityOps.Server.Support;
using ObservabilityOps.Shared;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Diagnostics;

namespace ObservabilityOps.Server.Play;

internal static class PlayHostFactory
{
    private const string RoomSpotType = "observability-room";
    private const string InstanceSpotType = "observability-instance";

    public static WebApplication Create(string[] args)
    {
        var options = PlayOptions.Parse(args);
        Directory.CreateDirectory(options.LogDir);
        var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        builder.Logging.ClearProviders();
        builder.Logging.AddSimpleConsole(console => console.SingleLine = true);
        builder.WebHost.UseUrls(options.HttpUrl);
        builder.Services.AddSingleton<EvidenceStore>();
        builder.Services.AddSingleton<RelocationOperation>();
        builder.Services.AddSingleton<ShutdownOperation>();
        builder.Services.AddSingleton<BoundedOperationGate>();
        builder.Services.AddSingleton<SpotClosingGate>();
        if (options.MetricsEnabled) builder.Services.AddSingleton<MetricEvidenceCollector>();
        builder.Services.AddSingleton(new E2eMessageFlowListener(
            Path.Combine(options.LogDir, $"flow-{options.Rid}.log"),
            options.Rid));
        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            framework.ApplicationVersion = options.ApplicationVersion;
            framework.MaintenanceWave = options.MaintenanceWave;
            framework.DefaultRequestTimeout = TimeSpan.FromSeconds(15);
            framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = options.RedisKeyPrefix; }));
            framework.AddRelocationStore(new ZLinkRedisRelocationStore(redis =>
            {
                redis.ConnectionString = options.RedisEndpoint;
                redis.KeyPrefix = $"{options.RedisKeyPrefix}:relocation";
            }));
            var locations = framework.ConfigureLocations();
            locations.RouteCacheMaxAge = TimeSpan.Zero;
            locations.MessageFollowDuration = TimeSpan.FromSeconds(5);
            locations.OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(options.LocationHeartbeatMs);
            locations.OwnerLeaseTtl = TimeSpan.FromMilliseconds(options.LocationLeaseTtlMs);
            locations.PollingInterval = TimeSpan.FromMilliseconds(250);
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            framework.AddHandlersFromAssemblyOf(typeof(PlayHostFactory));
            var mesh18 = framework.AddRouteMesh(ObservabilityNames.PlayMesh)
                .Listen(options.RouterEndpoint)
                .SetPlacementWeight(options.PlacementWeight)
                .SetActorLimit(128)
                .SetSpotLimit(128)
                .SetActivationConcurrency(32);
            mesh18.Objects().Server()
                .AddEntrySpot<PlayEntrySpot>()
                .AddActorFactory<PlayerActor, PlayerActorFactory>(
                    ObservabilityNames.PlayerActorType, factory => factory.PreserveStateWith<PlayerActorRelocationAdapter>())
                .AddSpotFactory<RoomSpot>(
                    RoomSpotType,
                    factory => factory
                        .StableTypeLimit(128)
                        .PreserveStateWith<RoomSpotRelocationAdapter>())
                .AddInstanceSpotFactory<PlayInstanceSpot>(
                    InstanceSpotType,
                    factory => factory
                        .StableTypeLimit(128)
                        .PreserveStateWith<PlayInstanceSpotRelocationAdapter>());
            if (!string.IsNullOrWhiteSpace(options.ManualPeerEndpoint))
                mesh18.PeerConnections.Connect(options.ManualPeerEndpoint);
            mesh18.Channel(ObservabilityNames.PlayMesh).Client();
        });
        builder.Services.AddHostedService(provider =>
            new Support.HostStateEvidenceObserver(
                provider.GetRequiredService<IZLinkFrameworkRuntime>(),
                provider.GetRequiredService<IZLinkRouteMeshRuntime>(),
                provider.GetRequiredService<EvidenceStore>(),
                options.Rid));

        var app = builder.Build();
        if (options.MetricsEnabled) _ = app.Services.GetRequiredService<MetricEvidenceCollector>();
        app.MapGet("/health", () => Results.Ok(new
        {
            status = "ready",
            options.Rid,
            options.ApplicationVersion
        }));
        app.MapGet("/identity", async (
            IZLinkLocationRuntimeQuery locations,
            CancellationToken cancellationToken) =>
        {
            var topology = await locations.ListTopologyAsync(
                new ZLinkLocationTopologyFilter(ObservabilityNames.PlayMesh),
                cancellationToken: cancellationToken);
            var local = topology.Items.SingleOrDefault(row =>
                string.Equals(
                    row.Endpoint,
                    options.RouterEndpoint,
                    StringComparison.Ordinal));
            return local is null
                ? Results.StatusCode(StatusCodes.Status503ServiceUnavailable)
                : Results.Ok(new NodeIdentityRes(
                    options.Rid,
                    local.NodeRid.ToString()));
        });
        app.MapGet("/relocation/readiness-evidence", async (
            IZLinkLocationRuntimeQuery locations,
            IZLinkRouteMeshRuntime routeMesh,
            CancellationToken cancellationToken) =>
        {
            var topology = await locations.ListTopologyAsync(
                new ZLinkLocationTopologyFilter(ObservabilityNames.PlayMesh),
                cancellationToken: cancellationToken);
            var status = routeMesh.GetStatus(ObservabilityNames.PlayMesh);
            return Results.Ok(new
            {
                routeMesh = new
                {
                    status.MeshName,
                    status.State,
                    status.IsReady,
                    status.ReadyPeerCount,
                    peers = status.Peers.Select(peer => new
                    {
                        nodeRid = peer.NodeRid.ToString(),
                        peer.State,
                        peer.UnavailableReason
                    })
                },
                topology = topology.Items.Select(item => new
                {
                    item.MeshName,
                    nodeRid = item.NodeRid.ToString(),
                    item.Endpoint,
                    item.Draining,
                    item.State,
                    item.UpdatedAt
                })
            });
        });
        app.MapDiagnosticsControl();
        app.MapPost("/rooms", async (CreateRoomReq request, IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            var created = await spots.GetOrCreate(request.RoomRid, RoomSpotType)
                .InMesh(ObservabilityNames.PlayMesh)
                .Request(request)
                .Async(cancellationToken);
            return Results.Ok(new CreateRoomRes(
                created.Spot.SpotId,
                created.Spot.NodeRid.ToString()));
        });
        app.MapPost("/placement-weight", (
            PlacementWeightReq request,
            IZLinkRouteMeshRuntimeOptions runtimeOptions) =>
        {
            runtimeOptions.Mesh(ObservabilityNames.PlayMesh).PlacementWeight =
                request.Weight;
            return Results.Ok(new PlacementWeightRes(request.Weight));
        });
        app.MapPost("/rooms/{roomRid}/close", async (string roomRid, IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            var room = await spots.FindAsync(roomRid, cancellationToken);
            return Results.Ok(new
            {
                closed = room is not null
                         && await spots.CloseAsync(room.Value, cancellationToken)
            });
        });
        app.MapPost("/spots/{spotId}/close", async (
            string spotId,
            IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            var spot = await spots.FindAsync(spotId, cancellationToken);
            return Results.Ok(spot is not null
                              && await spots.CloseAsync(
                                  spot.Value, cancellationToken));
        });
        app.MapPost("/instances/{spotId}", async (
            string spotId,
            ActivateInstanceSpotReq request,
            IZLinkSpotClient spots,
            CancellationToken cancellationToken) =>
        {
            var response = await spots.RequestToSpot(spotId, request)
                .InstanceSpot(InstanceSpotType)
                .InMesh(ObservabilityNames.PlayMesh)
                .Async<ActivateInstanceSpotRes>(cancellationToken);
            return Results.Ok(response);
        });
        app.MapGet("/evidence", CreateEvidenceAsync);
        app.MapPost("/evidence/wait", async (EvidenceWaitReq request, EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            var entries = await evidence.WaitAsync(snapshot => Matches(snapshot, request), timeout, cancellationToken);
            return Results.Ok(entries);
        });
        if (options.MetricsEnabled)
            app.MapPost("/metrics/wait", async (MetricWaitReq request, MetricEvidenceCollector metrics,
                CancellationToken cancellationToken) => Results.Ok(await metrics.WaitAsync(
                samples => MetricWait.Matches(samples, request),
                TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000)),
                cancellationToken)));
        app.MapMaintenanceOperations();
        app.MapBoundedOperationGate();
        app.MapSpotClosingGate();
        app.MapPost("/operation/start", async (
            PlayBoundedOperationReq request,
            IZLinkSpotClient routes,
            CancellationToken cancellationToken) =>
        {
            var response = await routes.RequestToSpot(request.RoomId, request)
                // The operation is intentionally accepted before relocation
                // seals admission. Its request deadline must cover the host
                // relocation deadline and the HTTP endpoint waiter.
                .Timeout(TimeSpan.FromSeconds(35))
                .Async<PlayBoundedOperationRes>(cancellationToken);
            return Results.Ok(response);
        });
        return app;

        async Task<IResult> CreateEvidenceAsync(
            IZLinkFrameworkRuntime runtime,
            IZLinkLocationRuntimeQuery locations,
            IZLinkSpotManager spots,
            IZLinkActorManager actors,
            EvidenceStore evidence,
            IServiceProvider services,
            string? spotRid,
            string? actorId,
            CancellationToken cancellationToken)
        {
            var peers = await locations.ListTopologyAsync(
                new ZLinkLocationTopologyFilter(ObservabilityNames.PlayMesh),
                cancellationToken: cancellationToken);
            // Topology reports nodes. Spot and Actor details are resolved
            // through their public identity-based APIs.
            var actorRows = Array.Empty<ActorRow>();
            if (!string.IsNullOrWhiteSpace(actorId)
                && await actors.FindAsync(actorId, cancellationToken) is { } actorRef)
                actorRows =
                [
                    new ActorRow(
                        actorId,
                        actorRef.NodeRid.ToString(),
                        checked((long)actorRef.ObjectGeneration))
                ];
            var spotRows = Array.Empty<SpotRow>();
            if (!string.IsNullOrWhiteSpace(spotRid)
                && await spots.FindAsync(spotRid, cancellationToken)
                    is { } spot)
                spotRows =
                [
                    new SpotRow(
                        spot.MeshName,
                        spot.NodeRid.ToString(),
                        spot.SpotId,
                        "spot",
                        checked((long)spot.ObjectGeneration))
                ];
            return Results.Ok(new EvidenceSnapshot(
                options.Rid, runtime.Status.IsReady, evidence.Snapshot(),
                services.GetService<MetricEvidenceCollector>()?.Snapshot() ?? [],
                peers.Items.Select(row => new PeerRow(row.NodeRid.ToString(),
                    row.Draining, row.UpdatedAt.UtcTicks)).ToArray(),
                actorRows,
                spotRows));
        }
    }

    private static bool Matches(string[] entries, EvidenceWaitReq request) =>
        request.ContainsAll.All(expected => entries.Any(entry => entry.Contains(expected, StringComparison.Ordinal)))
        && request.ContainsAnyGroups.All(group => group.Any(expected =>
            entries.Any(entry => entry.Contains(expected, StringComparison.Ordinal))));
}
