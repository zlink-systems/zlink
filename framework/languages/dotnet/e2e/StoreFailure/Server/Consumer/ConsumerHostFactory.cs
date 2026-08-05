using System.Diagnostics;
using Microsoft.Extensions.Configuration;

using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using StoreFailure.Shared;
using Zlink.Framework;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.LocationProvider;

using Systems.Zlink;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Diagnostics;

namespace StoreFailure.Server.Consumer;

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
        var delayState = new LocationStoreDelayState();
        builder.Services.AddSingleton(delayState);
        builder.Logging.ClearProviders();
        builder.Logging.AddSimpleConsole(console =>
        {
            console.SingleLine = true;
            console.TimestampFormat = "HH:mm:ss.fff ";
        });
        builder.WebHost.UseUrls(options.HttpUrl);
        builder.Services.AddSingleton(new E2eMessageFlowListener(
            Path.Combine(options.LogDir, $"{options.TraceLabel}-flow.log"),
            options.TraceLabel));
        builder.Services.AddZLinkFramework(framework =>
        {
            //  This E2E host is not started inside a memory-limited
            //  container. Supply a deterministic finite limit so the
            //  default Auto HWM contract does not depend on the host.
            framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
                1UL * 1024 * 1024 * 1024;
            var redisStore = new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = options.RedisKeyPrefix; });
            IZLinkLocationStore store = options.StoreMode switch
            {
                // The opaque provider SPI has no optional notification
                // surface. All providers therefore use the same polling
                // correctness path.
                "polling" => redisStore,
                "delay" => new DelayableLocationStore(redisStore, delayState),
                _ => redisStore
            };
            framework.AddLocationStore(store);
            var locations = framework.ConfigureLocations();
            locations.OwnerLeaseRenewInterval = TimeSpan.FromMilliseconds(options.LocationHeartbeatMs);
            locations.OwnerLeaseTtl = TimeSpan.FromMilliseconds(options.LocationLeaseTtlMs);
            // The E2E compresses the lease TTL to three seconds. Scale the
            // renewal attempt bound with the heartbeat as well; retaining
            // the production three-second default would consume the whole
            // lease before the failure could become observable.
            locations.OwnerLeaseRenewTimeout = TimeSpan.FromMilliseconds(
                Math.Max(100, options.LocationHeartbeatMs / 2));
            // The fencing margin needs the same treatment. Spec 21 section 4
            // requires renew interval + renew timeout to stay below TTL minus
            // margin, and the production five-second margin already exceeds
            // this lease on its own, so startup would fail validation.
            locations.OwnerLeaseFencingMargin = TimeSpan.FromMilliseconds(
                Math.Max(100, options.LocationLeaseTtlMs / 3));
            locations.PollingInterval = TimeSpan.FromMilliseconds(options.LocationPollingMs);
            locations.StoreFailureGrace = TimeSpan.FromMilliseconds(options.LocationGraceMs);
            framework.ConfigureDispatch().Diagnostics
                .SetLevel(ZLinkDiagnosticsLevel.Normal);
            // SF-A2 starts a second consumer against the same RouteMesh. Its
            // trace label is also its stable harness identity, so stopping
            // that observer cannot hand over and then remove the primary
            // consumer's descriptor under the same routing ID.
            JoinConsumerMesh(framework, options.TraceLabel);
        });

        var app = builder.Build();
        app.MapGet("/health", () => Results.Ok(new { status = "ready" }));
        app.MapPost("/shutdown", (IHostApplicationLifetime lifetime) =>
        {
            lifetime.StopApplication();
            return Results.Ok(new { status = "stopping" });
        });
        app.MapGet("/query/status", async (
            IZLinkLocationRuntimeQuery query,
            CancellationToken cancellationToken) =>
        {
            var status = await query.GetStatusAsync(cancellationToken);
            return Results.Ok(new RuntimeStatusRes(
                status.StoreHealthy,
                status.OwnerLeaseHealthy,
                status.OwnerLeaseRenewedAt,
                status.LastRefreshAt));
        });
        app.MapGet("/query/peers", async (
            IZLinkLocationRuntimeQuery query,
            CancellationToken cancellationToken) =>
        {
            try
            {
                var peers = await query.ListTopologyAsync(
                    new ZLinkLocationTopologyFilter(StoreFailureNames.Channel),
                    cancellationToken: cancellationToken);
                return Results.Ok(peers.Items
                    .Select(peer => new PeerRowRes(
                        peer.NodeRid.ToString(),
                        peer.Endpoint,
                        peer.Draining))
                    .ToArray());
            }
            catch (Exception error)
            {
                // A dead store makes the raw row list unavailable; the
                // probe treats 503 as "store unreachable".
                return Results.Problem(error.Message, statusCode: StatusCodes.Status503ServiceUnavailable);
            }
        });
        app.MapPost("/query/peers/wait", async (
            PeerRowsWaitReq request,
            IZLinkLocationRuntimeQuery query,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 60000));
            var elapsed = Stopwatch.StartNew();
            while (elapsed.Elapsed < timeout)
            {
                try
                {
                    var rows = (await query.ListTopologyAsync(
                            new ZLinkLocationTopologyFilter(StoreFailureNames.Channel),
                            cancellationToken: cancellationToken))
                        .Items.Select(peer => new PeerRowRes(
                            peer.NodeRid.ToString(),
                            peer.Endpoint,
                            peer.Draining))
                        .ToArray();
                    var reached = request.PresentRids.All(rid => rows.Any(row => MatchesRole(row.Rid, rid)))
                                  && request.AbsentRids.All(rid => rows.All(row => !MatchesRole(row.Rid, rid)))
                                  && request.DrainingRids.All(rid =>
                                      rows.Any(row => MatchesRole(row.Rid, rid) && row.Draining));
                    if (reached) return Results.Ok(rows);
                }
                catch
                {
                    // A store outage is a transient observation while this bounded wait is active.
                }

                await Task.Delay(TimeSpan.FromMilliseconds(100), cancellationToken);
            }

            return Results.Problem("Peer rows did not reach the requested state.",
                statusCode: StatusCodes.Status504GatewayTimeout);
        });
        app.MapPost("/query/routes/wait", async (
            RouteReadyWaitReq request,
            IZLinkRouteMeshRuntime runtime,
            ILoggerFactory loggerFactory,
            CancellationToken cancellationToken) =>
        {
            var logger = loggerFactory.CreateLogger("StoreFailure.RouteProbe");
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 60000));
            var elapsed = Stopwatch.StartNew();
            while (elapsed.Elapsed < timeout)
            {
                var current = runtime.GetStatus(StoreFailureNames.Channel);
                var readyMembers = current.Channels
                    .Where(channel => channel.ChannelName == StoreFailureNames.Channel)
                    .Select(channel => channel.ReadyTargetCount)
                    .DefaultIfEmpty()
                    .Max();
                var reached = readyMembers >= request.MinimumReadyMembers
                              && request.ReadyRids.All(rid => current.Peers.Any(peer =>
                                  MatchesRole(peer.NodeRid.ToString(), rid) && peer.State == ZLinkPeerState.Ready))
                              && request.NotReadyRids.All(rid => current.Peers.All(peer =>
                                  !MatchesRole(peer.NodeRid.ToString(), rid) || peer.State != ZLinkPeerState.Ready));
                if (reached)
                    return Results.Ok(new RouteReadyRes(readyMembers));

                await Task.Delay(TimeSpan.FromMilliseconds(50), cancellationToken);
            }

            var snapshot = runtime.GetStatus(StoreFailureNames.Channel);
            logger.LogWarning(
                "Route readiness wait expired mesh={MeshName} localRid={LocalRid} state={State} " +
                "peers={Peers} channels={Channels}",
                snapshot.MeshName,
                string.Empty,
                snapshot.State,
                DescribePeers(snapshot),
                DescribeChannels(snapshot));

            return Results.Problem("Route did not reach the requested ready-member count.",
                statusCode: StatusCodes.Status504GatewayTimeout);
        });
        app.MapPost("/query/status/wait", async (
            RuntimeStatusWaitReq request,
            IZLinkLocationRuntimeQuery query,
            CancellationToken cancellationToken) =>
        {
            var response = await RuntimeStatusWaiter.WaitAsync(async token =>
            {
                var status = await query.GetStatusAsync(token);
                return new RuntimeStatusRes(
                    status.StoreHealthy,
                    status.OwnerLeaseHealthy, status.OwnerLeaseRenewedAt, status.LastRefreshAt);
            }, request, cancellationToken);
            if (response is not null) return Results.Ok(response);

            return Results.Problem("Runtime status did not reach the requested state.",
                statusCode: StatusCodes.Status504GatewayTimeout);
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
            IZLinkRouteClient channel,
            IZLinkRouteMeshRuntime runtime,
            ILoggerFactory loggerFactory) =>
        {
            try
            {
                var reply = await channel.RequestToChannel(
                    StoreFailureNames.Channel, request)
                    .Timeout(TimeSpan.FromMilliseconds(milliseconds))
                    .Async<ProfileRes>();
                return Results.Ok(reply);
            }
            catch (TimeoutException)
            {
                return Results.StatusCode(StatusCodes.Status408RequestTimeout);
            }
            catch (Exception error)
            {
                var snapshot = runtime.GetStatus(StoreFailureNames.Channel);
                loggerFactory.CreateLogger("StoreFailure.RequestProbe").LogError(
                    error,
                    "Profile request failed marker={Marker} peers={Peers} channels={Channels}",
                    request.Marker,
                    DescribePeers(snapshot),
                    DescribeChannels(snapshot));
                throw;
            }
        });
        app.MapPost("/profile/request/missing", async (
            ProfileReq request,
            IZLinkRouteClient channel) =>
        {
            try
            {
                var reply = await channel.RequestToChannel(
                    StoreFailureNames.Channel, request)
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
                    StoreFailureNames.Channel, command)
                .Async(cancellationToken);
            return Results.Ok(new { status = "sent" });
        });
        app.MapPost("/profile/request/new-client", async (ProfileReq request) =>
        {
            using var host = CreateClientHost(options, $"storm-{request.Marker}");
            await host.StartAsync();
            try
            {
                var channel = host.Services.GetRequiredService<IZLinkRouteClient>();
                var reply = await RequestProfileAsync(channel, request);
                return Results.Ok(reply);
            }
            finally
            {
                await StopClientHostAsync(host);
            }
        });
        app.MapPost("/admin/store-delay", (StoreDelayReq request, LocationStoreDelayState state) =>
        {
            state.SetDelay(TimeSpan.FromMilliseconds(request.Milliseconds));
            return Results.Ok(new { delayMilliseconds = state.DelayMilliseconds });
        });
        return app;
    }

    static IHost CreateClientHost(ConsumerOptions options, string traceLabel)
    {
        return Host.CreateDefaultBuilder()
            .ConfigureAppConfiguration((_, configuration) => configuration.Sources.Clear())
            .ConfigureServices(services =>
            {
                services.AddSingleton(new E2eMessageFlowListener(
                    Path.Combine(options.LogDir, $"{traceLabel}-flow.log"),
                    traceLabel));
                services.AddZLinkFramework(framework =>
                {
                    framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = options.RedisKeyPrefix; }));
                    framework.ConfigureDispatch().Diagnostics
                        .SetLevel(ZLinkDiagnosticsLevel.Normal);
                    JoinConsumerMesh(framework, $"consumer-{traceLabel}");
                });
            })
            .Build();
    }

    // A caller joins the providers' RouteMesh with its own membership and
    // issues ChannelName select-one calls through IZLinkRouteClient (spec 10
    // §1). The bind uses an ephemeral port. Spec 13 §3.3 allows a fixed RID
    // only in an explicit manual topology, and these hosts discover each other
    // through the Location Store, so the harness role name is a diagnostic
    // prefix and the Framework issues the RID itself.
    // Automatic RIDs are 'prefix-<uuid v4>' (spec 13 §3.1), so the harness
    // role name a scenario asks about is the prefix, not the whole RID. The
    // trailing separator keeps 'api-a' from matching an 'api-ab' node.
    static bool MatchesRole(string nodeRid, string role) =>
        nodeRid.Equals(role, StringComparison.Ordinal)
        || nodeRid.StartsWith(role + "-", StringComparison.Ordinal);

    static void JoinConsumerMesh(IZLinkFrameworkOptions framework, string rid)
    {
        var mesh = framework.AddRouteMesh(StoreFailureNames.Channel)
            .Listen("tcp://127.0.0.1:0")
            .SetRoutingIdPrefix(rid);
        // The Client membership has to name the channel the providers serve.
        // A membership on any other name leaves the select-one call with no
        // process-local client for 'storefailure.profile'.
        mesh.Channel(StoreFailureNames.Channel).Client();
    }

    static async Task StopClientHostAsync(IHost host)
    {
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        try
        {
            await host.StopAsync(cts.Token);
        }
        catch (OperationCanceledException)
        {
            // A reconnect storm scenario must not hang the HTTP response while host shutdown waits.
        }
    }

    static async Task<ProfileRes> RequestProfileAsync(
        IZLinkRouteClient channel,
        ProfileReq request)
        => await channel.RequestToChannel(
                    StoreFailureNames.Channel, request)
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<ProfileRes>();

    static string DescribePeers(ZLinkRouteMeshStatus snapshot) =>
        string.Join(";", snapshot.Peers.Select(peer =>
            $"{peer.NodeRid}|state={peer.State}|reason={peer.UnavailableReason}"));

    static string DescribeChannels(ZLinkRouteMeshStatus snapshot) =>
        string.Join(";", snapshot.Channels.Select(channel =>
            $"{channel.ChannelName}|ready={channel.ReadyTargetCount}|selectable={channel.IsReady}"));
}

internal sealed record ConsumerOptions(
    string HttpUrl,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string LogDir,
    string TraceLabel,
    string StoreMode,
    int LocationHeartbeatMs,
    int LocationLeaseTtlMs,
    int LocationPollingMs,
    int LocationGraceMs)
{
    public static ConsumerOptions Parse(string[] args)
        => E2eConfiguration.Load<ConsumerOptions>(args);
}
