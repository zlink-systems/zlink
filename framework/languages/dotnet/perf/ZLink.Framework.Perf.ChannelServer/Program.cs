using ZLink.Framework.Perf;

var config = ServerApplication.ReadConfig(args);

var builder = ServerApplication.Builder(config, options =>
{
    if (config.scenario != "channel-echo-only") { PerfScenario.Configure(options, config); return; }
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
builder.Services.AddSingleton<PerfScenario>();
var app = builder.Build();
var baseline = config.scenario == "channel-echo-only";
var scenario = app.Services.GetRequiredService<PerfScenario>();
var channel = baseline ? app.Services.GetRequiredService<ChannelEchoOnlyScenario>() : null;
ServerApplication.Map(app, config.source ? (baseline ? channel!.RunAsync : scenario.RunAsync) : null,
    baseline ? (config.source ? channel!.PrepareAsync : null) : scenario.PrepareAsync);
await app.StartAsync();

await app.WaitForShutdownAsync();
