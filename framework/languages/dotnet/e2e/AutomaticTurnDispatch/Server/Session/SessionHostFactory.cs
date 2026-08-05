using Microsoft.Extensions.Configuration;

using Systems.Zlink;
using AutomaticTurnDispatch.Server.Session.Support;
using AutomaticTurnDispatch.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.E2E.Diagnostics;
using SessionServerOptions = AutomaticTurnDispatch.Server.Session.Support.SessionOptions;
using AwaitStreamSession = AutomaticTurnDispatch.Server.Session.Support.AwaitSession;

namespace AutomaticTurnDispatch.Server.Session;

internal static class SessionHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = SessionServerOptions.Parse(args);
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
        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = options.RedisKeyPrefix; }));
            framework.AddRelocationStore(new ZLinkRedisRelocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = $"{options.RedisKeyPrefix}:relocation"; }));
            framework.AddHandlersFromAssemblyOf(typeof(Program));
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            var controlMesh = framework.AddRouteMesh(AutomaticTurnDispatchNames.ControlChannel)
                .Listen(options.ControlEndpoint)
                .SetRoutingIdPrefix(options.Rid);
            controlMesh.Channel(AutomaticTurnDispatchNames.ControlChannel).Client();
            var spotRouteMesh = framework.AddRouteMesh(AutomaticTurnDispatchNames.SpotRouteChannel)
                .Listen("tcp://127.0.0.1:0")
                .SetRoutingIdPrefix(options.Rid);
            spotRouteMesh.Channel(AutomaticTurnDispatchNames.SpotRouteChannel).Server().SetWeight(0);
            var mesh23 = framework.AddRouteMesh(AutomaticTurnDispatchNames.SpotChannel)
                .Listen(options.SpotRouterEndpoint)
                .SetRoutingIdPrefix(options.Rid);
            // This node serves the RouteMesh channel, so it is not outbound-only.
            // Object Server includes the Object Client operations used by the session.
            mesh23.Objects().Server();
            mesh23.Channel(AutomaticTurnDispatchNames.SpotChannel).Server();
            framework.AddStreamNode(AutomaticTurnDispatchNames.StreamNode)
                .Bind(options.StreamEndpoint)
                .EnableActorDispatch()
                .AddSession<AwaitStreamSession>();
        });

        var app = builder.Build();
        app.MapGet("/health", () => Results.Ok(new { status = "ready", role = "session", options.Rid }));
        app.MapGet("/topology/ready", async (
            string meshName,
            string rid,
            IZLinkRouteMeshRuntime runtime,
            IZLinkRouteClient routes,
            CancellationToken cancellationToken) =>
        {
            var snapshot = runtime.GetStatus(meshName);
            var snapshotReady = snapshot.Peers.Any(peer =>
                peer.State == ZLinkPeerState.Ready
                && (string.Equals(
                        peer.NodeRid.ToString(),
                        rid,
                        StringComparison.Ordinal)
                    || peer.NodeRid.ToString().StartsWith(
                        $"{rid}-",
                        StringComparison.Ordinal)));
            if (!snapshotReady
                || !string.Equals(
                    meshName,
                    AutomaticTurnDispatchNames.ControlChannel,
                    StringComparison.Ordinal))
                return Results.Ok(new { ready = snapshotReady });

            try
            {
                var target = snapshot.Peers
                    .Where(peer => peer.State == ZLinkPeerState.Ready)
                    .Select(peer => peer.NodeRid)
                    .First(peer => peer.ToString().StartsWith(
                        $"{rid}-",
                        StringComparison.Ordinal));
                _ = await routes.RequestToNode(
                        meshName,
                        target,
                        new AwaitEvidenceReq($"readiness-{rid}"))
                    .Timeout(TimeSpan.FromMilliseconds(500))
                    .Async<AwaitEvidenceRes>(cancellationToken);
                return Results.Ok(new { ready = true });
            }
            catch (Exception error) when (
                error is ZLinkFrameworkException
                    or TimeoutException
                    or OperationCanceledException)
            {
                return Results.Ok(new { ready = false });
            }
        });
        app.MapGet("/evidence", (EvidenceStore evidence) => Results.Ok(evidence.Snapshot()));
        return app;
    }
}
