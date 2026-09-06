using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);
if (config.scenario != "channel-echo-only" || config.role != "channel")
    throw new ArgumentException("ChannelServer supports the channel-echo-only source and target.");
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
builder.Services.AddSingleton<ChannelEchoOnlyScenario>();
var app = builder.Build();
var scenario = app.Services.GetRequiredService<ChannelEchoOnlyScenario>();
ServerApplication.Map(app, config.source ? scenario.RunAsync : null);
await app.StartAsync();
if (config.source) await scenario.PrepareAsync(app.Lifetime.ApplicationStopping);
await app.WaitForShutdownAsync();
