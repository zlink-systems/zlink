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
// Binds this connection to a player. Until then the server has no player to
// forward packets to.
var authenticated = await connector
    .Request(new Authenticate("p1"))
    .Timeout(TimeSpan.FromSeconds(5))
    .Async<Authenticated>();

Console.WriteLine($"bound player: {authenticated.PlayerId}");

// Arrange to receive the push before sending, so a fast server cannot answer
// before the client is listening.
var changed = connector.WaitFor<NicknameChanged>().Async();

// No session handler matches this packet, so the session relays it to the bound
// player, whose handler pushes the result back over this same connection.
await connector.Send(new ChangeNickname("speedy")).Async();

Console.WriteLine($"pushed: {(await changed).Payload.Nickname}");
// --8<-- [end:session-actor-client]
