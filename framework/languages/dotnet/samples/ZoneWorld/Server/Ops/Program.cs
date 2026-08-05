using Microsoft.Extensions.Configuration;
using StackExchange.Redis;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;
using ZoneWorld.Server.Configuration;
using ZoneWorld.Server.Ops.Application.Ops;
using ZoneWorld.Server.Ops.Infrastructure.Store;
using ZoneWorld.Server.Ops.Infrastructure.ZLink;
using ZoneWorld.Server.Ops.Infrastructure.ZLink.Handlers;
using ZoneWorld.Server.Ops.Infrastructure.ZLink.Monitoring;
using ZoneWorld.Server.Ops.Infrastructure.ZLink.Sessions;
using ZoneWorld.Server.Ops.Ports;
using ZoneWorld.Shared.Contracts;

var configuration = ZoneWorldConfiguration.Load(args);
var shared = configuration.Shared;
var ops = configuration.Ops
          ?? throw new InvalidOperationException("Ops configuration is required.");

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
builder.Services.AddSingleton(ops);
builder.Services.AddSingleton<IConnectionMultiplexer>(
    _ => ConnectionMultiplexer.Connect(shared.RedisEndpoint));
builder.Services.AddSingleton<IMaintenanceStorePort>(services =>
    new MaintenanceStoreRepository(
        services.GetRequiredService<IConnectionMultiplexer>(),
        shared.RedisKeyPrefix));
builder.Services.AddSingleton<NodeRegistry>();
builder.Services.AddSingleton<OpsConsoleRegistry>();
builder.Services.AddSingleton<IWorldOperationsPort, WorldOperationsAdapter>();
builder.Services.AddSingleton<AnnouncementService>();
builder.Services.AddSingleton<MaintenanceService>();
builder.Services.AddSingleton<NodeDiagnosticsService>();
builder.Services.AddZLinkFramework(options =>
{
    options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
    {
        redis.ConnectionString = shared.RedisEndpoint;
        redis.KeyPrefix = shared.RedisKeyPrefix;
    }));
    // These values govern registrations owned by Ops. Zone nodes keep the documented 30-second
    // defaults, so crash scenarios still exercise real lease expiry (§4.2 and §8.1).
    var locations = options.ConfigureLocations();
    locations.OwnerLeaseRenewInterval = TimeSpan.FromSeconds(1);
    locations.OwnerLeaseTtl = TimeSpan.FromSeconds(3);
    locations.OwnerLeaseFencingMargin = TimeSpan.FromSeconds(1);
    locations.OwnerLeaseRenewTimeout = TimeSpan.FromMilliseconds(500);

    options.ConfigureDispatch()
        .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
    options.AddHandlersFromAssemblyOf(typeof(OpsConsoleSession));

    options.AddStreamNode(ZoneWorldNames.OpsStreamNode)
        .Bind(ops.StreamEndpoint)
        .AddSession<OpsConsoleSession>();

    // The announcement and the maintenance change both leave here without a node list.
    // Adding a node changes nothing on this side — that is the whole point (ZW-D2).
    options.AddFanoutChannel(ZoneWorldNames.BroadcastChannel)
        .EnablePublisher(ops.BroadcastEndpoint);

    var mesh = options.AddRouteMesh(ZoneWorldNames.MeshName)
        .Listen(ops.MeshEndpoint)
        .SetRoutingIdPrefix("ops");
    mesh.Channel(ZoneWorldNames.ReportChannel).Server()
        .AddHandlerGroup(HandlerGroups.Ops);
});

// Hosted services start in registration order. Observe the RouteMesh only after
// the framework has started and published its initial runtime state.
builder.Services.AddHostedService<NodeStatusBroadcaster>();
builder.Services.AddHostedService<SocketEventHandler>();

await builder.Build().RunAsync();
