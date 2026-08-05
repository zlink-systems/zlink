using Microsoft.Extensions.Configuration;

using Systems.Zlink;
using AutomaticTurnDispatch.Server.Play.Handlers;
using AutomaticTurnDispatch.Server.Play.Spots;
using AutomaticTurnDispatch.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.E2E.Diagnostics;

namespace AutomaticTurnDispatch.Server.Play;

internal static class PlayHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = PlayOptions.Parse(args);
        Directory.CreateDirectory(options.LogDir);

        var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        builder.Logging.ClearProviders();
        builder.Logging.AddSimpleConsole(console =>
        {
            console.SingleLine = true;
            console.TimestampFormat = "HH:mm:ss.fff ";
        });
        builder.WebHost.UseUrls(options.HttpUrl);
        builder.Services.AddSingleton(new EvidenceStore(options.Rid, options.EvidenceFile));
        builder.Services.AddSingleton(new NodeOptions(options.Rid));
        builder.Services.AddSingleton(new E2eMessageFlowListener(
            Path.Combine(options.LogDir, $"{options.Rid}-flow.log"),
            options.Rid));
        builder.Services.AddZLinkHttpClient("external-api", http => http
            .BaseUrl(options.ExternalApiBaseUrl)
            .Timeout(TimeSpan.FromSeconds(5)));
        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            // Actor Join callbacks intentionally hold admission for 350 ms.
            // Leave deterministic time for transport and Store work.
            framework.DefaultRequestTimeout = TimeSpan.FromSeconds(2);
            framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = options.RedisKeyPrefix; }));
            framework.AddRelocationStore(new ZLinkRedisRelocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = $"{options.RedisKeyPrefix}:relocation"; }));
            framework.AddHandlersFromAssemblyOf(typeof(Program));
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            var controlMesh = framework.AddRouteMesh(AutomaticTurnDispatchNames.ControlChannel)
                .Listen(options.ControlEndpoint)
                .SetRoutingIdPrefix(options.Rid);
            controlMesh.Channel(AutomaticTurnDispatchNames.ControlChannel).Server();
            controlMesh
                .AddRouteRequestHandler<BindAwaitActorsControlHandler, BindAwaitActorsReq, BindAwaitActorsRes>(
                    "BindAwaitActorsReq")
                .AddRouteRequestHandler<EnsureSpotControlHandler, EnsureSpotReq, EnsureSpotRes>("EnsureSpotReq")
                .AddRouteRequestHandler<AwaitEvidenceControlHandler, AwaitEvidenceReq, AwaitEvidenceRes>(
                    "AwaitEvidenceReq")
                .AddRouteRequestHandler<AwaitEvidenceWaitControlHandler, AwaitEvidenceWaitReq, AwaitEvidenceRes>(
                    "AwaitEvidenceWaitReq");
            var delayMesh = framework.AddRouteMesh(AutomaticTurnDispatchNames.DelayChannel)
                .Listen("tcp://127.0.0.1:0")
                .SetRoutingId(RoutingId.From(options.Rid));
            delayMesh.Channel(AutomaticTurnDispatchNames.DelayChannel).Client();
            delayMesh.PeerConnections.Connect(options.DelayEndpoint);
            var spotRouteMesh = framework.AddRouteMesh(AutomaticTurnDispatchNames.SpotRouteChannel)
                .Listen(options.SpotRouteEndpoint)
                .SetRoutingIdPrefix(options.Rid);
            spotRouteMesh.Channel(AutomaticTurnDispatchNames.SpotRouteChannel).Server();
            var mesh24 = framework.AddRouteMesh(AutomaticTurnDispatchNames.SpotChannel)
                .Listen(options.SpotRouterEndpoint)
                .SetRoutingIdPrefix(options.Rid)
                .SetPlacementWeight(options.PlacementWeight);
            mesh24.Objects().Server()
                .AddEntrySpot<AwaitEntrySpot>()
                .AddActorFactory<AwaitActor, AwaitActorFactory>(
                    AutomaticTurnDispatchNames.ActorType,
                    factory => factory.RecreateOnRelocation())
                .AddSpotFactory<AwaitProbeSpot>(
                    AutomaticTurnDispatchNames.SpotType,
                    factory => factory.DisableRelocation())
                .AddSpotFactory<PerActorAwaitSpot>(
                    AutomaticTurnDispatchNames.PerActorSpotType,
                    factory => factory
                        .ExecutionMode(ZLinkUserSpotExecutionMode.PerActor)
                        .RecreateOnRelocation());
            mesh24.Channel(AutomaticTurnDispatchNames.SpotChannel).Server();
        });

        var app = builder.Build();
        app.MapGet("/health", () => Results.Ok(new { status = "ready", role = "play", options.Rid }));
        app.MapGet("/topology/ready", async (
            string meshName,
            string rid,
            IZLinkRouteMeshRuntime runtime,
            IZLinkRouteClient routes,
            CancellationToken cancellationToken) =>
        {
            if (string.Equals(
                    meshName,
                    AutomaticTurnDispatchNames.DelayChannel,
                    StringComparison.Ordinal))
            {
                try
                {
                    _ = await routes.RequestToChannel(
                            meshName,
                            new DelayReq($"readiness-{rid}", 0, "readiness"))
                        .Timeout(TimeSpan.FromMilliseconds(500))
                        .Async<DelayRes>(cancellationToken);
                    return Results.Ok(new { ready = true });
                }
                catch (Exception error) when (
                    error is ZLinkFrameworkException
                        or TimeoutException
                        or OperationCanceledException)
                {
                    return Results.Ok(new { ready = false });
                }
            }

            var snapshot = runtime.GetStatus(meshName);
            var ready = snapshot.Peers.Any(peer =>
                peer.State == ZLinkPeerState.Ready
                && (string.Equals(
                        peer.NodeRid.ToString(),
                        rid,
                        StringComparison.Ordinal)
                    || peer.NodeRid.ToString().StartsWith(
                        $"{rid}-",
                        StringComparison.Ordinal)));
            return Results.Ok(new { ready });
        });
        app.MapPost("/placement-weight", async (
            PlacementWeightReq request,
            IZLinkRouteMeshRuntimeOptions runtimeOptions,
            IZLinkSpotManager spots,
            NodeOptions node,
            CancellationToken cancellationToken) =>
        {
            var placement = runtimeOptions.Mesh(AutomaticTurnDispatchNames.SpotChannel);
            placement.PlacementWeight = request.Weight;
            if (request.Weight > 0 && request.VerifyLocal)
            {
                var deadline = DateTimeOffset.UtcNow.AddSeconds(10);
                var consecutiveLocal = 0;
                while (DateTimeOffset.UtcNow < deadline)
                {
                    ZLinkSpotCreateResult probe;
                    try
                    {
                        probe = await spots.GetOrCreate(
                                $"placement-probe-{Guid.NewGuid():N}",
                                AutomaticTurnDispatchNames.SpotType)
                            .Timeout(TimeSpan.FromSeconds(2))
                            .Async(cancellationToken);
                    }
                    catch (ZLinkFrameworkException error)
                        when (error.Kind is
                            ZLinkFrameworkErrorKind.Unavailable
                            or ZLinkFrameworkErrorKind.CapacityExceeded
                            or ZLinkFrameworkErrorKind.DeadlineExceeded)
                    {
                        await Task.Delay(25, cancellationToken);
                        continue;
                    }
                    var local = probe.Spot.NodeRid.ToString().StartsWith(
                        $"{node.Rid}-",
                        StringComparison.Ordinal);
                    await spots.CloseAsync(probe.Spot, cancellationToken);
                    consecutiveLocal = local ? consecutiveLocal + 1 : 0;
                    if (consecutiveLocal >= 4)
                        return Results.Ok(new PlacementWeightRes(placement.PlacementWeight));
                    await Task.Delay(25, cancellationToken);
                }

                return Results.StatusCode(StatusCodes.Status503ServiceUnavailable);
            }

            return Results.Ok(new PlacementWeightRes(placement.PlacementWeight));
        });
        app.MapGet("/evidence", (EvidenceStore evidence) => Results.Ok(evidence.Snapshot()));
        return app;
    }
}
