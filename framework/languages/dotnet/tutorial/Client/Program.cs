using System.IO.Compression;
using System.Text;
using System.Text.Json;
using Systems.Zlink;
using Tutorial.Client;
using Tutorial.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Locations.Redis;

var jsonOptions = new JsonSerializerOptions(JsonSerializerDefaults.Web);
var builder = WebApplication.CreateBuilder(args);

// The HTTP surface the examples below are driven through.
builder.WebHost.UseUrls("http://127.0.0.1:5080");

builder.Services.AddZLinkFramework(options =>
{
    // --8<-- [start:location-store-client]
    // Rooms and players are looked up by whoever calls them, so a node that
    // hosts none still needs the store, pointed at the same prefix.
    options.AddLocationStore(
        new ZLinkRedisLocationStore(redis =>
        {
            redis.ConnectionString = "127.0.0.1:6379";
            redis.KeyPrefix = "zlink-tutorial";
        })
    );
    // --8<-- [end:location-store-client]

    // --8<-- [start:channel-client-register]
    // This node opens an endpoint too. Both sides listen to become peers.
    var mesh = options.AddRouteMesh("game").Listen("tcp://0.0.0.0:7202");

    // Client() means this node exposes no handler for the channel; it only calls.
    mesh.Channel("profile").Client();

    // A mesh peer connection, not a channel one. The mesh picks a node that
    // serves the channel from among the peers it learns this way, so a channel
    // call never names a node. Naming the expected routing id is optional: it
    // fences the connection to one node, and the handshake rejects a peer that
    // answers with a different id.
    mesh.PeerConnections.Connect(RoutingId.From("game-server-1"), "tcp://127.0.0.1:7201");
    // --8<-- [end:channel-client-register]

    // --8<-- [start:clientserver-client-register]
    // Here the caller decides who answers: the server it dialed. Mesh peers play
    // no part in the choice.
    options.AddClientServerChannel("ticketing").Client().Connect("tcp://127.0.0.1:7211");
    // --8<-- [end:clientserver-client-register]

    // --8<-- [start:fanout-publish-register]
    // The publisher keeps no subscriber list. Subscribers may come and go with
    // no change here.
    options.AddFanoutChannel("broadcast").EnablePublisher("tcp://127.0.0.1:7212");
    // --8<-- [end:fanout-publish-register]

    // --8<-- [start:spot-client-register]
    // Client() rules out registering factories. This node creates and calls
    // rooms and players; a node that picked Server() runs them.
    mesh.Objects().Client();
    // --8<-- [end:spot-client-register]
});

var app = builder.Build();

// Maps a failed framework call to the status code that says what happened.
// Without it every failure below reaches the host's default handler as a 500.
app.UseZLinkErrorResponse();

app.Use(
    async (context, next) =>
    {
        var segments = context.Request.Path.Value?.Split(
            '/',
            StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries
        );
        var isRoomState =
            segments is { Length: 2 }
            && string.Equals(segments[0], "rooms", StringComparison.OrdinalIgnoreCase);
        var acceptsGzip =
            context.Request.Headers.TryGetValue("Accept-Encoding", out var acceptEncoding)
            && acceptEncoding.Any(value =>
                value?.Contains("gzip", StringComparison.OrdinalIgnoreCase) == true
            );

        if (!isRoomState || !acceptsGzip)
        {
            await next(context);
            return;
        }

        context.Response.Headers.ContentEncoding = "gzip";
        context.Response.ContentLength = null;
        var originalBody = context.Response.Body;
        await using (
            var gzip = new GZipStream(originalBody, CompressionLevel.Fastest, leaveOpen: true)
        )
        {
            context.Response.Body = gzip;
            try
            {
                await next(context);
            }
            finally
            {
                context.Response.Body = originalBody;
            }
        }
    }
);

app.MapGet(
    "/player/{playerId}",
    (string playerId, HttpResponse response) =>
    {
        response.StatusCode = StatusCodes.Status301MovedPermanently;
        response.Headers.Location = $"/players/{playerId}";
        return Results.StatusCode(StatusCodes.Status301MovedPermanently);
    }
);

app.MapGet(
    "/rooms/{roomId}/export",
    async (
        string roomId,
        IZLinkSpotClient rooms,
        HttpResponse response,
        CancellationToken cancellationToken
    ) =>
    {
        var state = await rooms
            .RequestToSpot(roomId, new GetRoomState())
            .Timeout(TimeSpan.FromSeconds(3))
            .Async<RoomState>(cancellationToken);

        response.ContentType = "application/x-ndjson";
        response.ContentLength = null;
        await response.BodyWriter.WriteAsync(
            Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new { roomId }) + "\n"),
            cancellationToken
        );
        await response.BodyWriter.FlushAsync(cancellationToken);

        foreach (var message in state.Chat)
        {
            await response.BodyWriter.WriteAsync(
                Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new { message }) + "\n"),
                cancellationToken
            );
            await response.BodyWriter.FlushAsync(cancellationToken);
        }
    }
);

app.MapPost(
    "/rooms/{roomId}/import",
    async (
        string roomId,
        HttpRequest request,
        IZLinkSpotClient rooms,
        CancellationToken cancellationToken
    ) =>
    {
        if (
            !request.ContentType?.StartsWith(
                "application/x-ndjson",
                StringComparison.OrdinalIgnoreCase
            )
            ?? true
        )
            return Results.BadRequest();

        using var reader = new StreamReader(
            request.Body,
            Encoding.UTF8,
            detectEncodingFromByteOrderMarks: true
        );
        var imported = 0;
        while (await reader.ReadLineAsync(cancellationToken) is { } line)
        {
            if (string.IsNullOrWhiteSpace(line))
                continue;

            PostChat? message;
            try
            {
                message = JsonSerializer.Deserialize<PostChat>(line, jsonOptions);
            }
            catch (JsonException)
            {
                return Results.BadRequest();
            }

            if (message is null)
                return Results.BadRequest();

            await rooms.SendToSpot(roomId, message).Async(cancellationToken);
            imported++;
        }

        return Results.Ok(new ImportedResponse(imported));
    }
);

// --8<-- [start:channel-request-call]
app.MapGet(
    "/players/{playerId}/profile",
    async (string playerId, IZLinkRouteClient route, CancellationToken cancellationToken) =>
    {
        // The target is a channel name. Which node answers is decided at call time.
        var profile = await route
            .RequestToChannel("profile", new GetPlayerProfile(playerId))
            .Async<PlayerProfile>(cancellationToken);

        return Results.Ok(profile);
    }
);

// --8<-- [end:channel-request-call]

// --8<-- [start:channel-send-call]
app.MapPost(
    "/players/{playerId}/logins",
    async (string playerId, IZLinkRouteClient route, CancellationToken cancellationToken) =>
    {
        // Returns as soon as the message is sent, with no reply to wait for.
        await route.SendToChannel("profile", new RecordLogin(playerId)).Async(cancellationToken);

        return Results.Accepted();
    }
);

// --8<-- [end:channel-send-call]

// --8<-- [start:node-direct-call]
app.MapGet(
    "/ops/nodes/{nodeRid}/status",
    async (string nodeRid, IZLinkRouteClient route, CancellationToken cancellationToken) =>
    {
        // The target is one node, named by its routing id. No channel takes part,
        // so no candidate is chosen: this node answers or the call fails.
        var status = await route
            .RequestToNode("game", RoutingId.From(nodeRid), new GetNodeStatus())
            .Async<NodeStatus>(cancellationToken);

        return Results.Ok(status);
    }
);

// --8<-- [end:node-direct-call]

// --8<-- [start:clientserver-call]
app.MapPost(
    "/players/{playerId}/tickets",
    async (string playerId, IZLinkRouteClient route, CancellationToken cancellationToken) =>
    {
        // Same call shape as a mesh channel; only the routing differs.
        var ticket = await route
            .RequestToChannel("ticketing", new IssueSessionTicket(playerId))
            .Async<SessionTicket>(cancellationToken);

        return Results.Ok(ticket.Value);
    }
);

// --8<-- [end:clientserver-call]

// --8<-- [start:fanout-call]
app.MapPost(
    "/notices",
    async (
        MaintenanceNotice notice,
        IZLinkFanoutClient fanout,
        CancellationToken cancellationToken
    ) =>
    {
        // Delivered to every subscriber. No recipient is named.
        await fanout.Publish("broadcast", notice).Async(cancellationToken);

        return Results.Accepted();
    }
);

// --8<-- [end:fanout-call]

// --8<-- [start:spot-create-call]
app.MapPost(
    "/rooms",
    async (OpenRoom request, IZLinkSpotManager rooms, CancellationToken cancellationToken) =>
    {
        var created = await rooms
            .Create("game-room") // Picks the factory and the candidate nodes.
            .InMesh("game")
            .Request(request) // Reaches the room's create callback.
            .Async(cancellationToken);

        // From here on the room is addressed by this id alone.
        return Results.Ok(created.Spot.SpotId);
    }
);

// --8<-- [end:spot-create-call]

// --8<-- [start:spot-message-call]
// --8<-- [start:spot-send-call]
app.MapPost(
    "/rooms/{roomId}/chat",
    async (
        string roomId,
        PostChat message,
        IZLinkSpotClient rooms,
        CancellationToken cancellationToken
    ) =>
    {
        // The id is enough; the Framework resolves where the room currently runs.
        await rooms.SendToSpot(roomId, message).Async(cancellationToken);

        return Results.Accepted();
    }
);

// --8<-- [end:spot-send-call]

// --8<-- [start:spot-request-call]
app.MapGet(
    "/rooms/{roomId}",
    async (string roomId, IZLinkSpotClient rooms, CancellationToken cancellationToken) =>
    {
        var state = await rooms
            .RequestToSpot(roomId, new GetRoomState())
            .Timeout(TimeSpan.FromSeconds(3))
            .Async<RoomState>(cancellationToken);

        return Results.Ok(state);
    }
);

// --8<-- [end:spot-request-call]
// --8<-- [end:spot-message-call]

// --8<-- [start:instance-spot-call]
app.MapPost(
    "/match-queues/{mode}",
    async (
        string mode,
        JoinMatchQueue request,
        IZLinkSpotClient queues,
        CancellationToken cancellationToken
    ) =>
    {
        // No create call: the first message for this id brings the queue into being
        // and is then handled by it.
        var status = await queues
            .RequestToSpot(mode, request)
            .InstanceSpot("match-queue")
            .InMesh("game")
            .Timeout(TimeSpan.FromSeconds(3))
            .Async<MatchQueueStatus>(cancellationToken);

        return Results.Ok(status);
    }
);

// --8<-- [end:instance-spot-call]

// --8<-- [start:location-find]
// Find answers from the Location Store alone: it reports where the object is,
// and only while it is ready to receive. Nothing is sent to the object.
app.MapGet(
    "/locations/rooms/{roomId}",
    async (string roomId, IZLinkSpotManager rooms, CancellationToken cancellationToken) =>
    {
        var room = await rooms.FindAsync(roomId, cancellationToken);

        return room is null
            ? Results.NotFound()
            : Results.Ok(new { spotId = room.Value.SpotId, node = room.Value.NodeRid.ToString() });
    }
);

app.MapGet(
    "/locations/players/{playerId}",
    async (string playerId, IZLinkActorManager players, CancellationToken cancellationToken) =>
    {
        var player = await players.FindAsync(playerId, cancellationToken);

        return player is null
            ? Results.NotFound()
            : Results.Ok(
                new { actorId = player.Value.ActorId, node = player.Value.NodeRid.ToString() }
            );
    }
);

// --8<-- [end:location-find]

// --8<-- [start:actor-create-call]
app.MapPost(
    "/players/{playerId}",
    async (
        string playerId,
        CreatePlayer request,
        IZLinkActorManager players,
        CancellationToken cancellationToken
    ) =>
    {
        // GetOrCreate returns the existing player if there is one. The caller does
        // not choose which node hosts it.
        var result = await players
            .GetOrCreate(playerId, "player")
            .InMesh("game")
            .Request(request)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async(cancellationToken);

        return result switch
        {
            ZLinkActorCreateResult.Existing => Results.Ok("existing"),
            ZLinkActorCreateResult.Created => Results.Ok("created"),
            _ => Results.BadRequest("rejected"),
        };
    }
);

// --8<-- [end:actor-create-call]

// --8<-- [start:actor-message-call]
// --8<-- [start:actor-send-call]
app.MapPost(
    "/players/{playerId}/nickname",
    async (
        string playerId,
        ChangeNickname message,
        IZLinkActorClient players,
        CancellationToken cancellationToken
    ) =>
    {
        // Addressed by player id, like a room is by room id.
        await players.SendToActor(playerId, message).Async(cancellationToken);

        return Results.Accepted();
    }
);

// --8<-- [end:actor-send-call]

// --8<-- [start:actor-request-call]
app.MapGet(
    "/players/{playerId}",
    async (string playerId, IZLinkActorClient players, CancellationToken cancellationToken) =>
    {
        var info = await players
            .RequestToActor(playerId, new GetPlayer())
            .Timeout(TimeSpan.FromSeconds(3))
            .Async<PlayerInfo>(cancellationToken);

        return Results.Ok(info);
    }
);

// --8<-- [end:actor-request-call]
// --8<-- [end:actor-message-call]

// --8<-- [start:monitoring-call]
app.MapGet(
    "/status",
    (IZLinkFrameworkRuntime runtime) =>
    {
        // A snapshot taken at call time. Read it when needed rather than caching it.
        var status = runtime.Status;

        return Results.Ok(
            new
            {
                state = status.State.ToString(),
                ready = status.IsReady, // What a readiness probe would read.
                acceptingWork = status.AcceptingWork,
                safeToShutdown = status.SafeToShutdown,
                observedAt = status.ObservedAt,
            }
        );
    }
);

// --8<-- [end:monitoring-call]

await app.RunAsync();

public sealed record ImportedResponse(int Imported);
