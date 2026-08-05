// Verifies SM-D10 Bounded Session Backpressure behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace SpotService.Client.Scenarios;

internal static class SmD10BoundedSessionBackpressureScenario
{
    public static async Task RunAsync(string sessionAStreamEndpoint, string sessionBStreamEndpoint)
    {
        IZlinkStreamConnector? congested = null;
        IZlinkStreamConnector? isolated = null;
        try
        {
            congested = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(sessionAStreamEndpoint),
                ConnectTimeout = TimeSpan.FromSeconds(5),
                RequestTimeout = TimeSpan.FromSeconds(5),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                MaxReceivedMessages = 1
            });
            await congested.Connect.Async();
        await congested.Request(new AuthReq("actor-sm-d10-congested", "stream backpressure"))
                .PacketName("AuthReq")
                .Async<AuthRes>();

            isolated = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(sessionBStreamEndpoint),
                ConnectTimeout = TimeSpan.FromSeconds(5),
                RequestTimeout = TimeSpan.FromSeconds(5),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                MaxReceivedMessages = 1024
            });
            await isolated.Connect.Async();
        await isolated.Request(new AuthReq("actor-sm-d10-isolated", "stream backpressure peer"))
                .PacketName("AuthReq")
                .Async<AuthRes>();

            for (var index = 0; index < 8; index++)
            {
                var reply = await congested.Request(new ActorPushReq($"burst-{index}"))
                    .PacketName("ActorPushReq")
                    .Async<ActorPingRes>();
                ZlinkStreamAssert.Ensure(reply.ActorId == "actor-sm-d10-congested",
                    "SM-D10 congested reply actor mismatch.");
            }

            ZlinkStreamAssert.Ensure(
                congested.ReceivedCount("ActorPushNotify") <= 1,
                "SM-D10 expected bounded received-message queue for congested session.");
            var retained = await congested.WaitFor<ActorPushNotify>()
                .Where(message => message.Payload.ActorId == "actor-sm-d10-congested")
                .Timeout(TimeSpan.FromSeconds(2))
                .Async();
            ZlinkStreamAssert.Ensure(retained.Payload.Value == "burst-0",
                "SM-D10 expected the first accepted push to remain while newer pushes were rejected.");

            var stillAlive = await congested.Request(new ActorPingReq("after-backpressure"))
                .PacketName("ActorPingReq")
                .Async<ActorPingRes>();
            ZlinkStreamAssert.Ensure(stillAlive.ActorId == "actor-sm-d10-congested",
                "SM-D10 congested session stopped routing.");
            ZlinkStreamAssert.Ensure(stillAlive.Value == "after-backpressure", "SM-D10 congested session reply mismatch.");

            var isolatedPush = isolated.WaitFor<ActorPushNotify>().Async().AsTask();
            var isolatedReply = await isolated.Request(new ActorPushReq("isolated-push"))
                .PacketName("ActorPushReq")
                .Async<ActorPingRes>();
            var isolatedNotify = await isolatedPush;
            ZlinkStreamAssert.Ensure(isolatedReply.ActorId == "actor-sm-d10-isolated",
                "SM-D10 isolated reply actor mismatch.");
            ZlinkStreamAssert.Ensure(isolatedNotify.Payload.ActorId == "actor-sm-d10-isolated",
                "SM-D10 isolated push actor mismatch.");
            ZlinkStreamAssert.Ensure(isolatedNotify.Payload.Value == "isolated-push",
                "SM-D10 isolated session push mismatch.");
        }
        finally
        {
            if (congested is not null) await congested.DisposeAsync();
            if (isolated is not null) await isolated.DisposeAsync();
        }

        Console.WriteLine("operation SpotService.sm-d10 passed");
    }
}
