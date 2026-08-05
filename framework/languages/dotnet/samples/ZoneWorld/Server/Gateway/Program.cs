using Microsoft.Extensions.Configuration;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;
using ZoneWorld.Server.Configuration;
using ZoneWorld.Server.Gateway.Infrastructure.ZLink.Sessions;
using ZoneWorld.Shared.Contracts;

var configuration = ZoneWorldConfiguration.Load(args);
var shared = configuration.Shared;
var gateway = configuration.Gateway
              ?? throw new InvalidOperationException("Gateway configuration is required.");

var builder = Host.CreateApplicationBuilder(args);
builder.Configuration.Sources.Clear();
builder.Configuration.AddInMemoryCollection();
builder.Logging.ClearProviders();
builder.Logging.AddSimpleConsole(console =>
{
    console.SingleLine = true;
    console.TimestampFormat = "HH:mm:ss.fff ";
});

builder.Services.AddSingleton(shared);
builder.Services.AddSingleton(gateway);
builder.Services.AddSingleton<PlayerSessionBinder>();
builder.Services.AddSingleton<RelocationProbeService>();

builder.Services.AddZLinkFramework(options =>
{
    options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
    {
        redis.ConnectionString = shared.RedisEndpoint;
        redis.KeyPrefix = shared.RedisKeyPrefix;
    }));
    options.ConfigureDispatch()
        .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
    options.AddHandlersFromAssemblyOf(typeof(PlayerSession));

    // The browser's end of the world.
    options.AddStreamNode(ZoneWorldNames.GatewayStreamNode)
        .Bind(gateway.StreamEndpoint)
        .EnableActorDispatch()
        .AddSession<PlayerSession>();

    // The Gateway joins the spot mesh but hosts nothing in it — no entry spot, no actor
    // factory. Membership is what lets it bind a session to an actor living on a zone
    // node and relay packets to it.
    var mesh = options.AddRouteMesh(ZoneWorldNames.MeshName)
        .SetRoutingIdPrefix("gw0")
        .Listen(gateway.MeshEndpoint);
    mesh.Objects().Client();
});

await builder.Build().RunAsync();
