using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using ResilienceLifecycle.Shared;
using Systems.Zlink;
using Zlink.Framework;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Diagnostics;

namespace ResilienceLifecycle.Client.Support;

internal static class EphemeralRouteClient
{
    // A retiring host publishes drain state for one polling interval plus
    // the bounded location-store read window before it removes its row. The
    // next host can therefore need more than the five-second request timeout
    // to become visible even though its eventual request still uses that
    // stricter timeout.
    private static readonly TimeSpan PeerDiscoveryDeadline = TimeSpan.FromSeconds(15);

    public static async Task<ProfileRes> RequestAsync(
        ClientOptions options,
        ProfileReq request,
        CancellationToken cancellationToken = default)
    {
        await using var session = await EphemeralRouteSession.StartAsync(
            options,
            request.Marker,
            cancellationToken);
        return await session.RequestAsync(request, cancellationToken);
    }

    internal static IHostBuilder CreateHostBuilder(ClientOptions options, string identity)
        => Host.CreateDefaultBuilder()
            .ConfigureAppConfiguration(static configuration =>
                configuration.Sources.Clear())
            .ConfigureLogging(static logging => logging.ClearProviders())
            .ConfigureServices(services =>
            {
                services.AddSingleton(new E2eMessageFlowListener(
                    Path.Combine(options.LogDir, $"ephemeral-{identity}-flow.log"),
                    $"ephemeral-{identity}"));
                services.AddZLinkFramework(framework =>
                {
                    //  This E2E host is not started inside a memory-limited
                    //  container. Supply a deterministic finite limit so the
                    //  default Auto HWM contract does not depend on the host.
                    framework.ConfigureCoreHwm().CoreHwmMemoryLimitBytes =
                        1UL * 1024 * 1024 * 1024;
                    framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = options.RedisKeyPrefix; }));
                    framework.ConfigureDispatch().Diagnostics
                        .SetLevel(ZLinkDiagnosticsLevel.Normal);
                    var mesh = framework.AddRouteMesh(ResilienceLifecycleNames.Channel)
                        .Listen("tcp://127.0.0.1:0")
                        .SetRoutingIdPrefix($"ephemeral-{identity}");
                    mesh.Channel(ResilienceLifecycleNames.Channel).Client();
                });
            });

    internal static async Task WaitForReadyPeerAsync(
        IZLinkRouteMeshRuntime runtime,
        IZLinkLocationRuntimeQuery locations,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + PeerDiscoveryDeadline;
        string? lastSnapshot = null;
        while (DateTimeOffset.UtcNow < deadline)
        {
            var snapshot = runtime.GetStatus(ResilienceLifecycleNames.Channel);
            lastSnapshot = string.Join(
                ";",
                snapshot.Peers.Select(peer =>
                    $"{peer.NodeRid}:state={peer.State}"))
                + "|local="
                + string.Join(
                    ";",
                    snapshot.Channels.Select(channel =>
                        $"{channel.ChannelName}:ready={channel.ReadyTargetCount}:selectable={channel.IsReady}"));
            if (snapshot.Peers.Any(peer =>
                    peer.State == ZLinkPeerState.Ready)
                && snapshot.Channels.Any(channel =>
                    channel.ChannelName == ResilienceLifecycleNames.Channel
                    && channel.IsReady))
                return;
            await Task.Delay(TimeSpan.FromMilliseconds(100), cancellationToken);
        }

        var descriptors = await locations.ListTopologyAsync(
            new ZLinkLocationTopologyFilter(ResilienceLifecycleNames.Channel),
            cancellationToken: cancellationToken);
        var descriptorText = string.Join(
            ";",
            descriptors.Items.Select(descriptor =>
                $"{descriptor.NodeRid}@{descriptor.Endpoint}:updated={descriptor.UpdatedAt:O}"));
        throw new TimeoutException(
            "Ephemeral client did not observe a selectable provider: "
            + $"{lastSnapshot}|descriptors={descriptorText}.");
    }
}

internal sealed class EphemeralRouteSession : IAsyncDisposable
{
    private readonly IHost _host;
    private readonly IZLinkRouteClient _client;
    private int _disposed;

    private EphemeralRouteSession(IHost host)
    {
        _host = host;
        _client = host.Services.GetRequiredService<IZLinkRouteClient>();
    }

    public static async Task<EphemeralRouteSession> StartAsync(
        ClientOptions options,
        string identity,
        CancellationToken cancellationToken = default)
    {
        var host = EphemeralRouteClient.CreateHostBuilder(options, identity).Build();
        try
        {
            await host.StartAsync(cancellationToken);
            var session = new EphemeralRouteSession(host);
            await EphemeralRouteClient.WaitForReadyPeerAsync(
                host.Services.GetRequiredService<IZLinkRouteMeshRuntime>(),
                host.Services.GetRequiredService<IZLinkLocationRuntimeQuery>(),
                cancellationToken);
            return session;
        }
        catch
        {
            await host.StopAsync(CancellationToken.None);
            host.Dispose();
            throw;
        }
    }

    public ValueTask<ProfileRes> RequestAsync(
        ProfileReq request,
        CancellationToken cancellationToken = default)
        => _client.RequestToChannel(ResilienceLifecycleNames.Channel, request)
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<ProfileRes>(cancellationToken);

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;

        try
        {
            await _host.StopAsync(CancellationToken.None);
        }
        finally
        {
            _host.Dispose();
        }
    }
}
