using System.Diagnostics;
using ChannelEgressRouting.Server;
using ChannelEgressRouting.Shared;
using Microsoft.Extensions.Configuration;
using Systems.Zlink;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Locations.Redis;

var options = RoleOptions.Parse(args);
var builder = WebApplication.CreateBuilder(args);
builder.Configuration.Sources.Clear();
builder.Configuration.AddInMemoryCollection();
builder.Logging.ClearProviders();
builder.Logging.AddSimpleConsole(console =>
{
    console.SingleLine = true;
    console.TimestampFormat = "HH:mm:ss.fff ";
});
builder.WebHost.UseUrls(options.HttpUrl);
builder.Services.AddSingleton(options);
builder.Services.AddSingleton<EvidenceStore>();
builder.Services.AddZLinkFramework(framework =>
{
    //  This E2E host is not started inside a memory-limited
    //  container. Supply a deterministic finite limit so the
    //  default Auto HWM contract does not depend on the host.
    framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
        1UL * 1024 * 1024 * 1024;
    framework.AddLocationStore(new ZLinkRedisLocationStore(redis =>
    {
        redis.ConnectionString = options.RedisEndpoint;
        redis.KeyPrefix = options.RedisKeyPrefix;
    }));
    if (options.Role is "session" or "play")
    {
        framework.AddRelocationStore(new ZLinkRedisRelocationStore(redis =>
        {
            redis.ConnectionString = options.RedisEndpoint;
            redis.KeyPrefix = $"{options.RedisKeyPrefix}:relocation";
        }));
    }
    framework.AddHandlersFromAssemblyOf<ChannelEntrySpot>();
    if (options.Role == "session")
    {
        if (string.IsNullOrWhiteSpace(options.StreamEndpoint))
            throw new InvalidOperationException(
                "Session StreamEndpoint is required.");
        framework.AddStreamNode("config12-session")
            .Bind(options.StreamEndpoint)
            .EnableActorDispatch()
            .AddSession<ChannelSession>();
    }
    var locations = framework.ConfigureLocations();
    locations.PollingInterval = TimeSpan.FromMilliseconds(100);
    locations.OwnerLeaseRenewInterval = TimeSpan.FromSeconds(1);
    locations.OwnerLeaseRenewTimeout = TimeSpan.FromSeconds(1);
    locations.OwnerLeaseTtl = TimeSpan.FromSeconds(10);
    locations.OwnerLeaseFencingMargin = TimeSpan.FromSeconds(2);

    RegisterMesh(
        framework,
        ChannelEgressNames.GameMesh,
        options.Rid,
        (options.RouteServers ?? []).Where(static name => name.StartsWith("game.", StringComparison.Ordinal)),
        (options.RouteClients ?? []).Where(static name => name.StartsWith("game.", StringComparison.Ordinal)),
        options);
    RegisterMesh(
        framework,
        ChannelEgressNames.AuditMesh,
        $"{options.Rid}-audit",
        (options.RouteServers ?? []).Where(static name => name.StartsWith("audit.", StringComparison.Ordinal)),
        (options.RouteClients ?? []).Where(static name => name.StartsWith("audit.", StringComparison.Ordinal)),
        options);

    if (options.WorkflowClient || options.WorkflowServer)
    {
        var roles = framework.AddClientServerChannel(ChannelEgressNames.Workflow);
        if (options.WorkflowClient)
            roles.Client();
        if (options.WorkflowServer)
        {
            var server = roles.Server();
            if (string.IsNullOrWhiteSpace(options.WorkflowEndpoint))
                server.Listen();
            else
                server.Listen(new Uri(options.WorkflowEndpoint).Port);
            server
                .SetBindHost("127.0.0.1")
                .SetAdvertiseHost("127.0.0.1")
                .SetWeight(options.WorkflowWeight)
                .AddRequestHandler<ChannelProbeRequestHandler, ChannelProbeRequest, ChannelProbeReply>()
                .AddSendHandler<ChannelProbeCommandHandler, ChannelProbeCommand>();
        }

        if (options.InvalidMode == "duplicate-workflow-client")
            roles.Client();
    }

    if (options.InvalidMode == "route-clientserver-conflict")
        framework.AddClientServerChannel(
                (options.RouteServers ?? []).Concat(options.RouteClients ?? []).First())
            .Client();

    if (options.Role == "audit")
        framework.AddFanoutChannel("config12.fanout")
            .EnablePublisher()
            .SetBindHost("127.0.0.1")
            .SetAdvertiseHost("127.0.0.1")
            .SetRoutingIdPrefix("config12-audit-fanout");
    if (options.Role == "play")
        framework.AddFanoutChannel("config12.fanout")
            .EnableSubscriber()
            .AddHandler<FanoutProbeHandler, FanoutProbeEvent>();
});

var app = builder.Build();
app.MapGet("/health", () => Results.Ok(new { status = "ready", options.Role }));
app.MapGet("/evidence", (EvidenceStore evidence) =>
    Results.Ok(evidence.Snapshot()));
app.MapGet("/topology/{mesh}", (
    string mesh,
    Zlink.Framework.Contracts.Configuration.IZLinkRouteMeshRuntime runtime) =>
{
    var snapshot = runtime.GetStatus(mesh);
    return Results.Ok(new
    {
        state = snapshot.State.ToString(),
        snapshot.IsReady,
        snapshot.ReadyPeerCount,
        peers = snapshot.Peers.Select(peer => new
        {
            rid = peer.NodeRid.ToString(),
            state = peer.State.ToString()
        }),
        channels = snapshot.Channels.Select(channel => new
        {
            channel.ChannelName,
            channel.IsReady,
            channel.ReadyTargetCount
        })
    });
});
app.MapGet("/client-server/{channel}", (
    string channel,
    IZLinkClientServerRuntime runtime) =>
{
    var snapshot = runtime.GetStatus(channel);
    return Results.Ok(new
    {
        state = snapshot.State.ToString(),
        snapshot.IsReady,
        snapshot.ReadyTargetCount,
        localRole = snapshot.LocalRole.ToString(),
        targets = snapshot.Targets.Select(target => new
        {
            rid = target.NodeRid.ToString(),
            target.Weight,
            state = target.State.ToString()
        })
    });
});
app.MapPost("/client-server/{channel}/weight/{weight:int}", (
    string channel,
    int weight,
    IZLinkRouteMeshRuntimeOptions runtime) =>
{
    runtime.Channel(channel).Weight = weight;
    return Results.Ok(new { channel, weight });
});
app.MapPost("/request", async (
    RouteInvokeRequest request,
    IZLinkRouteClient routes,
    CancellationToken cancellationToken) =>
{
    var timer = Stopwatch.StartNew();
    try
    {
        var reply = await routes
            .RequestToChannel(
                request.Channel,
                new ChannelProbeRequest(request.Id, request.Mode))
            .Timeout(TimeSpan.FromSeconds(1))
            .Async<ChannelProbeReply>(cancellationToken);
        return Results.Ok(new RouteInvokeResult(
            true, null, reply, timer.ElapsedMilliseconds));
    }
    catch (ZLinkFrameworkException error)
    {
        return Results.Ok(new RouteInvokeResult(
            false, error.Kind.ToString(), null, timer.ElapsedMilliseconds));
    }
    catch (Exception error) when (error is TimeoutException or OperationCanceledException)
    {
        return Results.Ok(new RouteInvokeResult(
            false, error.GetType().Name, null, timer.ElapsedMilliseconds));
    }
});
app.MapPost("/send", async (
    RouteInvokeRequest request,
    IZLinkRouteClient routes,
    CancellationToken cancellationToken) =>
{
    var timer = Stopwatch.StartNew();
    try
    {
        await routes.SendToChannel(
                request.Channel,
                new ChannelProbeCommand(request.Id))
            .Async(cancellationToken);
        return Results.Ok(new SendInvokeResult(
            true, null, timer.ElapsedMilliseconds));
    }
    catch (ZLinkFrameworkException error)
    {
        return Results.Ok(new SendInvokeResult(
            false, error.Kind.ToString(), timer.ElapsedMilliseconds));
    }
});
app.MapPost("/fanout/{id}", async (
    string id,
    IZLinkFanoutClient fanout,
    CancellationToken cancellationToken) =>
{
    await fanout.Publish("config12.fanout", new FanoutProbeEvent(id))
        .Async(cancellationToken);
    return Results.Ok();
});
app.MapPost("/logical/{id}", async (
    string id,
    IZLinkSpotPublisherClient publisher,
    CancellationToken cancellationToken) =>
{
    await publisher
        .Publish(
            ChannelEgressNames.Play,
            "config12.logical",
            new LogicalMulticastProbeEvent(id))
        .Async(cancellationToken);
    return Results.Ok();
});
app.MapGet("/fanout-status", (IZLinkFanoutRuntime runtime) =>
{
    var snapshot = runtime.GetStatus("config12.fanout");
    return Results.Ok(new
    {
        state = snapshot.State.ToString(),
        snapshot.IsReady,
        snapshot.ReadyPublisherCount
    });
});
app.MapGet("/locations", async (
    IZLinkLocationRuntimeQuery query,
    CancellationToken cancellationToken) =>
{
    var rows = await query.ListTopologyAsync(
        new ZLinkLocationTopologyFilter(),
        cancellationToken: cancellationToken);
    return Results.Ok(rows.Items.Select(row => new
    {
        row.MeshName,
        rid = row.NodeRid.ToString(),
        row.Endpoint,
        state = row.State.ToString()
    }));
});
if (options.Role is "session" or "play" || options.WorkflowServer)
{
app.MapPost("/objects/actors", async (
    ChannelActorCreateRequest request,
    IZLinkActorManager actors,
    CancellationToken cancellationToken) =>
{
    var result = await actors
        .GetOrCreate(request.ActorId, ChannelObjectNames.ActorType)
        .InMesh(ChannelEgressNames.GameMesh)
        .Request(request)
        .Async(cancellationToken);
    var actor = result switch
    {
        ZLinkActorCreateResult.Existing existing => existing.Actor,
        ZLinkActorCreateResult.Created created => created.Actor,
        _ => throw new InvalidOperationException("Actor creation was rejected.")
    };
    return Results.Ok(new ChannelActorCreateReply(
        actor.ActorId,
        actor.NodeRid.ToString(),
        actor.ObjectGeneration));
});
app.MapPost("/objects/spots", async (
    ChannelSpotCreateRequest request,
    IZLinkSpotManager spots,
    CancellationToken cancellationToken) =>
{
    var result = await spots
        .GetOrCreate(request.SpotId, ChannelObjectNames.SpotType)
        .InMesh(ChannelEgressNames.GameMesh)
        .Request(request)
        .Async(cancellationToken);
    if (result.State == ZLinkSpotCreateState.Rejected)
        throw new InvalidOperationException("Spot creation was rejected.");
    return Results.Ok(new ChannelSpotCreateReply(
        result.Spot.SpotId,
        result.Spot.NodeRid.ToString(),
        result.Spot.ObjectGeneration));
});
app.MapPost("/objects/actors/{actorId}/probe", async (
    string actorId,
    ChannelObjectProbeRequest request,
    IZLinkActorClient actors,
    CancellationToken cancellationToken) =>
    Results.Ok(await actors
        .RequestToActor(actorId, request)
        .Timeout(TimeSpan.FromSeconds(5))
        .Async<ChannelObjectProbeReply>(cancellationToken)));
app.MapPost("/objects/spots/{spotId}/probe", async (
    string spotId,
    ChannelObjectProbeRequest request,
    IZLinkSpotClient spots,
    CancellationToken cancellationToken) =>
    Results.Ok(await spots
        .RequestToSpot(spotId, request)
        .Timeout(TimeSpan.FromSeconds(5))
        .Async<ChannelObjectProbeReply>(cancellationToken)));
app.MapPost("/objects/actors/{actorId}/workflow", async (
    string actorId,
    ChannelSpotWorkflowRequest request,
    IZLinkActorClient actors,
    CancellationToken cancellationToken) =>
    Results.Ok(await actors
        .RequestToActor(actorId, request)
        .Timeout(TimeSpan.FromSeconds(5))
        .Async<ChannelSpotWorkflowReply>(cancellationToken)));
app.MapPost("/objects/actors/{actorId}/join", async (
    string actorId,
    ChannelActorJoinRequest request,
    IZLinkActorClient actors,
    CancellationToken cancellationToken) =>
    Results.Ok(await actors
        .RequestToActor(actorId, request)
        .Timeout(TimeSpan.FromSeconds(5))
        .Async<ChannelActorJoinReply>(cancellationToken)));
app.MapPost("/objects/actors/{actorId}/bound-push", async (
    string actorId,
    ChannelBoundPushRequest request,
    IZLinkActorClient actors,
    CancellationToken cancellationToken) =>
    Results.Ok(await actors
        .RequestToActor(actorId, request)
        .Timeout(TimeSpan.FromSeconds(5))
        .Async<ChannelBoundPushReply>(cancellationToken)));
app.MapPost("/objects/state-address", async (
    ChannelObjectScenarioRequest request,
    IZLinkRouteClient routes,
    CancellationToken cancellationToken) =>
{
    var reply = await routes
        .RequestToChannel(
            ChannelEgressNames.Workflow,
            new ChannelProbeRequest(
                request.Id,
                $"state-address:{request.ActorId}:{request.SpotId}"))
        .Timeout(TimeSpan.FromSeconds(5))
        .Async<ChannelProbeReply>(cancellationToken);
    return Results.Ok(reply);
});
app.MapPost("/objects/nodes/{nodeRid}/probe", async (
    string nodeRid,
    ChannelObjectProbeRequest request,
    IZLinkRouteClient routes,
    CancellationToken cancellationToken) =>
    Results.Ok(await routes
        .RequestToNode(
            ChannelEgressNames.GameMesh,
            RoutingId.From(nodeRid),
            request)
        .Timeout(TimeSpan.FromSeconds(5))
        .Async<ChannelProbeReply>(cancellationToken)));
}
app.MapPost("/shutdown", (
    IHostApplicationLifetime lifetime) =>
{
    lifetime.StopApplication();
    return Results.Ok(new { status = "stopping" });
});
await app.RunAsync();

static void RegisterMesh(
    IZLinkFrameworkOptions framework,
    string meshName,
    string ridPrefix,
    IEnumerable<string> serverChannels,
    IEnumerable<string> clientChannels,
    RoleOptions options)
{
    var servers = serverChannels.Distinct(StringComparer.Ordinal).ToArray();
    var clients = clientChannels.Distinct(StringComparer.Ordinal).ToArray();
    var objectServer = meshName == ChannelEgressNames.GameMesh
                       && options.Role is "session" or "play";
    var objectClient = meshName == ChannelEgressNames.GameMesh
                       && options.WorkflowServer;
    if (servers.Length == 0 && clients.Length == 0
        && !objectServer && !objectClient)
        return;

    var mesh = framework.AddRouteMesh(meshName);
    if (string.IsNullOrWhiteSpace(options.RouteEndpoint))
        mesh.Listen();
    else
        mesh.Listen(options.RouteEndpoint);
    mesh
        .SetBindHost("127.0.0.1")
        .SetAdvertiseHost(options.RouteAdvertiseHost ?? "127.0.0.1")
        .SetRoutingIdPrefix(ridPrefix);
    if (objectServer)
    {
        mesh.AddRouteRequestHandler<
            ChannelNodeProbeHandler,
            ChannelObjectProbeRequest,
            ChannelProbeReply>();
    }
    if (objectServer)
    {
        mesh.SetPlacementWeight(100)
            .SetActorLimit(128)
            .SetSpotLimit(128)
            .SetActivationConcurrency(32)
            .Objects().Server()
            .AddEntrySpot<ChannelEntrySpot>()
            .AddActorFactory<ChannelActor, ChannelActorFactory>(
                ChannelObjectNames.ActorType, factory => factory.PreserveStateWith<ChannelActorRelocationAdapter>())
            .AddSpotFactory<ChannelRoomSpot>(
                ChannelObjectNames.SpotType,
                factory => factory
                    .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                    .PreserveStateWith<ChannelRoomRelocationAdapter>());
    }
    else if (objectClient)
    {
        mesh.Objects().Client();
    }
    foreach (var channelName in servers)
    {
        mesh.Channel(channelName).Server()
            .AddRequestHandler<ChannelProbeRequestHandler, ChannelProbeRequest, ChannelProbeReply>()
            .AddSendHandler<ChannelProbeCommandHandler, ChannelProbeCommand>();
    }

    foreach (var channelName in clients)
        mesh.Channel(channelName).Client();
}
