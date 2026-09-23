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
// With one Actor bound, the connector can send without an Actor handle.
var authenticated1 = await connector
    .Request(new Authenticate("p1"))
    .Timeout(TimeSpan.FromSeconds(5))
    .Async<Authenticated>();
Console.WriteLine($"bound player: {authenticated1.PlayerId}");

// --8<-- [start:single-actor-send]
var singleChanged = new TaskCompletionSource<ZlinkStreamMessage<NicknameChanged>>(
    TaskCreationOptions.RunContinuationsAsynchronously
);
using (
    connector.On<NicknameChanged>(
        (message, _) =>
        {
            singleChanged.SetResult(message);
            return ValueTask.CompletedTask;
        }
    )
)
{
    await connector.Send(new ChangeNickname("speedy")).Async();
    var pushed = await singleChanged.Task;
    Console.WriteLine($"pushed: {pushed.Payload.Nickname}, actor: {pushed.ActorId}");
}

// --8<-- [end:single-actor-send]

// A second Actor on the same connection calls for explicit handles.
var authenticated2 = await connector
    .Request(new Authenticate("p2"))
    .Timeout(TimeSpan.FromSeconds(5))
    .Async<Authenticated>();
Console.WriteLine($"bound player: {authenticated2.PlayerId}");

// --8<-- [start:actor-handle-send]
var player1 =
    connector.Actor(authenticated1.PlayerId)
    ?? throw new InvalidOperationException("Player Actor p1 was not bound.");
var player2 =
    connector.Actor(authenticated2.PlayerId)
    ?? throw new InvalidOperationException("Player Actor p2 was not bound.");
Console.WriteLine($"actor handle: {player1.ActorId}");
Console.WriteLine($"actor handle: {player2.ActorId}");

// --8<-- [end:actor-handle-send]

// --8<-- [start:actor-handle-per-handle-receive]
// Each callback receives only the push for its handle's Actor.
var changed1 = new TaskCompletionSource<ZlinkStreamMessage<NicknameChanged>>(
    TaskCreationOptions.RunContinuationsAsynchronously
);
var changed2 = new TaskCompletionSource<ZlinkStreamMessage<NicknameChanged>>(
    TaskCreationOptions.RunContinuationsAsynchronously
);
using var receive1 = player1.On<NicknameChanged>(
    (message, _) =>
    {
        changed1.SetResult(message);
        return ValueTask.CompletedTask;
    }
);
using var receive2 = player2.On<NicknameChanged>(
    (message, _) =>
    {
        changed2.SetResult(message);
        return ValueTask.CompletedTask;
    }
);

// --8<-- [end:actor-handle-per-handle-receive]

// --8<-- [start:actor-id-receive]
// Connector-level callbacks can distinguish the same pushes by ActorId.
using var receiveActorIds = connector.On<NicknameChanged>(
    (message, _) =>
    {
        Console.WriteLine($"received actor id: {message.ActorId}");
        return ValueTask.CompletedTask;
    }
);

// --8<-- [end:actor-id-receive]

// Each handle sends to its own player over the same connection.
// --8<-- [start:actor-handle-send-call]
await player1.Send(new ChangeNickname("speedy-p1")).Async();
await player2.Send(new ChangeNickname("speedy-p2")).Async();

// --8<-- [end:actor-handle-send-call]

// --8<-- [start:actor-handle-receive]
var pushed1 = await changed1.Task;
var pushed2 = await changed2.Task;
Console.WriteLine($"pushed: {pushed1.Payload.Nickname}, actor: {pushed1.ActorId}");
Console.WriteLine($"pushed: {pushed2.Payload.Nickname}, actor: {pushed2.ActorId}");
// --8<-- [end:actor-handle-receive]
// --8<-- [end:session-actor-client]
