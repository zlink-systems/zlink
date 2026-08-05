using Microsoft.Extensions.Configuration;

using System.Collections.Concurrent;
using System.Diagnostics;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework;
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

namespace SpotService.Server.Gateway;

using Zlink.Framework.E2E.Configuration;

internal static class GatewayHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = GatewayOptions.Parse(args);
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
                framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = options.RedisKeyPrefix
                                  ?? throw new InvalidOperationException("Shared.RedisKeyPrefix is required."); }));
                // Crash-recovery scenarios re-claim actors from a killed
                // node; a short owner lease keeps that takeover window
                // within the scenario's patience.
                var locations = framework.ConfigureLocations();
                locations.OwnerLeaseRenewInterval = TimeSpan.FromSeconds(1);
                locations.OwnerLeaseTtl = TimeSpan.FromSeconds(10);
                locations.PollingInterval = TimeSpan.FromMilliseconds(500);
            }
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            var externalMesh = framework.AddRouteMesh(SpotServiceNames.ExternalSpotChannel)
                .Listen("tcp://127.0.0.1:0")
                .SetRoutingIdPrefix(options.Rid);
            externalMesh.Channel(SpotServiceNames.ExternalSpotChannel).Client();
            var mesh19 = framework.AddRouteMesh(SpotServiceNames.SpotChannel)
                .Listen(Require(options.SpotRouterEndpoint, "SpotRouterEndpoint"))
                .SetRoutingIdPrefix(options.Rid);
            mesh19.Objects().Client();
            mesh19.Channel(SpotServiceNames.SpotChannel).Client();
            var publisherConfig = mesh19.ConfigureSpotPublisher();
            publisherConfig.SendHighWaterMark = options.SpotPublisherSendHighWaterMark;
            publisherConfig.SendTimeout = TimeSpan.FromMilliseconds(
                options.SpotPublisherSendTimeoutMilliseconds);
            mesh19.ConfigureRouterSocket().SendTimeout = TimeSpan.FromMilliseconds(
                options.SpotPublisherSendTimeoutMilliseconds);
        });

        var app = builder.Build();
        app.MapGet("/health", () => Results.Ok(new { status = "ready", options.Rid }));
        app.MapGet("/evidence", (EvidenceStore evidence) => Results.Ok(evidence.Snapshot()));
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
        app.MapPost("/spot/publish", async (
            SpotPublishReq request,
            IZLinkSpotPublisherClient publisher,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            await publisher.Publish(
                    SpotServiceNames.SpotChannel,
                    SpotServiceNames.SpotMsgTopic,
                    new SpotMsg(request.Marker))
                .Async(cancellationToken);
            evidence.Add($"spot-publish|rid={options.Rid}|spot={request.SpotRid}|marker={request.Marker}");
            return Results.Ok(new SpotPublishRes(
                "spot.sm-c4-publish",
                options.Rid,
                request.SpotRid,
                request.Marker,
                evidence.Snapshot()));
        });
        app.MapPost("/spot/backpressure-publish", async (
            SpotBackpressurePublishReq request,
            IZLinkSpotPublisherClient publisher,
            CancellationToken cancellationToken) =>
        {
            var payload = new string('x', Math.Clamp(request.PayloadBytes, 1024, 1024 * 1024));
            var sequence = Math.Max(1, request.StartSequence);
            var started = Stopwatch.GetTimestamp();
            await publisher.Publish(
                    SpotServiceNames.SpotChannel,
                    SpotServiceNames.SpotMsgTopic,
                    new SpotBackpressureMsg(request.Marker, sequence, payload))
                .Async(cancellationToken);
            var elapsed = Stopwatch.GetElapsedTime(started);
            return Results.Ok(new SpotBackpressureSubmitRes(
                sequence,
                (long)elapsed.TotalMilliseconds));
        });
        app.MapPost("/channel/route-ping", async (
            IZLinkRouteClient routes,
            ControlPingReq request,
            CancellationToken cancellationToken) =>
        {
            var reply = await routes.RequestToNode(
                    SpotServiceNames.ExternalSpotChannel,
                    RoutingId.From("play-a"),
                    request)
                .Async<ControlPingRes>(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/spot/route-state", async (
            IZLinkSpotClient spotsClient,
            SpotStateRouteReq request,
            CancellationToken cancellationToken) =>
        {
            var reply = await spotsClient
                .RequestToSpot(request.SpotRid, new StateReq(request.Operation, request.Delta))
                .Async<StateRes>(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/actor/push", async (
            ActorPushByActorReq request,
            IZLinkActorClient actors,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            evidence.Add($"actor-push-request|rid={options.Rid}|actor={request.ActorId}|value={request.Value}");
            try
            {
                var reply = await actors.RequestToActor(
                        request.ActorId,
                        new ActorPushReq(request.Value))
                    .Timeout(TimeSpan.FromSeconds(10))
                    .Async<ActorPingRes>(cancellationToken);
                evidence.Add(
                    $"actor-push-delivered|rid={options.Rid}|actor={reply.ActorId}"
                    + $"|value={reply.Value}|node={reply.NodeRid}");
                return Results.Ok(new ActorPushByActorRes(reply.ActorId, reply.Value, true, string.Empty));
            }
            catch (ZLinkFrameworkException ex)
            {
                evidence.Add(
                    $"actor-push-failed|rid={options.Rid}|actor={request.ActorId}"
                    + $"|error={ex.GetType().Name}|kind={ex.Kind}");
                return Results.Ok(new ActorPushByActorRes(
                    request.ActorId,
                    request.Value,
                    false,
                    ex.Kind.ToString()));
            }
            catch (Exception ex)
            {
                evidence.Add(
                    $"actor-push-failed|rid={options.Rid}|actor={request.ActorId}"
                    + $"|error={ex.GetType().Name}");
                return Results.Ok(new ActorPushByActorRes(
                    request.ActorId,
                    request.Value,
                    false,
                    ex.GetType().Name));
            }
        });
        app.MapPost("/actor/wait-missing", async (
            ActorMissingWaitReq request,
            IZLinkActorManager actorDirectory,
            CancellationToken cancellationToken) =>
        {
            var deadline = DateTimeOffset.UtcNow
                           + TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            while (DateTimeOffset.UtcNow < deadline)
            {
                if (await actorDirectory.FindAsync(request.ActorId, cancellationToken) is null)
                    return Results.Ok();
                await Task.Delay(TimeSpan.FromMilliseconds(100), cancellationToken);
            }

            return Results.Problem(
                $"Actor route '{request.ActorId}' remained visible.",
                statusCode: StatusCodes.Status504GatewayTimeout);
        });
        app.MapPost("/actor/capture-ref", async (
            ActorRefReq request,
            IZLinkActorManager actors,
            CancellationToken cancellationToken) =>
        {
            var actor = await actors.FindAsync(request.ActorId, cancellationToken)
                        ?? throw new InvalidOperationException(
                            $"Actor route '{request.ActorId}' was not found.");
            return Results.Ok(new ActorRefRes(
                actor.ActorId, actor.NodeRid.ToString(), actor.ObjectGeneration));
        });
        app.MapPost("/actor/get-or-create", async (
            EnsureActorReq request,
            IZLinkActorManager actors,
            CancellationToken cancellationToken) =>
        {
            var result = await actors
                .GetOrCreate(request.ActorId, SpotServiceNames.ActorType)
                .InMesh(SpotServiceNames.SpotChannel)
                .Request(new ScenarioActorCreateReq(request.DisplayName))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(cancellationToken);
            var actor = result switch
            {
                ZLinkActorCreateResult.Existing value => value.Actor,
                ZLinkActorCreateResult.Created value => value.Actor,
                _ => throw new InvalidOperationException("Actor creation was rejected.")
            };
            return Results.Ok(new EnsureActorRes(
                actor.ActorId,
                actor.NodeRid.ToString(),
                actor.ObjectGeneration));
        });
        app.MapPost("/actor/manager-probe", async (
            ActorManagerProbeReq request,
            IZLinkActorManager actors,
            CancellationToken cancellationToken) =>
        {
            if (string.Equals(request.Operation, "find", StringComparison.Ordinal))
            {
                var found = await actors.FindAsync(request.ActorId, cancellationToken);
                return Results.Ok(new ActorManagerProbeRes(
                    request.Operation,
                    found is null ? "Missing" : "Found",
                    found is null
                        ? null
                        : new ActorRefRes(
                            found.Value.ActorId,
                            found.Value.NodeRid.ToString(),
                            found.Value.ObjectGeneration)));
            }

            ZLinkActorCreateResult result;
            if (string.Equals(request.Operation, "create", StringComparison.Ordinal))
            {
                result = await actors
                    .Create(request.ActorId, SpotServiceNames.ActorType)
                    .Request(new ScenarioActorCreateReq(request.Operation))
                    .Timeout(TimeSpan.FromSeconds(15))
                    .Async(cancellationToken);
            }
            else if (string.Equals(
                         request.Operation,
                         "get-or-create",
                         StringComparison.Ordinal))
            {
                result = await actors
                    .GetOrCreate(request.ActorId, SpotServiceNames.ActorType)
                    .InMesh(SpotServiceNames.SpotChannel)
                    .Request(new ScenarioActorCreateReq(request.Operation))
                    .Timeout(TimeSpan.FromSeconds(15))
                    .Async(cancellationToken);
            }
            else
            {
                throw new ArgumentException(
                    $"Unknown Actor manager probe operation '{request.Operation}'.",
                    nameof(request));
            }

            return result switch
            {
                ZLinkActorCreateResult.Created created =>
                    Results.Ok(new ActorManagerProbeRes(
                        request.Operation,
                        "Created",
                        new ActorRefRes(
                            created.Actor.ActorId,
                            created.Actor.NodeRid.ToString(),
                            created.Actor.ObjectGeneration))),
                ZLinkActorCreateResult.Existing existing =>
                    Results.Ok(new ActorManagerProbeRes(
                        request.Operation,
                        "Existing",
                        new ActorRefRes(
                            existing.Actor.ActorId,
                            existing.Actor.NodeRid.ToString(),
                            existing.Actor.ObjectGeneration))),
                ZLinkActorCreateResult.Rejected =>
                    Results.Ok(new ActorManagerProbeRes(
                        request.Operation,
                        "Rejected",
                        null)),
                _ => throw new InvalidOperationException(
                    "Unknown Actor create terminal result.")
            };
        });
        app.MapPost("/spot/get-or-create", async (
            CreateSpotReq request,
            IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            var result = await spots
                .GetOrCreate(request.SpotRid, SpotServiceNames.UserSpotType)
                .InMesh(SpotServiceNames.SpotChannel)
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(cancellationToken);
            return Results.Ok(new CreateSpotRes(
                result.Spot.SpotId,
                result.Spot.NodeRid.ToString(),
                result.State.ToString()));
        });
        app.MapPost("/spot/get-or-create-typed", async (
            TypedSpotCreateReq request,
            IZLinkSpotManager spots,
            CancellationToken cancellationToken) =>
        {
            var result = await spots
                .GetOrCreate(request.SpotId, request.SpotType)
                .InMesh(SpotServiceNames.SpotChannel)
                .Timeout(TimeSpan.FromSeconds(15))
                .Async(cancellationToken);
            return Results.Ok(new CreateSpotRes(
                result.Spot.SpotId,
                result.Spot.NodeRid.ToString(),
                result.State.ToString()));
        });
        app.MapPost("/actor/request", async (
            ActorRequestReq request,
            IZLinkActorClient actors,
            CancellationToken cancellationToken) =>
        {
            try
            {
                var call = request.DelayMilliseconds > 0
                    ? actors.RequestToActor(
                        request.ActorId,
                        new SlowActorPingReq(request.Value, request.DelayMilliseconds))
                    : actors.RequestToActor(
                        request.ActorId, new ActorPingReq(request.Value));
                var reply = await call
                    .Timeout(TimeSpan.FromMilliseconds(
                        Math.Clamp(request.TimeoutMilliseconds, 1, 30000)))
                    .Async<ActorPingRes>(cancellationToken);
                return Results.Ok(new ActorRequestRes(true, string.Empty, reply));
            }
            catch (ZLinkFrameworkException error)
            {
                return Results.Ok(new ActorRequestRes(false, error.Kind.ToString(), null));
            }
            catch (TimeoutException)
            {
                return Results.Ok(new ActorRequestRes(false, "Timeout", null));
            }
        });
        app.MapPost("/actor/join-spot", async (
            JoinUserSpotActorReq request,
            IZLinkActorClient actors,
            CancellationToken cancellationToken) =>
        {
            var reply = await actors
                .RequestToActor(request.ActorId, request)
                .Timeout(TimeSpan.FromSeconds(15))
                .Async<JoinUserSpotActorRes>(cancellationToken);
            return Results.Ok(reply);
        });
        app.MapPost("/actor/destroy-ref", async (
            ActorRefDestroyReq request,
            IZLinkActorManager actors,
            CancellationToken cancellationToken) =>
        {
            try
            {
                var destroyed = await actors.DestroyAsync(
                    new ActorRef(
                        request.Actor.ActorId,
                        request.Actor.Generation,
                        SpotServiceNames.SpotChannel,
                        RoutingId.From(request.Actor.NodeRid)),
                    cancellationToken);
                return Results.Ok(new ActorRefDestroyRes(
                    true,
                    destroyed,
                    string.Empty));
            }
            catch (ZLinkFrameworkException error)
            {
                return Results.Ok(new ActorRefDestroyRes(
                    false,
                    false,
                    error.Kind.ToString()));
            }
        });
        app.MapPost("/node/wait-ready", async (
            NodeReadinessWaitReq request,
            IZLinkRouteMeshRuntime meshRuntime,
            CancellationToken cancellationToken) =>
        {
            var deadline = DateTimeOffset.UtcNow
                           + TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            var lastPeerReady = false;
            while (DateTimeOffset.UtcNow < deadline)
            {
                var peer = meshRuntime.GetStatus(SpotServiceNames.SpotChannel)
                    .Peers
                    .SingleOrDefault(row =>
                {
                    var rowRid = row.NodeRid.ToString();
                    return row.State == ZLinkPeerState.Ready
                           && (string.Equals(rowRid, request.NodeRid, StringComparison.Ordinal)
                               || rowRid.StartsWith(
                                   $"{request.NodeRid}-",
                                   StringComparison.Ordinal));
                });
                var peerReady = peer is not null;
                lastPeerReady = peerReady;
                if (peerReady)
                    return Results.Ok(new NodeReadinessWaitRes(request.NodeRid, true));
                await Task.Delay(100, cancellationToken);
            }
            Console.WriteLine(
                $"spot-service node-readiness-timeout node={request.NodeRid}"
                + $" peer={lastPeerReady}"
                + $" mesh-peers={string.Join(',', meshRuntime.GetStatus(SpotServiceNames.SpotChannel).Peers
                    .Select(peer => $"{peer.NodeRid}:{peer.State}:{peer.UnavailableReason?.ToString() ?? "none"}"))}");
            return Results.Problem(
                $"Node '{request.NodeRid}' peer readiness did not converge.",
                statusCode: StatusCodes.Status504GatewayTimeout);
        });
        app.MapPost("/shutdown", (IHostApplicationLifetime lifetime) =>
        {
            lifetime.StopApplication();
            return Results.Ok(new { status = "stopping" });
        });
        return app;
    }

    static string Require(string? value, string optionName)
        => string.IsNullOrWhiteSpace(value)
            ? throw new InvalidOperationException($"{optionName} is required.")
            : value;
}

internal sealed class EvidenceStore
{
    readonly ConcurrentQueue<string> _entries = new();
    readonly string? _file;
    // A pulse completed on every Add and swapped for a fresh one, so EVERY
    // concurrent waiter wakes — a counted semaphore hands one release to one
    // waiter and silently starves the rest.
    readonly object _pulseGate = new();
    TaskCompletionSource<bool> _pulse =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public EvidenceStore(string rid, string? file)
    {
        _file = file;
        Add($"start|rid={rid}");
    }

    public void Add(string value)
    {
        _entries.Enqueue(value);
        TaskCompletionSource<bool> pulse;
        lock (_pulseGate)
        {
            pulse = _pulse;
            _pulse = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
        }

        pulse.TrySetResult(true);
        if (!string.IsNullOrWhiteSpace(_file))
        {
            File.AppendAllLines(_file, new[] { value });
        }
    }

    public string[] Snapshot() => _entries.ToArray();

    public async Task<string[]> WaitUntilAsync(
        Func<string[], bool> condition,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (true)
        {
            Task pulseTask;
            lock (_pulseGate)
            {
                pulseTask = _pulse.Task;
            }

            var snapshot = Snapshot();
            if (condition(snapshot)) return snapshot;

            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining <= TimeSpan.Zero) throw new TimeoutException("Timed out waiting for spot service evidence.");

            await Task.WhenAny(pulseTask, Task.Delay(remaining, cancellationToken));
        }
    }
}

internal sealed record GatewayOptions(
    string Rid,
    string HttpUrl,
    string LogDir,
    string? EvidenceFile = null,
    string? RedisEndpoint = null,
    string? RedisKeyPrefix = null,
    string? SpotRouterEndpoint = null,
    string? SpotPubEndpoint = null,
    string? ExternalSpotEndpoint = null,
    string? SpotPeerAEndpoint = null,
    string? SpotPeerBEndpoint = null,
    ulong SpotPublisherSendHighWaterMark = 1,
    int SpotPublisherSendTimeoutMilliseconds = 250)
{
    public static GatewayOptions Parse(string[] args)
        => E2eConfiguration.Load<GatewayOptions>(args);
}
