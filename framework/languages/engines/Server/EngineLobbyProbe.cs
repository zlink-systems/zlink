using Systems.Zlink.Stream.Connector.Contracts;

namespace EngineLobby;

public static class EngineLobbyProbe
{
    public static async Task RunAsync(string endpoint)
    {
        await using var alice = CreateConnector(endpoint);
        await using var bob = CreateConnector(endpoint);

        await alice.Connect.Async();
        await bob.Connect.Async();

        var pong = await alice.Request(new Ping("1000")).Async<Pong>();
        Ensure(pong.SentAtUnixMs == "1000", "Ping/Pong payload mismatch.");

        var joinedAlice = await alice.Request(new Join("alice")).Async<Joined>();
        var joinedBob = await bob.Request(new Join("bob")).Async<Joined>();
        Ensure(joinedAlice.Name == "alice", "Alice joined with the wrong name.");
        Ensure(joinedBob.Name == "bob", "Bob joined with the wrong name.");
        Ensure(
            joinedAlice.ActorId.Length > 0 && joinedAlice.ActorId != joinedBob.ActorId,
            "Joined actor IDs must be non-empty and distinct."
        );

        var aliceChat = alice.WaitFor<ChatNotify>().Async().AsTask();
        var bobChat = bob.WaitFor<ChatNotify>().Async().AsTask();
        await alice.Send(new Chat("hello")).Async();

        var notifications = await Task.WhenAll(aliceChat, bobChat);
        foreach (var notification in notifications.Select(static value => value.Payload))
        {
            Ensure(notification.ActorId == joinedAlice.ActorId, "Chat sender ID mismatch.");
            Ensure(notification.Name == "alice", "Chat sender name mismatch.");
            Ensure(notification.Text == "hello", "Chat text mismatch.");
        }

        Console.WriteLine("engine-lobby-probe=ok");
    }

    private static IZlinkStreamConnector CreateConnector(string endpoint)
    {
        return ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(endpoint),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            }
        );
    }

    private static void Ensure(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }
}
