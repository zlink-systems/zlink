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
builder.Services.AddSingleton(TimeProvider.System);
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
    // owner lease는 Location runtime 5절이 정한 기본값을 그대로 쓴다 — TTL 15초, 갱신
    // 5초, renew timeout 3초, fencing margin 5초. ZoneWorld 스펙은 report TTL 15초(2.2)만
    // 정하고 owner lease 재정의를 요구하지 않으므로 어떤 역할도 덮어쓰지 않는다.

    options.ConfigureDispatch()
        .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
    options.AddHandlersFromAssemblyOf(typeof(OpsConsoleSession));

    options.AddStreamNode(ZoneWorldNames.OpsStreamNode)
        .Bind(ops.StreamEndpoint)
        .AddSession<OpsConsoleSession>();

    // The announcement and the maintenance change both leave here without a node list.
    // Adding a node changes nothing on this side — that is the whole point (ZW-D2).
    options.AddFanoutChannel(ZoneWorldNames.BroadcastChannel)
        .EnablePublisher();

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
builder.Services.AddHostedService<NodeRegistrationExpiryService>();

await builder.Build().RunAsync();
