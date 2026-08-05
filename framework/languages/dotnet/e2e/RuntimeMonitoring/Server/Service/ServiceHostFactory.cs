using Microsoft.Extensions.Configuration;
using StackExchange.Redis;

using Microsoft.AspNetCore.Mvc;
using RuntimeMonitoring.Server.Service.Handlers;
using RuntimeMonitoring.Server.Service.Support;
using RuntimeMonitoring.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Spots;

using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Diagnostics;

namespace RuntimeMonitoring.Server.Service;

internal static class ServiceHostFactory
{
    public static WebApplication CreateAll(string[] args)
    {
        var options = ServerOptions.Parse(args, "service");
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
        builder.Services.AddSingleton(new EvidenceStore(options.EvidenceFile, options.Rid));
        builder.Services.AddSingleton<ApplicationDispatchGate>();
        builder.Services.AddSingleton<ObserverIsolationProbe>();
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
            if (!string.IsNullOrWhiteSpace(options.RedisEndpoint))
            {
                var redisConfiguration = ConfigurationOptions.Parse(options.RedisEndpoint);
                // This local failure-recovery fixture must surface a paused
                // store inside the three-second readiness boundary. The
                // production default command timeout would defer the first
                // failure observation for about five seconds.
                redisConfiguration.AsyncTimeout = 500;
                redisConfiguration.ConnectTimeout = 500;
                framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConfigurationOptions = redisConfiguration; redis.KeyPrefix = options.RedisKeyPrefix
                                  ?? throw new InvalidOperationException("Shared.RedisKeyPrefix is required."); }));
            }
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);

            var channelMesh = framework.AddRouteMesh(RuntimeMonitoringNames.Channel)
                .Listen(Require(options.ChannelEndpoint, "ChannelEndpoint"))
                .SetRoutingIdPrefix(options.Rid);
            channelMesh.Channel(RuntimeMonitoringNames.Channel).Server()
                .AddRequestHandler<ProfileRequestHandler, ProfileReq, ProfileRes>("ProfileReq");

            var spotMesh = framework.AddRouteMesh(RuntimeMonitoringNames.SpotChannel);
            spotMesh.Channel(RuntimeMonitoringNames.SpotChannel).Server()
                .AddRequestHandler<
                    ProfileRequestHandler,
                    ProfileReq,
                    ProfileRes>("ProfileReq");
            spotMesh.Listen(Require(options.SpotRouterEndpoint, "SpotRouterEndpoint"))
                .SetRoutingIdPrefix(options.Rid);
            spotMesh.Objects().Server()
                .AddEntrySpot<MonitoringEntrySpot>()
                .AddSpotFactory<MonitoringSubjectSpot>(
                    RuntimeMonitoringNames.SubjectSpotType, factory => factory.DisableRelocation());
            var spotRouter = spotMesh.ConfigureRouterSocket();
            spotRouter.SendHighWaterMark = 1;
            spotRouter.SendTimeout = TimeSpan.FromMilliseconds(250);
            spotRouter.MailboxMessageBudget = 1;
            spotRouter.MailboxByteBudget = 2 * 1024 * 1024;
        });
        // Observe public runtime status only after the framework hosted service
        // has started the runtime.
        builder.Services.AddHostedService<MeshEventRecorder>();

        var app = builder.Build();
        app.MapGet("/health", () => Results.Ok(new { status = "ready", options.Role, options.Rid }));
        app.MapGet("/evidence", (EvidenceStore evidence) => Results.Ok(evidence.Snapshot()));
        app.MapGet("/runtime/snapshot", (
            [FromServices] IZLinkRouteMeshRuntime runtime) =>
            Results.Ok(Project(runtime.GetStatus(RuntimeMonitoringNames.SpotChannel))));
        app.MapGet("/runtime/snapshot/{meshName}", (
            string meshName,
            [FromServices] IZLinkRouteMeshRuntime runtime) =>
            Results.Ok(Project(runtime.GetStatus(meshName))));
        app.MapPost("/runtime/observer/start/{meshName}", (
            string meshName,
            [FromServices] ObserverIsolationProbe probe) =>
        {
            probe.Start(meshName);
            return Results.Ok(probe.Status());
        });
        app.MapPost("/runtime/observer/release", (
            [FromServices] ObserverIsolationProbe probe) =>
        {
            probe.ReleaseSlowConsumer();
            return Results.Ok(probe.Status());
        });
        app.MapGet("/runtime/observer/status", (
            [FromServices] ObserverIsolationProbe probe) =>
            Results.Ok(probe.Status()));
        app.MapGet("/runtime/validate", async (
            [FromServices] IZLinkRouteMeshRuntime runtime,
            CancellationToken cancellationToken) =>
        {
            var missingSnapshotRejected = Rejects(
                () => runtime.GetStatus("missing.mesh"));
            var missingObserverRejected = await RejectsObserverAsync(
                runtime,
                "missing.mesh",
                cancellationToken);
            var registeredObserverProducedStatus = await ProducesStatusAsync(
                runtime,
                RuntimeMonitoringNames.SpotChannel,
                cancellationToken);
            return Results.Ok(new RuntimeValidationRes(
                missingSnapshotRejected,
                missingObserverRejected,
                registeredObserverProducedStatus));
        });
        app.MapPost("/evidence/wait", async (
            EvidenceWaitReq request,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            var snapshot = await evidence.WaitUntilAsync(
                entries => request.ContainsAll.All(expected =>
                               entries.Skip(request.AfterIndex)
                                   .Any(entry => entry.Contains(expected, StringComparison.Ordinal)))
                           && request.ContainsAnyGroups.All(group =>
                               group.Any(expected =>
                                   entries.Skip(request.AfterIndex)
                                       .Any(entry => entry.Contains(expected, StringComparison.Ordinal)))),
                timeout,
                cancellationToken);
            return Results.Ok(snapshot.Skip(request.AfterIndex).ToArray());
        });
        app.MapPost("/shutdown", (IHostApplicationLifetime lifetime) =>
        {
            lifetime.StopApplication();
            return Results.Ok(new { status = "stopping" });
        });
        app.MapPost("/profile/request", async (
            ProfileReq request,
            [FromServices] IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            var response = await channel.RequestToChannel(RuntimeMonitoringNames.Channel, request)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async<ProfileRes>(cancellationToken);
            return Results.Ok(response);
        });
        app.MapPost("/spot/profile/request", async (
            ProfileReq request,
            [FromServices] IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            var response = await channel.RequestToChannel(
                    RuntimeMonitoringNames.SpotChannel,
                    request)
                .Timeout(TimeSpan.FromSeconds(30))
                .Async<ProfileRes>(cancellationToken);
            return Results.Ok(response);
        });
        app.MapPost("/spot/publish/{topic}", async (
            string topic,
            ProfileReq request,
            [FromServices] IZLinkSpotPublisherClient publisher,
            CancellationToken cancellationToken) =>
        {
            await publisher.Publish(
                    RuntimeMonitoringNames.SpotChannel,
                    topic,
                    request)
                .Async(cancellationToken);
            return Results.Ok();
        });
        app.MapPost("/admin/application-gate/reset", (
            [FromServices] ApplicationDispatchGate gate) =>
        {
            gate.Reset();
            return Results.Ok(new { status = "reset" });
        });
        app.MapPost("/admin/application-gate/release", (
            [FromServices] ApplicationDispatchGate gate) =>
        {
            gate.Release();
            return Results.Ok(new { status = "released" });
        });
        app.MapPost("/admin/subject/create", async (
            [FromServices] IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            await spots.GetOrCreate(
                    "monitor-subject",
                    RuntimeMonitoringNames.SubjectSpotType)
                .InMesh(RuntimeMonitoringNames.SpotChannel)
                .Async(cancellationToken);
            return Results.Ok(new { status = "created" });
        });
        app.MapPost("/admin/subject/create/{spotRid}", async (
            string spotRid,
            [FromServices] IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            await spots.GetOrCreate(
                    spotRid,
                    RuntimeMonitoringNames.SubjectSpotType)
                .InMesh(RuntimeMonitoringNames.SpotChannel)
                .Async(cancellationToken);
            return Results.Ok(new { status = "created", spotRid });
        });
        app.MapPost("/admin/subject/close", async (
            [FromServices] IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            var spot = await spots.FindAsync("monitor-subject", cancellationToken);
            var closed = spot is not null
                         && await spots.CloseAsync(spot.Value, cancellationToken);
            return Results.Ok(new { status = closed ? "closed" : "not-found" });
        });
        app.MapPost("/admin/subject/close/{spotRid}", async (
            string spotRid,
            [FromServices] IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            var spot = await spots.FindAsync(spotRid, cancellationToken);
            var closed = spot is not null
                         && await spots.CloseAsync(spot.Value, cancellationToken);
            return Results.Ok(new
            {
                status = closed ? "closed" : "not-found",
                spotRid
            });
        });
        app.MapPost("/admin/weight/exclude", (
            [FromServices] IZLinkRouteMeshRuntimeOptions runtimeOptions,
            [FromServices] EvidenceStore evidence) =>
        {
            runtimeOptions.Channel(RuntimeMonitoringNames.Channel).Weight = 0;
            evidence.Add($"admin|rid={evidence.Rid}|action=drain|weight=0");
            return Results.Ok(new { status = "drained", weight = 0 });
        });
        app.MapPost("/admin/weight/include", (
            [FromServices] IZLinkRouteMeshRuntimeOptions runtimeOptions,
            [FromServices] EvidenceStore evidence) =>
        {
            runtimeOptions.Channel(RuntimeMonitoringNames.Channel).Weight = 100;
            evidence.Add($"admin|rid={evidence.Rid}|action=restore|weight=100");
            return Results.Ok(new { status = "restored", weight = 100 });
        });
        app.MapPost("/admin/spot-weight/exclude", (
            [FromServices] IZLinkRouteMeshRuntimeOptions runtimeOptions) =>
        {
            runtimeOptions.Channel(RuntimeMonitoringNames.SpotChannel).Weight = 0;
            return Results.Ok(new { status = "drained", weight = 0 });
        });
        app.MapPost("/admin/spot-weight/include", (
            [FromServices] IZLinkRouteMeshRuntimeOptions runtimeOptions) =>
        {
            runtimeOptions.Channel(RuntimeMonitoringNames.SpotChannel).Weight = 100;
            return Results.Ok(new { status = "restored", weight = 100 });
        });
        app.MapPost("/admin/graceful-drain", async (
            [FromServices] IZLinkFrameworkRuntime runtime,
            CancellationToken cancellationToken) =>
        {
            var relocation = await runtime.RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = TimeSpan.FromSeconds(30)
                },
                cancellationToken);
            if (relocation.Outcome != ZLinkFrameworkRelocationOutcome.Relocated)
                return Results.Ok(new DrainResultRes("Blocked", relocation.Reason.ToString()));

            var result = await runtime.ShutdownAsync(
                TimeSpan.FromSeconds(30),
                cancellationToken);
            return Results.Ok(result switch
            {
                { Outcome: ZLinkFrameworkTerminationOutcome.Stopped } =>
                    new DrainResultRes("Drained"),
                _ => new DrainResultRes("ForceStopped", result.Reason.ToString())
            });
        });
        return app;
    }

    private static string Require(string? value, string name)
    {
        return string.IsNullOrWhiteSpace(value)
            ? throw new InvalidOperationException($"{name} is required.")
            : value;
    }

    private static bool Rejects(Action action)
    {
        try
        {
            action();
            return false;
        }
        catch (Exception exception) when (
            exception is ArgumentException
                or InvalidOperationException
                or KeyNotFoundException)
        {
            return true;
        }
    }

    private static async Task<bool> RejectsObserverAsync(
        IZLinkRouteMeshRuntime runtime,
        string meshName,
        CancellationToken cancellationToken)
    {
        try
        {
            await using var observer = runtime.ObserveAsync(
                    meshName,
                    cancellationToken)
                .GetAsyncEnumerator(cancellationToken);
            _ = await observer.MoveNextAsync();
            return false;
        }
        catch (Exception exception) when (
            exception is ArgumentException
                or InvalidOperationException
                or KeyNotFoundException)
        {
            return true;
        }
    }

    private static async Task<bool> ProducesStatusAsync(
        IZLinkRouteMeshRuntime runtime,
        string meshName,
        CancellationToken cancellationToken)
    {
        await using var observer = runtime.ObserveAsync(
                meshName,
                cancellationToken)
            .GetAsyncEnumerator(cancellationToken);
        return await observer.MoveNextAsync();
    }

    private static MeshRuntimeSnapshotRes Project(ZLinkRouteMeshStatus snapshot)
    {
        return new MeshRuntimeSnapshotRes(
            snapshot.MeshName,
            snapshot.State.ToString(),
            snapshot.IsReady,
            snapshot.ReadyPeerCount,
            snapshot.Sequence,
            snapshot.ObservedAt,
            snapshot.Peers.Select(static peer => new MeshRuntimePeerRes(
                peer.NodeRid.ToString(),
                peer.State.ToString(),
                peer.UnavailableReason?.ToString())).ToArray(),
            snapshot.Channels.Select(static channel => new MeshRuntimeChannelRes(
                channel.ChannelName,
                channel.IsReady,
                channel.ReadyTargetCount)).ToArray(),
            new MeshRuntimePlacementRes(
                snapshot.Placement.IsAvailable,
                snapshot.Placement.ActiveActorCount,
                snapshot.Placement.ActiveSpotCount,
                snapshot.Placement.UnavailableReason?.ToString()));
    }
}
