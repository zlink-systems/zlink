using Microsoft.Extensions.Configuration;

using System.Diagnostics;
using SpotService.Server.Session.Handlers;
using SpotService.Server.Session.Spots;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Errors;

using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Diagnostics;

namespace SpotService.Server.Session;

internal static class SessionHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = ServerOptions.Parse(args, "session");
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
        var evidence = new EvidenceStore(options.Rid, options.EvidenceFile);
        builder.Services.AddSingleton(evidence);
        builder.Services.AddSingleton<SessionBindingProbeStore>();
        builder.Services.AddSingleton(new NodeOptions(options.Rid));
        builder.Services.AddSingleton(new E2eMessageFlowListener(
            Path.Combine(options.LogDir, $"{options.Rid}-flow.log"),
            options.Rid,
            flow =>
            {
                if (flow.Phase is not ("error" or "dropped")) return;
                evidence.Add(
                    "dispatch-error"
                    + $"|surface={flow.Surface}"
                    + $"|reason={flow.Reason}"
                    + $"|action={flow.Action}"
                    + $"|packet={flow.PacketName ?? "<null>"}");
            }));

        LocationStoreReadProbe? locationStoreReadProbe = null;
        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            if (!string.IsNullOrWhiteSpace(options.RedisEndpoint))
            {
                locationStoreReadProbe = new LocationStoreReadProbe(
                    new ZLinkRedisLocationStore(redis =>
                    {
                        redis.ConnectionString = options.RedisEndpoint;
                        redis.KeyPrefix = options.RedisKeyPrefix
                                          ?? throw new InvalidOperationException(
                                              "Shared.RedisKeyPrefix is required.");
                    }));
                framework.AddLocationStore(locationStoreReadProbe);
                framework.AddRelocationStore(new ZLinkRedisRelocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = $"{options.RedisKeyPrefix}:relocation"; }));
                // Crash-recovery scenarios re-claim actors from a killed
                // node; a short owner lease keeps that takeover window
                // within the scenario's patience.
                var locations = framework.ConfigureLocations();
                locations.OwnerLeaseRenewInterval = TimeSpan.FromSeconds(1);
                locations.OwnerLeaseTtl = TimeSpan.FromSeconds(10);
                locations.PollingInterval = TimeSpan.FromMilliseconds(500);
            }
            framework.AddHandlersFromAssemblyOf(typeof(Program));
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            var controlMesh = framework.AddRouteMesh(SpotServiceNames.ControlChannel)
                .Listen(Require(options.ControlEndpoint, "ControlEndpoint"))
                .SetRoutingIdPrefix(options.Rid);
            controlMesh.Channel(SpotServiceNames.ControlChannel).Server();
            controlMesh
                .AddRouteRequestHandler<EnsureActorHandler>()
                .AddRouteRequestHandler<ControlPingHandler>()
                .AddRouteRequestHandler<CreateSpotHandler>()
                .AddRouteRequestHandler<CloseSpotHandler>()
                .AddRouteRequestHandler<SpotTypeMismatchHandler>();
            var mesh22 = framework.AddRouteMesh(SpotServiceNames.SpotChannel)
                .Listen(Require(options.SpotRouterEndpoint, "SpotRouterEndpoint"))
                .SetRoutingIdPrefix(options.Rid)
                .SetPlacementWeight(0);
            if (!string.IsNullOrWhiteSpace(options.SpotRouterAdvertiseHost))
            {
                mesh22.SetAdvertiseHost(options.SpotRouterAdvertiseHost);
            }
            mesh22.Objects().Server()
                .AddEntrySpot<ScenarioEntrySpot>()
                .AddActorFactory<ScenarioActor, ScenarioActorFactory>(
                    SpotServiceNames.ActorType, factory => factory.RecreateOnRelocation())
                .AddSpotFactory<ScenarioUserSpot>(
                    SpotServiceNames.UserSpotType, factory => factory.DisableRelocation())
                .AddSpotFactory<ScenarioAlternateSpot>(
                    SpotServiceNames.AlternateSpotType, factory => factory.DisableRelocation())
                .AddSpotFactory<MultiNodeSpotA>(
                    SpotServiceNames.MultiSpotTypeA, factory => factory.DisableRelocation())
                .AddSpotFactory<MultiNodeSpotB>(
                    SpotServiceNames.MultiSpotTypeB, factory => factory.DisableRelocation());
            mesh22.Channel(SpotServiceNames.SpotChannel).Server();
            framework.AddStreamNode(SpotServiceNames.StreamNode)
                .Bind(Require(options.StreamEndpoint, "StreamEndpoint"))
                .EnableActorDispatch()
                .AddSession<ScenarioSession>();
            if (!string.IsNullOrWhiteSpace(options.TlsStreamEndpoint))
                framework.AddStreamNode(SpotServiceNames.TlsStreamNode)
                    .Bind(options.TlsStreamEndpoint)
                    .EnableActorDispatch()
                    .SetTlsServer(
                        Require(options.TlsCertPath, "TlsCertPath"),
                        Require(options.TlsKeyPath, "TlsKeyPath"))
                    .AddSession<ScenarioSession>();
        });

        var app = builder.Build();
        app.MapGet("/health", () => Results.Ok(new { status = "ready", options.Role, options.Rid }));
        app.MapPost("/location-store/read-probe/configure", (
            LocationStoreReadProbeReq request) =>
        {
            var probe = locationStoreReadProbe
                        ?? throw new InvalidOperationException(
                            "The Location Store read probe requires Redis.");
            probe.Configure(request.ActorIds, request.Blocked);
            return Results.Ok(probe.Snapshot());
        });
        app.MapGet("/location-store/read-probe", () =>
        {
            var probe = locationStoreReadProbe
                        ?? throw new InvalidOperationException(
                            "The Location Store read probe requires Redis.");
            return Results.Ok(probe.Snapshot());
        });
        app.MapPost("/placement-weight", (
            PlacementWeightReq request,
            IZLinkRouteMeshRuntimeOptions runtimeOptions) =>
        {
            runtimeOptions.Mesh(SpotServiceNames.SpotChannel).PlacementWeight = request.Weight;
            return Results.Ok(new PlacementWeightRes(request.Weight));
        });
        app.MapGet("/channel/spot-peer-ready/{targetRid}", (
            string targetRid,
            IZLinkRouteMeshRuntime meshRuntime) =>
        {
            var peer = ResolveReadyPeer(
                meshRuntime,
                SpotServiceNames.SpotChannel,
                targetRid);
            return peer is not null
                ? Results.Ok(peer)
                : Results.StatusCode(StatusCodes.Status503ServiceUnavailable);
        });
        app.MapGet("/evidence", (EvidenceStore evidence) => Results.Ok(evidence.Snapshot()));
        app.MapPost("/actor/request", async (
            ActorRequestReq request,
            IZLinkActorClient actors,
            CancellationToken cancellationToken) =>
        {
            try
            {
                var reply = await actors
                    .RequestToActor(
                        request.ActorId,
                        new UserActorPingReq(request.Value))
                    // This E2E-only marker lets the transport proxy hold one
                    // already-resolved request without holding control frames.
                    .Metadata("x-zlink-e2e-transport-gate", request.Value)
                    .Timeout(TimeSpan.FromMilliseconds(
                        Math.Clamp(
                            request.TimeoutMilliseconds,
                            1,
                            30000)))
                    .Async<ActorPingRes>(cancellationToken);
                return Results.Ok(new ActorRequestRes(
                    true,
                    string.Empty,
                    reply));
            }
            catch (ZLinkFrameworkException error)
            {
                return Results.Ok(new ActorRequestRes(
                    false,
                    error.Kind.ToString(),
                    null));
            }
            catch (TimeoutException)
            {
                return Results.Ok(new ActorRequestRes(
                    false,
                    "Timeout",
                    null));
            }
        });
        app.MapPost("/evidence/wait", async (
            EvidenceWaitReq request,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            var snapshot = await evidence.WaitUntilAsync(
                entries => request.ContainsAll.All(expected =>
                    entries.Any(entry => entry.Contains(expected, StringComparison.Ordinal))),
                timeout,
                cancellationToken);
            return Results.Ok(snapshot);
        });
        app.MapPost("/channel/control-ping/{targetRid}", async (
            string targetRid,
            ControlPingReq request,
            IZLinkRouteClient route,
            IZLinkRouteMeshRuntime meshRuntime) =>
        {
            var reply = await route.RequestToNode(
                    SpotServiceNames.ControlChannel,
                    ResolvePeerRoutingId(
                        meshRuntime,
                        SpotServiceNames.ControlChannel,
                        targetRid),
                    request)
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<ControlPingRes>();
            return Results.Ok(reply);
        });
        app.MapPost("/shutdown", (IHostApplicationLifetime lifetime) =>
        {
            lifetime.StopApplication();
            return Results.Ok(new { status = "stopping" });
        });
        return app;
    }

    internal static RoutingId ResolvePeerRoutingId(
        IZLinkRouteMeshRuntime meshRuntime,
        string meshName,
        string ridOrPrefix)
    {
        var peer = ResolveReadyPeer(meshRuntime, meshName, ridOrPrefix);
        return peer?.NodeRid
               ?? throw new InvalidOperationException(
                   $"Mesh '{meshName}' has no Ready node for prefix '{ridOrPrefix}'.");
    }

    private static ZLinkPeerStatus? ResolveReadyPeer(
        IZLinkRouteMeshRuntime meshRuntime,
        string meshName,
        string ridOrPrefix)
    {
        var readyPeers = meshRuntime.GetStatus(meshName).Peers.Where(candidate =>
        {
            var candidateRid = candidate.NodeRid.ToString();
            return candidate.State == ZLinkPeerState.Ready
                   && (string.Equals(candidateRid, ridOrPrefix, StringComparison.Ordinal)
                       || candidateRid.StartsWith(
                           $"{ridOrPrefix}-",
                           StringComparison.Ordinal));
        }).ToArray();

        return readyPeers.Length switch
        {
            0 => null,
            1 => readyPeers[0],
            _ => throw new InvalidOperationException(
                $"Mesh '{meshName}' has more than one Ready node for prefix "
                + $"'{ridOrPrefix}': "
                + string.Join(", ", readyPeers.Select(
                    static peer => peer.NodeRid.ToString()))
                + ".")
        };
    }

    private static string Require(string? value, string optionName)
    {
        return string.IsNullOrWhiteSpace(value)
            ? throw new InvalidOperationException($"{optionName} is required.")
            : value;
    }
}
