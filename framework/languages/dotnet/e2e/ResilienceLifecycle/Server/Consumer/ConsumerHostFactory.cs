using System.Diagnostics;
using Microsoft.Extensions.Configuration;

using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using ResilienceLifecycle.Shared;
using Zlink.Framework;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Errors;

using Systems.Zlink;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Diagnostics;

namespace ResilienceLifecycle.Server.Consumer;

using Zlink.Framework.E2E.Configuration;

internal static class ConsumerHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var options = ConsumerOptions.Parse(args);
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
        builder.Services.AddSingleton<ConnectionEvidence>();
        builder.Services.AddSingleton(new E2eMessageFlowListener(
            Path.Combine(options.LogDir, "consumer-flow.log"),
            "consumer"));
        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = options.RedisKeyPrefix; }));
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            JoinConsumerMesh(framework, "consumer");
        });
        // Mesh peers replace the 9.x socket monitor as the connection-evidence
        // source: a peer reaching ready is the wire-level ConnectionReady and a
        // ready peer dropping out is its Disconnected.
        builder.Services.AddHostedService(provider => new MeshConnectionObserverService(
            provider,
            provider.GetRequiredService<ConnectionEvidence>(),
            "monitor-socket|source=resilience.profile.client|"));

        var app = builder.Build();
        app.MapGet("/health", () => Results.Ok(new { status = "ready" }));
        app.MapGet("/connections", (ConnectionEvidence evidence) => Results.Ok(evidence.Snapshot()));
        app.MapPost("/connections/wait", async (
            ConnectionWaitReq request,
            ConnectionEvidence evidence,
            CancellationToken cancellationToken) => Results.Ok(await evidence.WaitAsync(
                request.ContainsAll,
                request.AfterCount,
                TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000)),
                cancellationToken)));
        // Topology waits observe the peer location list — the operational
        // surface the scenarios verify recovery against (config-5 §3).
        app.MapPost("/topology/wait", async (
            IZLinkLocationRuntimeQuery query,
            IZLinkRouteMeshRuntime meshRuntime,
            TopologyWaitReq request) =>
        {
            if (!Enum.TryParse<ZLinkLocationTopologyState>(
                    request.State,
                    ignoreCase: true,
                    out var expectedState))
            {
                return Results.BadRequest(
                    $"Unknown topology state '{request.State}'.");
            }

            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            var elapsed = Stopwatch.StartNew();
            while (true)
            {
                var peers = await query.ListTopologyAsync(
                    new ZLinkLocationTopologyFilter(
                        ResilienceLifecycleNames.Channel,
                        State: expectedState));
                var matches = peers.Items
                    .Where(peer => MatchesRole(peer.NodeRid.ToString(), request.RoutingId)
                                   && (request.ExpectedWeight is null
                                       || request.ExpectedWeight == 0)
                                   && (request.ExpectedDraining is null
                                       || peer.Draining == request.ExpectedDraining))
                    .ToArray();
                var status = meshRuntime.GetStatus(ResilienceLifecycleNames.Channel);
                var readyRids = status.Peers
                    .Where(static peer => peer.State == ZLinkPeerState.Ready)
                    .Select(static peer => peer.NodeRid.ToString())
                    .ToHashSet(StringComparer.Ordinal);
                var activeRids = status.Peers
                    .Where(static peer => peer.State is ZLinkPeerState.Ready or ZLinkPeerState.Draining)
                    .Select(static peer => peer.NodeRid.ToString())
                    .ToHashSet(StringComparer.Ordinal);
                // A gone row is not yet a gone candidate. Spec 08 §3.2 picks
                // among ready Server members, so a scenario that waits for a
                // provider to disappear has to see it leave the ready set too;
                // otherwise the next call still selects it and fails with a
                // transport error instead of the contracted NotFound.
                var satisfied = request.ExpectedCount == 0
                    ? matches.Length == 0
                      && !readyRids.Any(rid => MatchesRole(rid, request.RoutingId))
                    : matches.Length >= request.ExpectedCount
                      && matches.All(peer =>
                          (request.ExpectedDraining == true
                              ? activeRids
                              : readyRids).Contains(peer.NodeRid.ToString()));
                if (satisfied)
                    return Results.Ok(matches
                        .Select(peer => new TopologyEntryRes(
                            peer.NodeRid.ToString(),
                            peer.Endpoint,
                            peer.State.ToString(),
                            0,
                            peer.UpdatedAt.UtcTicks,
                            peer.Draining))
                        .ToArray());

                if (elapsed.Elapsed >= timeout)
                    return Results.Problem(
                        $"Topology wait for '{request.RoutingId}' (expected {request.ExpectedCount}) timed out.");

                await Task.Delay(150);
            }
        });

        app.MapPost("/profile/request", async (
            ProfileReq request,
            IZLinkRouteClient channel) =>
        {
            var reply = await RequestProfileAsync(channel, request);
            return Results.Ok(reply);
        });
        app.MapPost("/profile/request/timeout/{milliseconds:int}", async (
            int milliseconds,
            ProfileReq request,
            IZLinkRouteClient channel) =>
        {
            try
            {
                var reply = await channel.RequestToChannel(
                        ResilienceLifecycleNames.Channel, request)
                    .Timeout(TimeSpan.FromMilliseconds(milliseconds))
                    .Async<ProfileRes>();
                return Results.Ok(reply);
            }
            catch (TimeoutException)
            {
                return Results.StatusCode(StatusCodes.Status408RequestTimeout);
            }
        });
        app.MapPost("/profile/request/attempt/{milliseconds:int}", async (
            int milliseconds,
            ProfileReq request,
            IZLinkRouteClient channel) =>
        {
            try
            {
                var reply = await channel.RequestToChannel(
                        ResilienceLifecycleNames.Channel, request)
                    .Timeout(TimeSpan.FromMilliseconds(milliseconds))
                    .Async<ProfileRes>();
                return new ProfileAttemptRes(reply, null, false);
            }
            catch (TimeoutException)
            {
                return new ProfileAttemptRes(null, nameof(TimeoutException), true);
            }
            catch (ZLinkFrameworkException error)
            {
                var retryable = error.Kind is ZLinkFrameworkErrorKind.Unavailable
                    or ZLinkFrameworkErrorKind.CapacityExceeded
                    or ZLinkFrameworkErrorKind.DeadlineExceeded
                    or ZLinkFrameworkErrorKind.ShuttingDown;
                return new ProfileAttemptRes(null, error.Kind.ToString(), retryable);
            }
        });
        app.MapPost("/profile/request/missing", async (
            ProfileReq request,
            IZLinkRouteClient channel) =>
        {
            try
            {
                var reply = await channel.RequestToChannel(
                        ResilienceLifecycleNames.Channel,
                        new MissingProfileReq(request.Value, request.Marker))
                    .Timeout(TimeSpan.FromSeconds(3))
                    .Async<ProfileRes>();
                return Results.Ok(reply);
            }
            catch (Exception ex)
            {
                return Results.Problem(ex.Message);
            }
        });
        app.MapPost("/profile/command", async (
            ProfileMsg command,
            IZLinkRouteClient channel,
            CancellationToken cancellationToken) =>
        {
            await channel.SendToChannel(
                    ResilienceLifecycleNames.Channel, command)
                .Async(cancellationToken);
            return Results.Ok(new { status = "sent" });
        });
        return app;
    }

    // Automatic RIDs are 'prefix-<uuid v4>' (spec 13 §3.1), so the provider
    // role name a probe asks about is the prefix, not the whole RID. The
    // trailing separator keeps 'api-a' from matching an 'api-ab' node.
    static bool MatchesRole(string nodeRid, string role) =>
        nodeRid.Equals(role, StringComparison.Ordinal)
        || nodeRid.StartsWith(role + "-", StringComparison.Ordinal);

    // A caller joins the providers' RouteMesh with its own membership and
    // issues ChannelName select-one calls through IZLinkRouteClient. The bind
    // uses an ephemeral port and a Framework-generated lifecycle identity.
    internal static void JoinConsumerMesh(IZLinkFrameworkOptions framework, string ridPrefix)
    {
        var mesh = framework.AddRouteMesh(ResilienceLifecycleNames.Channel)
            .Listen("tcp://127.0.0.1:0")
            .SetRoutingIdPrefix(ridPrefix);
        mesh.Channel(ResilienceLifecycleNames.Channel).Client();
    }

    static async Task<ProfileRes> RequestProfileAsync(
        IZLinkRouteClient channel,
        ProfileReq request)
        => await channel.RequestToChannel(
                        ResilienceLifecycleNames.Channel, request)
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<ProfileRes>();
}

internal sealed record ConsumerOptions(
    string HttpUrl,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string LogDir)
{
    public static ConsumerOptions Parse(string[] args)
        => E2eConfiguration.Load<ConsumerOptions>(args);
}

internal sealed record TopologyEntryRes(
    string? RoutingId,
    string Endpoint,
    string State,
    uint Weight,
    long Generation,
    bool Draining);

internal sealed class ConnectionEvidence
{
    private readonly System.Collections.Concurrent.ConcurrentQueue<string> _entries = new();
    // A pulse completed on every Add and swapped for a fresh one, so EVERY
    // concurrent waiter wakes — a counted semaphore hands one release to one
    // waiter and silently starves the rest.
    private readonly object _pulseGate = new();
    private TaskCompletionSource<bool> _pulse =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    public void Add(string entry)
    {
        _entries.Enqueue(entry);
        TaskCompletionSource<bool> pulse;
        lock (_pulseGate)
        {
            pulse = _pulse;
            _pulse = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
        }

        pulse.TrySetResult(true);
    }

    public async Task<string[]> WaitAsync(
        IReadOnlyCollection<string> required,
        int afterCount,
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

            var snapshot = _entries.ToArray();
            if (snapshot.Skip(Math.Clamp(afterCount, 0, snapshot.Length)).Any(line =>
                    required.All(expected => line.Contains(expected, StringComparison.Ordinal)))) return snapshot;

            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining <= TimeSpan.Zero
                || await Task.WhenAny(pulseTask, Task.Delay(remaining, cancellationToken)) != pulseTask)
                throw new TimeoutException("Expected connection evidence did not arrive.");
        }
    }

    public async Task<string[]> WaitAllAsync(
        IReadOnlyCollection<string> requiredEvents,
        int afterCount,
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

            var snapshot = _entries.ToArray();
            var added = snapshot.Skip(Math.Clamp(afterCount, 0, snapshot.Length)).ToArray();
            if (requiredEvents.All(expected => added.Any(line =>
                    line.Contains(expected, StringComparison.Ordinal)))) return snapshot;

            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining <= TimeSpan.Zero
                || await Task.WhenAny(pulseTask, Task.Delay(remaining, cancellationToken)) != pulseTask)
                throw new TimeoutException("Expected connection evidence did not arrive.");
        }
    }


    public string[] Snapshot() => _entries.ToArray();
}

// Polls the mesh runtime snapshot and turns peer ready transitions into the
// connection-evidence lines the scenarios wait on. Wire-level socket sources
// do not exist for mesh nodes (spec 50 owns the mesh monitoring surface).
internal sealed class MeshConnectionObserverService(
    IServiceProvider services,
    ConnectionEvidence evidence,
    string linePrefix) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        var meshRuntime = services.GetRequiredService<IZLinkRouteMeshRuntime>();
        var query = services.GetRequiredService<IZLinkLocationRuntimeQuery>();
        var ready = new Dictionary<string, string>(StringComparer.Ordinal);
        while (!stoppingToken.IsCancellationRequested)
        {
            try
            {
                var snapshot = meshRuntime.GetStatus(ResilienceLifecycleNames.Channel);
                // The topology query carries the endpoint currently
                // advertised for each Ready peer.
                var rows = await query.ListTopologyAsync(
                    new ZLinkLocationTopologyFilter(ResilienceLifecycleNames.Channel),
                    cancellationToken: stoppingToken);
                var advertised = rows.Items.ToDictionary(
                    static row => row.NodeRid.ToString(),
                    static row => row.Endpoint,
                    StringComparer.Ordinal);
                var current = snapshot.Peers
                    .Where(static peer => peer.State == ZLinkPeerState.Ready)
                    .ToDictionary(
                        static peer => peer.NodeRid.ToString(),
                        peer => advertised.TryGetValue(peer.NodeRid.ToString(), out var endpoint)
                            ? endpoint
                            : string.Empty,
                        StringComparer.Ordinal);
                foreach (var (rid, endpoint) in current)
                    if (!ready.TryGetValue(rid, out var previousEndpoint))
                    {
                        evidence.Add($"{linePrefix}kind=ConnectionReady|remote={endpoint}|routing={rid}");
                    }
                    else if (!string.Equals(previousEndpoint, endpoint, StringComparison.Ordinal))
                    {
                        // A same-RID replacement may keep the logical peer
                        // continuously ready while its physical endpoint
                        // changes. Preserve both edges so operators can
                        // distinguish a handover from an unchanged peer.
                        evidence.Add(
                            $"{linePrefix}kind=Disconnected|remote={previousEndpoint}|routing={rid}");
                        evidence.Add(
                            $"{linePrefix}kind=ConnectionReady|remote={endpoint}|routing={rid}");
                    }
                foreach (var (rid, endpoint) in ready)
                    if (!current.ContainsKey(rid))
                    {
                        evidence.Add($"{linePrefix}kind=Disconnected|remote={endpoint}|routing={rid}");
                    }
                ready = current;
            }
            catch (Exception)
            {
                // The mesh node may not be started yet; keep polling.
            }

            try
            {
                await Task.Delay(TimeSpan.FromMilliseconds(100), stoppingToken);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }
}
