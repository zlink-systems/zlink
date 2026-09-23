using Systems.Zlink.Stream.Connector.Contracts;
using Tutorial.Shared;

// --8<-- [start:stream-client]
// A game client outside the mesh. It references the connector only, never the
// Framework, and speaks to the port the stream node opened.
await using var connector = ZlinkStreamConnectorFactory.Create(
    new ZlinkStreamConnectorOptions
    {
        Endpoint = new Uri("tcp://127.0.0.1:7301"),
        ConnectTimeout = TimeSpan.FromSeconds(5),
        RequestTimeout = TimeSpan.FromSeconds(5),
        DispatchMode = ZlinkStreamDispatchMode.Immediate,
    }
);

await connector.Connect.Async();
Console.WriteLine($"connected: {connector.IsConnected}");

// A request waits for its reply. Use Send for one-way traffic; the server then
// answers with Client.Send rather than Reply.
var sentAt = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
var pong = await connector
    .Request(new Ping(sentAt.ToString()))
    .Timeout(TimeSpan.FromSeconds(5))
    .Async<Pong>();

var elapsed = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - long.Parse(pong.SentAtUnixMs);
Console.WriteLine($"round trip: {elapsed}ms");

// --8<-- [end:stream-client]

// --8<-- [start:session-actor-client]
// --8<-- [start:actor-handle-events]
using var boundNotice = connector.OnActorBound(
    (actor, _) =>
    {
        Console.WriteLine($"actor bound: {actor.ActorId}");
        return ValueTask.CompletedTask;
    }
);
using var unboundNotice = connector.OnActorUnbound(
    (actor, _) =>
    {
        Console.WriteLine($"actor unbound: {actor.ActorId}");
        return ValueTask.CompletedTask;
    }
);

// --8<-- [end:actor-handle-events]
// Binds this connection to a player. Until then the server has no player to
// forward packets to.
var authenticated = await connector
    .Request(new Authenticate("p1"))
    .Timeout(TimeSpan.FromSeconds(5))
    .Async<Authenticated>();

Console.WriteLine($"bound player: {authenticated.PlayerId}");

// --8<-- [start:actor-handle-send]
var player =
    connector.Actor(authenticated.PlayerId)
    ?? throw new InvalidOperationException("Player Actor was not bound");
Console.WriteLine($"actor handle: {player.ActorId}");

// --8<-- [end:actor-handle-send]

// Arrange to receive the push before sending, so a fast server cannot answer
// before the client is listening.
var changed = connector.WaitFor<NicknameChanged>().Async();

// The handle addresses this player directly. Its handler pushes the result
// back over the same connection.
// --8<-- [start:actor-handle-send-call]
await player.Send(new ChangeNickname("speedy")).Async();

// --8<-- [end:actor-handle-send-call]

// --8<-- [start:actor-handle-receive]
var pushed = await changed;
Console.WriteLine($"pushed: {pushed.Payload.Nickname}, actor: {pushed.ActorId}");
// --8<-- [end:actor-handle-receive]
// --8<-- [end:session-actor-client]
