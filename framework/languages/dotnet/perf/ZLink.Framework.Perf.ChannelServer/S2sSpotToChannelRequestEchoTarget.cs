using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;

namespace ZLink.Framework.Perf;

// The Channel echo target of §10.5: an automatic RouteMesh node (run Docker Redis) that serves this cell's
// ChannelName with the typed request handler. The measured operation lives in the Spot process; this side echoes.
public static class S2sSpotToChannelRequestEchoTarget
{
    public static async Task RunAsync(RoleConfig config)
    {
        if (config.role != "channel" || config.source) throw new ArgumentException("The Channel role is the echo target of this scenario.");
        var builder = ServerApplication.Builder(config, options =>
            options.AddRouteMesh(config.meshName!).SetRoutingIdPrefix("perf-channel").Listen(config.transportEndpoints["mesh"])
                .Channel(config.channelName!).Server().AddRequestHandler<ChannelEchoHandler, PerfEchoRequest, PerfEchoReply>());
        var app = builder.Build();
        ServerApplication.Map(app);
        await app.RunAsync();
    }
}
