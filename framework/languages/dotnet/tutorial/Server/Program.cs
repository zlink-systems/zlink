using System.Text;
using Systems.Zlink;
using Tutorial.Server.Actors;
using Tutorial.Server.Channel;
using Tutorial.Server.Dispatch;
using Tutorial.Server.Ops;
using Tutorial.Server.Sessions;
using Tutorial.Server.Spots;
using Tutorial.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;

var builder = WebApplication.CreateBuilder(args);

// This process exposes no HTTP of its own. The port only has to differ from the
// gateway's when both run on the same host.
builder.WebHost.UseUrls("http://127.0.0.1:5081");

builder.Services.AddZLinkFramework(options =>
{
    // --8<-- [start:location-store]
    // Rooms and players are addressed by id, not by host, so their current
    // location is kept here. Every node reads and writes the same store under
    // the same prefix.
    options.AddLocationStore(
        new ZLinkRedisLocationStore(redis =>
        {
            redis.ConnectionString = "127.0.0.1:6379";
            redis.KeyPrefix = "zlink-tutorial";
        })
    );
    // --8<-- [end:location-store]

    // --8<-- [start:relocation-store]
    // Registering any instance-spot factory requires this store, even with
    // relocation turned off: the registration itself is the condition.
    options.AddRelocationStore(
        new ZLinkRedisRelocationStore(redis =>
        {
            redis.ConnectionString = "127.0.0.1:6379";
            redis.KeyPrefix = "zlink-tutorial";
        })
    );
    // --8<-- [end:relocation-store]

    // --8<-- [start:filter-register]
    // Registration order is execution order. Filters wrap handlers this node
    // receives; Spot and Actor handlers are not covered.
    options.UseFilter<CallLogFilter>();
    // --8<-- [end:filter-register]

    // --8<-- [start:mesh-register]
    // Both sides must name the mesh identically, or they never see each other
    // as peers. The routing id names this node; without it the Framework assigns
    // a generated one, which a caller cannot type into a URL.
    var mesh = options
        .AddRouteMesh("game")
        .Listen("tcp://127.0.0.1:7201")
        .SetRoutingId(RoutingId.From("game-server-1"));
    // --8<-- [end:mesh-register]

    // --8<-- [start:channel-register]
    // Only handlers exposed here can be called by other nodes. A handler class
    // sitting in the same assembly but left out stays unreachable.
    mesh.Channel("profile")
        .Server()
        .AddRequestHandler<GetPlayerProfileHandler, GetPlayerProfile, PlayerProfile>()
        .AddSendHandler<RecordLoginHandler, RecordLogin>();
    // --8<-- [end:channel-register]

    // --8<-- [start:node-direct-register]
    // Registered on the mesh itself, with no Channel(...) call. Handlers added
    // this way are reached by routing id instead of by channel name.
    mesh.AddRouteRequestHandler<NodeStatusHandler, GetNodeStatus, NodeStatus>();
    // --8<-- [end:node-direct-register]

    // --8<-- [start:clientserver-register]
    // The caller dials this endpoint directly, so it needs a port of its own and
    // an address to advertise, separate from the mesh.
    options
        .AddClientServerChannel("ticketing")
        .Server()
        .Listen(7211)
        .SetBindHost("127.0.0.1")
        .SetAdvertiseHost("127.0.0.1")
        .AddRequestHandler<IssueSessionTicketHandler, IssueSessionTicket, SessionTicket>();
    // --8<-- [end:clientserver-register]

    // --8<-- [start:fanout-subscribe]
    // No publisher endpoint is given: the location store supplies it. Adding a
    // manual Connect alongside it is rejected at startup.
    options
        .AddFanoutChannel("broadcast")
        .EnableSubscriber()
        .Subscribe(nameof(MaintenanceNotice))
        .AddHandler<MaintenanceNoticeSubscriber, MaintenanceNotice>();
    // --8<-- [end:fanout-subscribe]

    // --8<-- [start:object-server]
    // A mesh node picks this role once. Keep the builder and reuse it, because
    // calling Objects().Server() a second time is rejected at startup.
    var objects = mesh.Objects().Server();
    // --8<-- [end:object-server]

    // --8<-- [start:spot-register]
    // "game-room" is the stable type a caller names when opening a room. Any
    // node that registers it is a candidate to host one.
    objects.AddSpotFactory<GameRoom>(
        "game-room",
        // Exactly one relocation policy is required. Moving a live room to
        // another node is a separate topic.
        factory => factory.DisableRelocation()
    );
    // --8<-- [end:spot-register]

    // --8<-- [start:instance-spot-register]
    // Registered the same way, but callers never create one explicitly.
    objects.AddInstanceSpotFactory<MatchQueue>(
        "match-queue",
        factory => factory.DisableRelocation()
    );
    // --8<-- [end:instance-spot-register]

    // --8<-- [start:actor-register]
    // One lobby per object server. Newly created players start there.
    objects.AddEntrySpot<LobbySpot>();

    // Nodes that register "player" are candidates to host one.
    objects.AddActorFactory<Player, PlayerFactory>(
        "player",
        factory => factory.DisableRelocation()
    );
    // --8<-- [end:actor-register]

    // --8<-- [start:stream-register]
    // The port game clients connect to. One session type per stream node, and
    // actor dispatch must be on for a session to relay to its player.
    options
        .AddStreamNode("client-stream")
        .Bind("tcp://127.0.0.1:7301")
        .EnableActorDispatch()
        .AddSession<GameSession>();
    // --8<-- [end:stream-register]
});

var app = builder.Build();

app.MapGet(
    "/fanout/broadcast/ready",
    (IZLinkFanoutRuntime fanout) =>
        fanout.GetStatus("broadcast").IsReady
            ? Results.Ok()
            : Results.StatusCode(StatusCodes.Status503ServiceUnavailable)
);

// Tutorial credentials stay in code because this standalone sample deliberately
// has no configuration file; production admin credentials belong in configuration.
const string tutorialAdminUser = "ops";
const string tutorialAdminPassword = "tutorial-admin";
const string tutorialAdminChallenge = "Basic realm=\"tutorial-admin\"";

app.Use(
    async (context, next) =>
    {
        if (!context.Request.Path.StartsWithSegments("/admin"))
        {
            await next(context);
            return;
        }

        var authorization = context.Request.Headers.Authorization.ToString();
        var authorized = false;
        if (authorization.StartsWith("Basic ", StringComparison.OrdinalIgnoreCase))
        {
            try
            {
                var encoded = authorization["Basic ".Length..].Trim();
                var credentials = Encoding.UTF8.GetString(Convert.FromBase64String(encoded));
                var separator = credentials.IndexOf(':');
                authorized =
                    separator >= 0
                    && string.Equals(
                        credentials[..separator],
                        tutorialAdminUser,
                        StringComparison.Ordinal
                    )
                    && string.Equals(
                        credentials[(separator + 1)..],
                        tutorialAdminPassword,
                        StringComparison.Ordinal
                    );
            }
            catch (FormatException)
            {
                authorized = false;
            }
        }

        if (!authorized)
        {
            context.Response.StatusCode = StatusCodes.Status401Unauthorized;
            context.Response.Headers.WWWAuthenticate = tutorialAdminChallenge;
            return;
        }

        await next(context);
    }
);

// --8<-- [start:weight-runtime]
// Weight is the one value this node can change while running. 0 keeps the
// socket open and finishes in-flight work, but other nodes stop choosing this
// one for new calls. 100 is the normal value.
app.MapPost(
    "/admin/channels/{channel}/weight",
    (string channel, int value, IZLinkRouteMeshRuntimeOptions mesh) =>
    {
        mesh.Channel(channel).Weight = value;
        return Results.Ok(new { channel, weight = value });
    }
);

// --8<-- [end:weight-runtime]

await app.RunAsync();
