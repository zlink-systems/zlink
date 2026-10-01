using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Configuration;

namespace ZLink.Framework.Perf;

// §11.2 role host: manual RouteMesh or ClientServer, no Store and no objects.
public static class ChannelEchoOnlyHost
{
    public static async Task RunAsync(RoleConfig config)
    {
        if (config.role != "channel") throw new ArgumentException("ChannelServer supports the channel-echo-only source and target.");
        var builder = ServerApplication.Builder(config, options =>
        {
            if (config.topology == "routemesh")
            {
                var mesh = options.AddRouteMesh(config.meshName!).Listen(config.listenerEndpoint!);
                if (config.source)
                {
                    mesh.Channel(config.channelName!).Client();
                    mesh.PeerConnections.Connect(config.peerEndpoint!);
                }
                else mesh.Channel(config.channelName!).Server().AddRequestHandler<ChannelEchoHandler, PerfEchoRequest, PerfEchoReply>();
            }
            else if (config.topology == "clientserver")
            {
                var channel = options.AddClientServerChannel(config.channelName!);
                if (config.source) channel.Client().Connect(config.peerEndpoint!);
                else channel.Server().Listen(new Uri(config.listenerEndpoint!).Port)
                    .AddRequestHandler<ChannelEchoHandler, PerfEchoRequest, PerfEchoReply>();
            }
            else throw new ArgumentException("Unsupported channel topology.");
        });
        if (config.source)
        {
            builder.Services.AddSingleton(new ObjectsReadiness(false, "The Channel target probe has not completed."));
            builder.Services.AddSingleton<ChannelEchoOnlyScenario>();
        }
        var app = builder.Build();
        var scenario = config.source ? app.Services.GetRequiredService<ChannelEchoOnlyScenario>() : null;
        ServerApplication.Map(app, scenario is null ? null : scenario.RunAsync);
        await app.StartAsync();
        if (scenario is not null) await scenario.PrepareAsync(app.Lifetime.ApplicationStopping);
        await app.WaitForShutdownAsync();
    }
}
