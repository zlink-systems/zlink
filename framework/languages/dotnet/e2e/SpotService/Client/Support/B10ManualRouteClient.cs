using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;

namespace SpotService.Client.Support;

internal static class B10ManualRouteClient
{
    private static readonly TimeSpan ReadyDeadline = TimeSpan.FromSeconds(10);

    public static async Task<ControlPingRes> RequestAsync(
        string controlEndpoint,
        string controlRid,
        ControlPingReq request,
        CancellationToken cancellationToken = default)
    {
        using var host = Host.CreateDefaultBuilder()
            .ConfigureAppConfiguration(static configuration =>
                configuration.Sources.Clear())
            .ConfigureLogging(static logging => logging.ClearProviders())
            .ConfigureServices(services =>
            {
                services.AddZLinkFramework(framework =>
                {
                    var mesh = framework.AddRouteMesh(SpotServiceNames.ControlChannel)
                        .Listen("tcp://127.0.0.1:0")
                        .SetRoutingId(RoutingId.From("b10-requester"));
                    mesh.PeerConnections.Connect(
                        RoutingId.From(controlRid),
                        controlEndpoint);
                    mesh.Channel(SpotServiceNames.ControlChannel).Client();
                });
            })
            .Build();

        await host.StartAsync(cancellationToken);
        try
        {
            var runtime = host.Services.GetRequiredService<IZLinkRouteMeshRuntime>();
            await WaitForReadyPeerAsync(runtime, cancellationToken);
            return await host.Services
                .GetRequiredService<IZLinkRouteClient>()
                .RequestToChannel(SpotServiceNames.ControlChannel, request)
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<ControlPingRes>(cancellationToken);
        }
        finally
        {
            await host.StopAsync(CancellationToken.None);
        }
    }

    private static async Task WaitForReadyPeerAsync(
        IZLinkRouteMeshRuntime runtime,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + ReadyDeadline;
        while (DateTimeOffset.UtcNow < deadline)
        {
            var status = runtime.GetStatus(SpotServiceNames.ControlChannel);
            if (status.Peers.Any(static peer => peer.State == ZLinkPeerState.Ready)
                && status.Channels.Any(static channel =>
                    channel.ChannelName == SpotServiceNames.ControlChannel
                    && channel.IsReady))
                return;

            await Task.Delay(TimeSpan.FromMilliseconds(50), cancellationToken);
        }

        var lastStatus = runtime.GetStatus(SpotServiceNames.ControlChannel);
        throw new TimeoutException(
            "SM-B10 RouteMesh client did not observe a ready manual host: "
            + $"peers={string.Join(';', lastStatus.Peers.Select(static peer =>
                $"{peer.NodeRid}:{peer.State}"))};"
            + $"channels={string.Join(';', lastStatus.Channels.Select(static channel =>
                $"{channel.ChannelName}:ready={channel.IsReady}"))}.");
    }
}
