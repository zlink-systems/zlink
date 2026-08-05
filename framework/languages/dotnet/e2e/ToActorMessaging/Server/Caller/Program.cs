using Microsoft.Extensions.Configuration;

using System.Collections.Concurrent;
using Systems.Zlink;
using ToActorMessaging.Caller;
using ToActorMessaging.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Locations.Redis;
using Zlink.Framework.E2E.Configuration;

var options = ServerOptions.Parse(args, "caller");
Directory.CreateDirectory(options.LogDir);

var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
builder.WebHost.UseUrls(options.HttpUrl);
var cachedActors = new ConcurrentDictionary<string, ActorRef>(StringComparer.Ordinal);
IZLinkMeshPeerConnections? actorConnections = null;
builder.Services.AddZLinkFramework(framework =>
{
    //  This E2E host is not started inside a memory-limited
    //  container. Supply a deterministic finite limit so the
    //  default Auto HWM contract does not depend on the host.
    framework.ConfigureInboundDispatch().ProcessMemoryLimitBytes =
        1UL * 1024 * 1024 * 1024;
    framework.AddLocationStore(new ZLinkRedisLocationStore(redis => { redis.ConnectionString = options.RedisEndpoint; redis.KeyPrefix = options.RedisKeyPrefix; }));
    var spot = framework.AddRouteMesh("to-actor")
        .Listen(options.RouterEndpoint)
        .SetRoutingIdPrefix(options.Rid);
    // The caller finds and messages Actors, so it declares the Object Client
    // role that surfaces IZLinkActorManager; spec 13 §3.3 then rules out a
    // fixed RID for this MeshNode.
    spot.Objects().Client();
    spot.Channel("to-actor").Client();
    // An Object Client cannot combine a manual topology with the automatic
    // RID its role requires: spec 13 §3.3 keeps a fixed RID out of an Object
    // role, and a routing ID prefix is only valid with automatic discovery.
    // So this caller discovers actor nodes through the Location Store.
    _ = options.ConnectActorRoutes;
    actorConnections = spot.PeerConnections;
});

var app = builder.Build();
app.MapGet("/health", () => Results.Ok(new { status = "ok" }));
app.MapPost("/send", async (
    ActorCallRequest request,
    IZLinkActorManager actorDirectory,
    IZLinkActorClient actors,
    CancellationToken ct) =>
{
    try
    {
        var actor = await ResolveActorAsync(request, actorDirectory, ct);
        await actors.SendToActor(actor.ActorId, new ActorNotify(request.Scenario, request.ActorId, request.Value))
            .Async(ct);
        return Results.Ok(new ActorCallResponse(request.Scenario, request.ActorId, "sent"));
    }
    catch (ZLinkFrameworkException error)
    {
        return Results.Ok(new ActorCallResponse(request.Scenario, request.ActorId, "failed", error.Kind.ToString()));
    }
});
app.MapPost("/request", async (
    ActorCallRequest request,
    IZLinkActorManager actorDirectory,
    IZLinkActorClient actors,
    CancellationToken ct) =>
{
    try
    {
        var actor = await ResolveActorAsync(request, actorDirectory, ct);
        var reply = await actors.RequestToActor(
                actor.ActorId, new ActorAsk(request.Scenario, request.ActorId, request.Value))
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<ActorReply>(ct);
        return Results.Ok(new ActorCallResponse(request.Scenario, request.ActorId, reply.Value));
    }
    catch (ZLinkFrameworkException error)
    {
        return Results.Ok(new ActorCallResponse(request.Scenario, request.ActorId, "failed", error.Kind.ToString()));
    }
});
app.MapPost("/refs/{actorId}/capture", async (
    string actorId,
    IZLinkActorManager actorDirectory,
    CancellationToken ct) =>
{
    var actor = await actorDirectory.FindAsync(actorId, ct)
                ?? throw new InvalidOperationException(
                    $"Actor route '{actorId}' was not found.");
    cachedActors[actorId] = actor;
    return Results.Ok(actor);
});
app.MapGet("/directory/{actorId}", async (
    string actorId,
    IZLinkActorManager actorDirectory,
    CancellationToken ct) =>
{
    var actor = await actorDirectory.FindAsync(actorId, ct);
    return Results.Ok(new ActorRouteStatus(actorId, actor is not null));
});
app.MapPost("/cached/request", async (
    ActorCallRequest request,
    IZLinkActorClient actors,
    CancellationToken ct) =>
{
    try
    {
        if (!cachedActors.TryGetValue(request.ActorId, out var actor))
            throw new InvalidOperationException($"Actor ref '{request.ActorId}' was not captured.");
        var reply = await actors.RequestToActor(
                actor.ActorId, new ActorAsk(request.Scenario, request.ActorId, request.Value))
            .Timeout(TimeSpan.FromSeconds(2))
            .Async<ActorReply>(ct);
        return Results.Ok(new ActorCallResponse(request.Scenario, request.ActorId, reply.Value));
    }
    catch (ZLinkFrameworkException error)
    {
        return Results.Ok(new ActorCallResponse(request.Scenario, request.ActorId, "failed", error.Kind.ToString()));
    }
});
app.MapPost("/route/disconnect", () =>
{
    var connections = actorConnections
                      ?? throw new InvalidOperationException("Actor router connections are unavailable.");
    connections.Disconnect(options.ActorRouterEndpoint);
    connections.Disconnect(options.ActorBRouterEndpoint);
    return Results.Ok(new { status = "disconnected" });
});
app.MapPost("/route/reconnect", () =>
{
    var connections = actorConnections
                      ?? throw new InvalidOperationException("Actor router connections are unavailable.");
    connections.Connect(options.ActorRouterEndpoint);
    connections.Connect(options.ActorBRouterEndpoint);
    return Results.Ok(new { status = "connected" });
});
app.MapPost("/shutdown", async (IHostApplicationLifetime lifetime) =>
{
    await Task.Yield();
    lifetime.StopApplication();
    return Results.Ok();
});
await app.RunAsync();

static async ValueTask<ActorRef> ResolveActorAsync(
    ActorCallRequest request,
    IZLinkActorManager actorDirectory,
    CancellationToken cancellationToken)
{
    if (!string.IsNullOrWhiteSpace(request.TargetNodeRid)
        && request.TargetGeneration is { } generation)
        return new ActorRef(
            request.ActorId,
            generation,
            "to-actor",
            RoutingId.From(request.TargetNodeRid));

    return await actorDirectory.FindAsync(request.ActorId, cancellationToken)
           ?? throw new InvalidOperationException(
               $"Actor route '{request.ActorId}' was not found.");
}

namespace ToActorMessaging.Caller
{
    internal sealed record ServerOptions(
        string Rid,
        string HttpUrl,
        string RedisEndpoint,
        string RedisKeyPrefix,
        string RouterEndpoint,
        string PubSubEndpoint,
        string ActorRid,
        string ActorRouterEndpoint,
        string ActorBRid,
        string ActorBRouterEndpoint,
        string LogDir,
        bool ConnectActorRoutes)
    {
        public static ServerOptions Parse(string[] args, string role)
            => E2eConfiguration.Load<ServerOptions>(args);
    }
}
