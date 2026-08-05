// Verifies SM-D4 Multiple Actor Binding behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace SpotService.Client.Scenarios;

internal static class SmD4MultipleActorBindingScenario
{
    public static async Task RunAsync(string sessionAStreamEndpoint)
    {
        await using var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await client.Connect.Async();
        var bound = await client.Request(new MultiBindReq("actor-sm-d4-x", "actor-sm-d4-y"))
            .PacketName("MultiBindReq").Async<MultiBindRes>();
        ZlinkStreamAssert.Ensure(bound.BoundCount == 2, "SM-D4 expected two bound actors.");

        var x = await client.Request(new ActorPingReq("to-x"))
            .PacketName("ActorPingReq")
            .Metadata(SpotServiceNames.ActorIdMetadata, "actor-sm-d4-x")
            .Async<ActorPingRes>();
        var y = await client.Request(new ActorPingReq("to-y"))
            .PacketName("ActorPingReq")
            .Metadata(SpotServiceNames.ActorIdMetadata, "actor-sm-d4-y")
            .Async<ActorPingRes>();
        ZlinkStreamAssert.Ensure(x.ActorId == "actor-sm-d4-x" && x.Value == "to-x", "SM-D4 x relay mismatch.");
        ZlinkStreamAssert.Ensure(y.ActorId == "actor-sm-d4-y" && y.Value == "to-y", "SM-D4 y relay mismatch.");

        var xPushed = client.WaitFor<ActorPushNotify>()
            .Where(message => message.Payload.ActorId == "actor-sm-d4-x")
            .Async().AsTask();
        var xPushReply = await client.Request(new ActorPushReq("push-x"))
            .PacketName("ActorPushReq")
            .Metadata(SpotServiceNames.ActorIdMetadata, "actor-sm-d4-x")
            .Async<ActorPingRes>();
        var xNotify = await xPushed;
        ZlinkStreamAssert.Ensure(xPushReply.ActorId == "actor-sm-d4-x", "SM-D4 x push reply actor mismatch.");
        ZlinkStreamAssert.Ensure(xNotify.Payload.Value == "push-x", "SM-D4 x push payload mismatch.");

        var yPushed = client.WaitFor<ActorPushNotify>()
            .Where(message => message.Payload.ActorId == "actor-sm-d4-y")
            .Async().AsTask();
        var yPushReply = await client.Request(new ActorPushReq("push-y"))
            .PacketName("ActorPushReq")
            .Metadata(SpotServiceNames.ActorIdMetadata, "actor-sm-d4-y")
            .Async<ActorPingRes>();
        var yNotify = await yPushed;
        ZlinkStreamAssert.Ensure(yPushReply.ActorId == "actor-sm-d4-y", "SM-D4 y push reply actor mismatch.");
        ZlinkStreamAssert.Ensure(yNotify.Payload.Value == "push-y", "SM-D4 y push payload mismatch.");

        await ZlinkStreamAssert.ExpectFailureAsync(
            async cancellationToken => _ = await client.Request(new ActorPingReq("missing-actor-id"))
                .PacketName("ActorPingReq")
                .Timeout(TimeSpan.FromSeconds(2))
                .Async<ActorPingRes>(cancellationToken),
            nameof(ZlinkStreamErrorCode.RemoteError));
        Console.WriteLine("operation SpotService.sm-d4 passed");
    }
}
