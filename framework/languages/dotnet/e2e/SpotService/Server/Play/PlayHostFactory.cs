using Microsoft.Extensions.Configuration;

using SpotService.Server.Play.Endpoints;
using SpotService.Server.Play.Handlers;
using SpotService.Server.Play.Spots;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Spots;

using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Diagnostics;

namespace SpotService.Server.Play;

internal static class PlayHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = ServerOptions.Parse(args, "play");
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
        builder.Services.AddSingleton(new NodeOptions(options.Rid));
        builder.Services.AddSingleton<ApplicationJoinCoordinator>();
        LocationStoreOperationProbe? locationStoreOperationProbe = null;
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

        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            if (!string.IsNullOrWhiteSpace(options.RedisEndpoint))
            {
                locationStoreOperationProbe = new LocationStoreOperationProbe(
                    new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = options.RedisKeyPrefix
                                      ?? throw new InvalidOperationException("Shared.RedisKeyPrefix is required."); }));
                framework.AddLocationStore(locationStoreOperationProbe);
                framework.AddRelocationStore(new ZLinkRedisRelocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = $"{options.RedisKeyPrefix}:relocation"; }));
                // Crash-recovery scenarios re-claim actors from a killed
                // node; a short owner lease keeps that takeover window
                // within the scenario's patience.
                var locations = framework.ConfigureLocations();
                locations.OwnerLeaseRenewInterval = TimeSpan.FromSeconds(1);
                locations.OwnerLeaseTtl =
                    options.OwnerLeaseTtlMilliseconds is > 0
                        ? TimeSpan.FromMilliseconds(
                            options.OwnerLeaseTtlMilliseconds.Value)
                        : TimeSpan.FromSeconds(10);
                locations.PollingInterval = TimeSpan.FromMilliseconds(500);
                if (options.MessageFollowDurationMilliseconds is > 0)
                {
                    locations.MessageFollowDuration = TimeSpan.FromMilliseconds(
                        options.MessageFollowDurationMilliseconds.Value);
                    locations.RouteCacheMaxAge = TimeSpan.FromSeconds(1);
                }
            }
            framework.AddHandlersFromAssemblyOf(typeof(Program));
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            var controlMesh = framework.AddRouteMesh(SpotServiceNames.ControlChannel)
                .Listen(Require(options.ControlEndpoint, "ControlEndpoint"))
                .SetRoutingIdPrefix(options.Rid);
            controlMesh.Channel(SpotServiceNames.ControlChannel).Server();
            AddPlayRouteHandlers(controlMesh);
            var externalSpotChannel = string.Equals(options.Rid, "play-b", StringComparison.Ordinal)
                ? SpotServiceNames.ExternalSpotChannelB
                : SpotServiceNames.ExternalSpotChannel;
            if (!string.IsNullOrWhiteSpace(options.ExternalSpotEndpoint))
            {
                var externalMesh = framework.AddRouteMesh(externalSpotChannel)
                    .Listen(options.ExternalSpotEndpoint)
                    .SetRoutingIdPrefix(options.Rid);
                externalMesh.Channel(externalSpotChannel).Server();
                AddPlayRouteHandlers(externalMesh);
            }
            var spot = framework.AddRouteMesh(SpotServiceNames.SpotChannel)
                .Listen(Require(options.SpotRouterEndpoint, "SpotRouterEndpoint"))
                .SetRoutingIdPrefix(options.Rid);
            if (options.InstanceSpotIdleTimeoutMilliseconds is > 0)
            {
                spot.SetInstanceSpotIdleTimeout(
                    TimeSpan.FromMilliseconds(
                        options.InstanceSpotIdleTimeoutMilliseconds.Value));
                evidence.Add(
                    $"instance-idle-config|rid={options.Rid}"
                    + $"|milliseconds={options.InstanceSpotIdleTimeoutMilliseconds.Value}");
            }
            if (!string.IsNullOrWhiteSpace(options.SpotRouterAdvertiseHost))
            {
                spot.SetAdvertiseHost(options.SpotRouterAdvertiseHost);
            }
            if (options.PopulationLimit is { } populationLimit)
            {
                spot.SetActorLimit(populationLimit);
                spot.SetSpotLimit(populationLimit);
            }
            spot.Objects().Server()
                .AddEntrySpot<ScenarioEntrySpot>()
                .AddActorFactory<ScenarioActor, ScenarioActorFactory>(
                    SpotServiceNames.ActorType, factory => factory.RecreateOnRelocation())
                .AddSpotFactory<ScenarioUserSpot>(
                    SpotServiceNames.UserSpotType, factory =>
                    {
                        if (options.PopulationLimit is { } stableTypeLimit)
                            factory.StableTypeLimit(stableTypeLimit);
                        factory.DisableRelocation();
                    })
                .AddInstanceSpotFactory<ScenarioInstanceSpot>(
                    SpotServiceNames.InstanceSpotType,
                    factory => factory.DisableRelocation())
                .AddSpotFactory<ScenarioAlternateSpot>(
                    SpotServiceNames.AlternateSpotType, factory => factory.DisableRelocation())
                .AddSpotFactory<ScenarioWeightCapacitySpot>(
                    SpotServiceNames.WeightCapacitySpotType,
                    factory => factory
                        .StableTypeLimit(1)
                        .DisableRelocation())
                .AddSpotFactory<MultiNodeSpotA>(
                    SpotServiceNames.MultiSpotTypeA, factory => factory.DisableRelocation())
                .AddSpotFactory<MultiNodeSpotB>(
                    SpotServiceNames.MultiSpotTypeB, factory => factory.DisableRelocation());
            spot.Channel(SpotServiceNames.SpotChannel).Server();
            if (string.Equals(options.Rid, "play-a", StringComparison.Ordinal))
            {
                spot.Channel(SpotServiceNames.ExternalClientChannel).Server()
                    .AddRequestHandler<ChannelEchoHandler>()
                    .AddSendHandler<ChannelNotifyHandler>();
            }
        });
        if (locationStoreOperationProbe is not null)
            builder.Services.AddSingleton(locationStoreOperationProbe);

        var app = builder.Build();
        OperationalEndpoints.MapOperationalEndpoints(app, options);
        SpotLifecycleEndpoints.MapSpotLifecycleEndpoints(app);
        SpotFailureEndpoints.MapSpotFailureEndpoints(app);
        SpotInteractionEndpoints.MapSpotInteractionEndpoints(app);
        InstanceSpotEndpoints.MapInstanceSpotEndpoints(app);
        app.MapGet("/mesh-snapshot", (IZLinkRouteMeshRuntime meshRuntime) =>
            Results.Ok(meshRuntime.GetStatus(SpotServiceNames.SpotChannel)));
        return app;
    }

    private static void AddPlayRouteHandlers(IZLinkMeshNodeBuilder mesh)
    {
        mesh.AddRouteRequestHandler<EnsureActorHandler>()
            .AddRouteRequestHandler<ControlPingHandler>()
            .AddRouteRequestHandler<CreateSpotHandler>()
            .AddRouteRequestHandler<CloseSpotHandler>()
            .AddRouteRequestHandler<SpotTypeMismatchHandler>();
    }

    internal static string Require(string? value, string optionName)
    {
        return string.IsNullOrWhiteSpace(value)
            ? throw new InvalidOperationException($"{optionName} is required.")
            : value;
    }

    internal static async Task<bool> FailsAsync(Task task)
    {
        try
        {
            await task;
            return false;
        }
        catch
        {
            return true;
        }
    }

    internal static async Task<StateRes> RequestSpotStateAsync(
        IZLinkSpotClient routes,
        string targetSpotRid,
        StateReq request)
        => await routes.RequestToSpot(targetSpotRid, request)
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<StateRes>();

    internal static async Task SendSpotCommandAsync(
        IZLinkSpotClient routes,
        string targetSpotRid,
        object command,
        CancellationToken cancellationToken = default)
        => await routes.SendToSpot(targetSpotRid, command)
            .Async(cancellationToken);

    internal static async Task<SpotToSpotRes> RequestSpotToSpotAsync(
        IZLinkSpotClient routes,
        string sourceSpotRid,
        SpotToSpotReq request)
        => await routes.RequestToSpot(sourceSpotRid, request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async<SpotToSpotRes>();

    internal static async Task WaitUntilAsync(Func<bool> condition, string failureMessage)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
        while (DateTimeOffset.UtcNow < deadline)
        {
            if (condition()) return;
            await Task.Delay(10);
        }

        throw new InvalidOperationException(failureMessage);
    }

    internal static async Task WaitUntilAsync(Func<Task<bool>> condition, string failureMessage)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
        Exception? last = null;
        while (DateTimeOffset.UtcNow < deadline)
        {
            try
            {
                if (await condition()) return;
            }
            catch (Exception ex)
            {
                last = ex;
            }

            await Task.Delay(10);
        }

        throw new InvalidOperationException(failureMessage, last);
    }

    private static int FindIndex(string[] lines, string pattern)
    {
        return Array.FindIndex(lines, line => line.Contains(pattern, StringComparison.Ordinal));
    }

    private static ulong ExtractUInt64(string line, string key)
    {
        var prefix = key + "=";
        var start = line.IndexOf(prefix, StringComparison.Ordinal);
        if (start < 0) throw new InvalidOperationException($"Missing field '{key}' in evidence line: {line}");

        start += prefix.Length;
        var end = line.IndexOf('|', start);
        var value = end < 0 ? line[start..] : line[start..end];
        return ulong.Parse(value);
    }

    internal static int CountNew(string[] after, string[] before, string pattern)
    {
        var beforeCount = before.Count(line => line.Contains(pattern, StringComparison.Ordinal));
        var afterCount = after.Count(line => line.Contains(pattern, StringComparison.Ordinal));
        return afterCount - beforeCount;
    }

}
