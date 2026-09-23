using EngineLobby.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace EngineLobby.Server;

public static class EngineLobbyProbe
{
    public static async Task RunAsync(string endpoint)
    {
        await using var alice = CreateConnector(endpoint);
        await using var bob = CreateConnector(endpoint);

        await alice.Connect.Async();
        await bob.Connect.Async();

        var pong = await alice.Request(new PingReq("1000")).Async<PingRes>();
        Ensure(pong.SentAtUnixMs == "1000", "PingReq/PingRes payload mismatch.");

        var joinedAlice = await alice.Request(new JoinReq("alice")).Async<JoinRes>();
        var joinedBob = await bob.Request(new JoinReq("bob")).Async<JoinRes>();
        Ensure(joinedAlice.Name == "alice", "Alice joined with the wrong name.");
        Ensure(joinedBob.Name == "bob", "Bob joined with the wrong name.");
        Ensure(
            joinedAlice.ActorId.Length > 0 && joinedAlice.ActorId != joinedBob.ActorId,
            "JoinRes actor IDs must be non-empty and distinct."
        );

        var aliceChat = alice.WaitFor<ChatNotify>().Async().AsTask();
        var bobChat = bob.WaitFor<ChatNotify>().Async().AsTask();
        await alice.Send(new ChatMsg("hello")).Async();

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
