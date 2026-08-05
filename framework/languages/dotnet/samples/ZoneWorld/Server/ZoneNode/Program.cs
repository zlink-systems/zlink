using StackExchange.Redis;
using Microsoft.Extensions.Configuration;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;
using ZoneWorld.Server.Configuration;
using ZoneWorld.Server.ZoneNode.Application.Node;
using ZoneWorld.Server.ZoneNode.Application.Zone;
using ZoneWorld.Server.ZoneNode.Infrastructure.Store;
using ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Actors;
using ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Handlers;
using ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Monitoring;
using ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Spots;
using ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Spots.Handlers;
using ZoneWorld.Server.ZoneNode.Ports;
using ZoneWorld.Shared.Contracts;

var configuration = ZoneWorldConfiguration.Load(args);
var shared = configuration.Shared;
var node = configuration.ZoneNode
           ?? throw new InvalidOperationException("ZoneNode configuration is required.");
var nodeId = node.NodeId;

// A node with no zones is the probe of §11.1: it hosts nothing, serves nothing, and is known
// to no other role. All it does is subscribe to the broadcast channel, which is what makes it
// evidence for ZW-D2 — Ops publishes to a node it was never told about.
var hostsZones = !node.SubscriberOnly;

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
builder.Services.AddSingleton(node);
builder.Services.AddSingleton<IConnectionMultiplexer>(
    _ => ConnectionMultiplexer.Connect(shared.RedisEndpoint));
builder.Services.AddSingleton<IMaintenanceStorePort>(services =>
    new MaintenanceStoreRepository(
        services.GetRequiredService<IConnectionMultiplexer>(),
        shared.RedisKeyPrefix));
builder.Services.AddSingleton(new NodeMaintenancePolicy(nodeId));
builder.Services.AddSingleton<NodePlayerCensus>();
builder.Services.AddSingleton<MoveUseCase>();
builder.Services.AddSingleton<PlayerMovement>();
builder.Services.AddSingleton<IOpsReportPort, OpsReportAdapter>();
builder.Services.AddZLinkFramework(options =>
{
    options.DefaultRequestTimeout = TimeSpan.FromSeconds(15);
    var locations = options.ConfigureLocations();
    locations.RouteCacheMaxAge = TimeSpan.Zero;
    locations.MessageFollowDuration = TimeSpan.FromSeconds(5);
    options.AddLocationStore(new ZLinkRedisLocationStore(redis =>
    {
        redis.ConnectionString = shared.RedisEndpoint;
        redis.KeyPrefix = shared.RedisKeyPrefix;
    }));
    options.AddRelocationStore(new ZLinkRedisRelocationStore(redis =>
    {
        redis.ConnectionString = shared.RedisEndpoint;
        redis.KeyPrefix = $"{shared.RedisKeyPrefix}relocation:";
    }));
    options.ConfigureDispatch()
        // Normal records the required key transitions. The runner redirects this process's
        // output to its per-run log file, so the flow evidence remains available without
        // relying on a console scroll.
        .Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
    options.AddHandlersFromAssemblyOf(typeof(ZoneSpot));

    // The node that hosts no zone registers the broadcast subscriber and nothing else — no
    // mesh, no bridge, no channel of its own (§11.1). Anything more would make it a node Ops
    // could have been told about, and ZW-D2 would stop meaning anything.
    if (!hostsZones)
    {
        options.AddFanoutChannel(ZoneWorldNames.BroadcastChannel)
            .Connect(shared.BroadcastEndpoint)
            .AddHandler<BroadcastProbeSubscriber, WorldAnnounceEvent>();
        return;
    }

    // The zone spots and the player actors. A player entering a zone joins the spot named
    // after it, and when that spot is on another node the join causes relocation — which is
    // why the relocation adapter is not optional (§2.6).
    var mesh = options.AddRouteMesh(ZoneWorldNames.MeshName)
        .SetRoutingIdPrefix("zn")
        .Listen(node.MeshEndpoint);
    mesh.Objects().Server()
        .AddEntrySpot<ZoneEntrySpot>()
        .AddActorFactory<PlayerActor, PlayerActorFactory>(
            ZoneWorldNames.PlayerActorType, factory => factory.PreserveStateWith<PlayerActorRelocationAdapter>())
        .AddSpotFactory<ZoneSpot>(
            ZoneWorldNames.ZoneSpotType,
            factory => factory
                // Two Zone Spots per eligible node make the two configured node pairs
                // deterministic while keeping placement inside the public Location Store
                // contract. The process-specific bootstrap requests only its logical pair;
                // the capacity reservation prevents a startup race from placing both pairs
                // on whichever process became ready first.
                .StableTypeLimit(2)
                .DisableRelocation());
    mesh.Channel(ZoneWorldNames.ZoneChannel).Server();

    options.AddFanoutChannel(ZoneWorldNames.BroadcastChannel)
        .Connect(shared.BroadcastEndpoint)
        .AddHandler<WorldAnnounceSubscriber, WorldAnnounceEvent>()
        .AddHandler<NodeMaintenanceChangedSubscriber, NodeMaintenanceChangedEvent>();

    // The report channel carries this node's identity: Ops reads the socket events on its
    // server side and needs to know *which node* connected or went away (§8.1).
    mesh.Channel(ZoneWorldNames.ReportChannel).Client();
});

if (hostsZones)
{
    // Registered after the framework so they start after it: the bootstrap creates spots and
    // actors, and the runtime has to be accepting operations before it can.
    builder.Services.AddHostedService<ZoneNodeBootstrap>();
    builder.Services.AddHostedService<NodeStatusReporter>();
}
var host = builder.Build();

if (!hostsZones)
{
    // The probe node has no spots or actors to bring up, but the runner still waits for the
    // line that says a node has finished starting. It must not appear before the subscriber is
    // running, or an announcement can be published into a node that is not yet listening.
    await host.StartAsync();
    host.Services
        .GetRequiredService<ILoggerFactory>()
        .CreateLogger("ZoneWorld.BroadcastProbe")
        .LogInformation("topology=ready node={NodeId} zones=", nodeId);
    await host.WaitForShutdownAsync();
    return;
}

await host.RunAsync();
