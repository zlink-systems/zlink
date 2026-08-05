// Verifies SM-D12 Session Reconnect Migration behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace SpotService.Client.Scenarios;

internal static class SmD12SessionReconnectMigrationScenario
{
    public static async Task RunAsync(string sessionAStreamEndpoint, string sessionBStreamEndpoint)
    {
        const string actorId = "actor-sm-d12-transfer";

        await using var first = CreateConnector(sessionAStreamEndpoint);
        await first.Connect.Async();
        await first.Request(new AuthReq(actorId, "api transfer"))
            .PacketName("AuthReq").Async<AuthRes>();

        var firstReply = await first.Request(new ActorPingReq("before-transfer"))
            .PacketName("ActorPingReq")
            .Async<ActorPingRes>();
        ZlinkStreamAssert.Ensure(firstReply.ActorId == actorId, "SM-D12 first api actor mismatch.");
        ZlinkStreamAssert.Ensure(firstReply.NodeRid == "play-a", "SM-D12 first api node mismatch.");
        ZlinkStreamAssert.Ensure(firstReply.Seen == 1, "SM-D12 expected initial actor state.");
        await first.Close.Async();

        await using var second = CreateConnector(sessionBStreamEndpoint);
        await second.Connect.Async();
        await second.Request(new AuthReq(actorId, "api transfer"))
            .PacketName("AuthReq").Async<AuthRes>();

        var snapshot = await second.Request(new SnapshotReq(actorId))
            .PacketName("SnapshotReq")
            .Async<SnapshotRes>();
        ZlinkStreamAssert.Ensure(snapshot.ActorId == actorId, "SM-D12 snapshot actor mismatch.");
        ZlinkStreamAssert.Ensure(snapshot.Seen == 1, "SM-D12 actor state was not preserved across apis.");

        var pushed = second.WaitFor<ActorPushNotify>().Async().AsTask();
        var resumed = await second.Request(new ActorPushReq("after-transfer"))
            .PacketName("ActorPushReq")
            .Async<ActorPingRes>();
        var notify = await pushed;
        ZlinkStreamAssert.Ensure(resumed.ActorId == actorId, "SM-D12 resumed actor mismatch.");
        ZlinkStreamAssert.Ensure(resumed.NodeRid == "play-a", "SM-D12 resumed node mismatch.");
        ZlinkStreamAssert.Ensure(resumed.Seen == 2, "SM-D12 resumed actor state mismatch.");
        ZlinkStreamAssert.Ensure(notify.Payload.ActorId == actorId, "SM-D12 resumed push actor mismatch.");
        ZlinkStreamAssert.Ensure(notify.Payload.Value == "after-transfer", "SM-D12 resumed push value mismatch.");
        ZlinkStreamAssert.Ensure(notify.Payload.Seen == 2, "SM-D12 resumed push state mismatch.");
        Console.WriteLine("operation SpotService.sm-d12 passed");
    }

    private static IZlinkStreamConnector CreateConnector(string endpoint) =>
        ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(endpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
}
